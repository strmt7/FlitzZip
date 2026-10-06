"""Offline contracts for pinned source admission and reproducible standard wheels."""

from __future__ import annotations

import base64
import csv
import hashlib
import io
import tempfile
import unittest
import warnings
import zipfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from tools import nltk_security_build as build


class SecurityBuildTests(unittest.TestCase):
    """Verify artifact boundaries without installing packages or invoking a build."""

    def test_original_source_archive_is_immutable_and_license_complete(self):
        """Purpose: Retain provenance and rights. Inputs: Tracked source archive. Outputs: Hash/license assertions."""
        data = build.recipe()
        with zipfile.ZipFile(build.SOURCE_ROOT / data["source"]) as source:
            prefix = f"nltk-{data['commit']}/"
            self.assertEqual(source.read(prefix + "nltk/VERSION").decode().strip(), data["upstream_version"])
            for name in ("LICENSE.txt", "AUTHORS.md", "README.md"):
                self.assertGreater(len(source.read(prefix + name)), 100)
        self.assertIn("superzip.security", data["version"])
        self.assertIn("GHSA-8mgp-746c-j5xp", data["advisories"])

    def test_digest_and_source_tampering_refused(self):
        """Purpose: Refuse substituted provenance. Inputs: Malformed digest or wrong archive. Outputs: Errors."""
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp, "source.sha256")
            for bad in ("", "garbage file.zip", hashlib.sha256(b"fixture").hexdigest()):
                path.write_text(bad, encoding="ascii")
                with self.assertRaises(ValueError):
                    build.expected_digest(path)
        with (
            patch.object(build, "digest", return_value=hashlib.sha256(b"different archive").hexdigest()),
            self.assertRaisesRegex(ValueError, "archive digest"),
        ):
            build.recipe()

    def test_extraction_rejects_escape_symlink_and_duplicates_before_publication(self):
        """Purpose: Bound source extraction. Inputs: Malformed owned ZIP fixtures. Outputs: No escaped writes."""
        commit = build.recipe()["commit"]
        prefix = f"nltk-{commit}/"
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            for index, bad in enumerate((prefix + "../escaped", "/absolute", prefix + "drive:stream")):
                archive = directory / f"bad-{index}.zip"
                with zipfile.ZipFile(archive, "w") as source:
                    source.writestr(bad, "owned fixture")
                with self.assertRaises(ValueError):
                    build.extract_source(archive, directory / f"target-{index}", commit)
            archive = directory / "link.zip"
            with zipfile.ZipFile(archive, "w") as source:
                member = zipfile.ZipInfo(prefix + "link")
                member.external_attr = 0o120777 << 16
                source.writestr(member, "../escaped")
            with self.assertRaises(ValueError):
                build.extract_source(archive, directory / "target-link", commit)
            archive = directory / "duplicate.zip"
            with zipfile.ZipFile(archive, "w") as source, warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)
                source.writestr(prefix + "duplicate", "first")
                source.writestr(prefix + "duplicate", "second")
            with self.assertRaises(ValueError):
                build.extract_source(archive, directory / "target-duplicate", commit)
            self.assertFalse((directory / "escaped").exists())

    def test_owned_packaging_preserves_runtime_source_and_original_notices(self):
        """Purpose: Separate identity from code repairs. Inputs: Owned upstream tree. Outputs: Exact runtime bytes."""
        data = build.recipe()
        with tempfile.TemporaryDirectory() as temp:
            source = build.extract_source(build.SOURCE_ROOT / data["source"], Path(temp), data["commit"])
            targets = ("nltk/pathsec.py", "nltk/tag/perceptron.py", "nltk/parse/transitionparser.py", "LICENSE.txt")
            before = {name: (source / name).read_bytes() for name in targets}
            build.identify_build(source, data)
            self.assertEqual(before, {name: (source / name).read_bytes() for name in targets})
            self.assertEqual((source / "nltk/VERSION").read_text().strip(), data["version"])
            self.assertIn("not a published NLTK release", (source / "SUPERZIP_BUILD_NOTICE.txt").read_text())
            self.assertIn("SUPERZIP_BUILD_NOTICE.txt", (source / "setup.cfg").read_text())

    def test_installed_runtime_binds_original_sources_without_import_or_version_fallback(self):
        """Purpose: Reject installed source drift. Inputs: Original owned tree and mutations. Outputs: Exact refusal."""
        data = build.recipe()
        expected = build.runtime_source_sha256()
        with tempfile.TemporaryDirectory() as temp:
            source = build.extract_source(build.SOURCE_ROOT / data["source"], Path(temp), data["commit"])
            build.identify_build(source, data)
            root = source / "nltk"
            distribution = SimpleNamespace(locate_file=lambda name: source / name)
            with patch("importlib.metadata.distribution", return_value=distribution):
                build.verify_installed_runtime(expected)
                for name in (
                    "pathsec.py",
                    "picklesec.py",
                    "data.py",
                    "tag/perceptron.py",
                    "parse/transitionparser.py",
                    "classify/maxent.py",
                    "VERSION",
                    "__init__.py",
                ):
                    path = root / name
                    original = path.read_bytes()
                    path.write_bytes(b"raise RuntimeError('integrity probe must never import package code')\n")
                    with self.assertRaisesRegex(ValueError, "runtime source changed"):
                        build.verify_installed_runtime(expected)
                    path.write_bytes(original)
                for name in ("unexpected.py", "unexpected.PY"):
                    extra = root / name
                    extra.write_text("# unexpected installed source\n", encoding="utf-8")
                    with self.assertRaisesRegex(ValueError, "runtime source changed"):
                        build.verify_installed_runtime(expected)
                    extra.unlink()
                for suffix in (".pyd", ".so", ".dll", ".dylib"):
                    extra = root / ("unexpected" + suffix)
                    extra.write_bytes(b"inert native file fixture")
                    with self.assertRaisesRegex(ValueError, "native executable file"):
                        build.verify_installed_runtime(expected)
                    extra.unlink()
                for name in ("unexpected.pyc", "unexpected.PYC", "unexpected.pyo", "tag/unexpected.pyc"):
                    extra = root / name
                    extra.write_bytes(b"inert sourceless file fixture")
                    with self.assertRaisesRegex(ValueError, "sourceless bytecode"):
                        build.verify_installed_runtime(expected)
                    extra.unlink()
                cache = root / "__pycache__"
                cache.mkdir()
                (cache / "__init__.cpython-313.pyc").write_bytes(b"preserved cache fixture")
                build.verify_installed_runtime(expected)
                missing = root / "tag/perceptron.py"
                original = missing.read_bytes()
                missing.unlink()
                with self.assertRaisesRegex(ValueError, "runtime source changed"):
                    build.verify_installed_runtime(expected)
                missing.write_bytes(original)
                build.verify_installed_runtime(expected)
                with self.assertRaisesRegex(ValueError, "Malformed NLTK runtime"):
                    build.verify_installed_runtime("unreviewed")
                with patch.object(build, "MAX_SOURCE_BYTES", 1), self.assertRaisesRegex(ValueError, "byte budget"):
                    build.verify_installed_runtime(expected)

    def test_source_metadata_rejects_case_alias_and_normalized_names_before_writes(self):
        """Purpose: Preserve source identity. Inputs: Ambiguous ZIPs. Outputs: Refusal before extraction."""
        commit = build.recipe()["commit"]
        prefix = f"nltk-{commit}/"
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for index, names in enumerate((("a.py", "A.py"), ("first.py", "folder//second.py"))):
                archive = root / f"alias-{index}.zip"
                with zipfile.ZipFile(archive, "w") as source:
                    for name in names:
                        source.writestr(prefix + name, b"# owned source\n")
                target = root / f"target-{index}"
                with self.assertRaisesRegex(ValueError, "Unsafe or duplicate"):
                    build.extract_source(archive, target, commit)
                self.assertFalse(target.exists())
            for kind in (0o010000, 0o020000, 0o060000, 0o040000):
                archive = root / f"special-{kind}.zip"
                with zipfile.ZipFile(archive, "w") as source:
                    member = zipfile.ZipInfo(prefix + "model.py")
                    member.external_attr = kind << 16
                    source.writestr(member, b"# inadmissible source type\n")
                target = root / f"target-special-{kind}"
                with self.assertRaisesRegex(ValueError, "Unsafe or duplicate"):
                    build.extract_source(archive, target, commit)
                self.assertFalse(target.exists())

    def test_canonical_wheel_is_host_independent_and_record_valid(self):
        """Purpose: Preserve standard wheel integrity. Inputs: Equivalent platform ZIPs. Outputs: Equal valid wheels."""
        data = build.recipe()
        info = f"nltk-{data['version']}.dist-info/"
        payloads = {
            "nltk/model.py": b"# Original runtime bytes\nvalue = 1\n",
            info + "METADATA": f"Name: nltk\nVersion: {data['version']}\n\n".encode(),
            info + "RECORD": b"upstream-generated-placeholder\n",
            info + "licenses/LICENSE.txt": b"original license fixture\n",
        }
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            outputs = []
            for index in (0, 1):
                original = directory / f"built-{index}.whl"
                with zipfile.ZipFile(original, "w") as wheel:
                    for name, contents in payloads.items() if index == 0 else reversed(list(payloads.items())):
                        member = zipfile.ZipInfo(name, (2020 + index, 1, 1, 0, 0, 0))
                        member.create_system = index
                        if index and name.endswith("METADATA"):
                            contents = contents.replace(b"\n", b"\r\n")
                        wheel.writestr(member, contents)
                output = directory / f"canonical-{index}.whl"
                build.canonical_wheel(original, output, data)
                outputs.append(output)
            self.assertEqual(outputs[0].read_bytes(), outputs[1].read_bytes())
            with zipfile.ZipFile(outputs[0]) as wheel:
                self.assertEqual(wheel.read("nltk/model.py"), payloads["nltk/model.py"])
                rows = csv.reader(io.StringIO(wheel.read(info + "RECORD").decode()))
                for name, value, size in rows:
                    if name.endswith("/RECORD"):
                        self.assertEqual((value, size), ("", ""))
                        continue
                    contents = wheel.read(name)
                    expected = base64.urlsafe_b64encode(hashlib.sha256(contents).digest()).rstrip(b"=").decode()
                    self.assertEqual(value, "sha256=" + expected)
                    self.assertEqual(int(size), len(contents))

    def test_cached_wheel_tampering_refuses_installation(self):
        """Purpose: Fail closed on cache corruption. Inputs: Substituted wheel bytes. Outputs: Refusal/no build."""
        data = build.recipe()
        with tempfile.TemporaryDirectory() as temp:
            home = Path(temp)
            directory = home / ("nltk-wheel-" + build.identity()[:16])
            directory.mkdir()
            (directory / f"nltk-{data['version']}-py3-none-any.whl").write_bytes(b"substituted")
            with patch.object(build, "build_wheel") as builder, self.assertRaisesRegex(ValueError, "wheel digest"):
                build.ensure_wheel(home)
            builder.assert_not_called()

    def test_hosted_scanners_discover_nonstandard_manifest_names(self):
        """Purpose: Prevent false empty dependency scans. Inputs: Owned scanners. Outputs: Explicit graph discovery."""
        scanner = (build.ROOT / ".github/workflows/security-code-scanning.yml").read_text(encoding="utf-8")
        for name in ("crawl4ai", "nltk-build"):
            self.assertIn(f"--lockfile=requirements.txt:tools/requirements/{name}.txt", scanner)
        dependabot = (build.ROOT / ".github/dependabot.yml").read_text(encoding="utf-8")
        self.assertIn("directory: /tools/requirements", dependabot)


if __name__ == "__main__":
    unittest.main()
