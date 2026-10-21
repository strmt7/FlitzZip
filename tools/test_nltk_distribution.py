"""Official NLTK distribution and shared pure-wheel artifact contracts."""

from __future__ import annotations

import base64
import csv
import hashlib
import io
import json
import os
import shutil
import stat
import subprocess
import tempfile
import unittest
import warnings
import zipfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from tools import crawl4ai_source_build, semgrep_distribution
from tools import nltk_distribution as build
from tools import python_wheel as packaging


def source_fixture(path: Path, data: dict) -> dict:
    """Purpose: Exercise admission offline. Inputs: Owned path/provenance shape. Outputs: Inert wheel and digest."""
    prefix = f"nltk-{data['version']}.dist-info/"
    members = {
        "nltk/VERSION": data["version"].encode(),
        prefix + "METADATA": f"Name: nltk\nVersion: {data['version']}\n\n".encode(),
        prefix + "WHEEL": b"Wheel-Version: 1.0\nRoot-Is-Purelib: true\nTag: py3-none-any\n\n",
        prefix + "licenses/LICENSE.txt": (build.ROOT / "docs/licenses/NLTK-3.10.4.txt").read_bytes(),
        prefix + "licenses/AUTHORS.md": b"Owned author-notice fixture.\n" * 5,
    }
    for name in (
        "__init__.py",
        "pathsec.py",
        "picklesec.py",
        "data.py",
        "tag/perceptron.py",
        "parse/transitionparser.py",
        "classify/maxent.py",
    ):
        members["nltk/" + name] = b"raise RuntimeError('source admission must not import this fixture')\n"
    with zipfile.ZipFile(path, "w") as wheel:
        for name, contents in members.items():
            wheel.writestr(name, contents)
    return data | {"sha256": packaging.digest(path), "size": path.stat().st_size}


class DistributionTests(unittest.TestCase):
    """Preserve upstream identity, complete source admission and portable standard packaging."""

    # Purpose: Reject contradictory or non-JSON provenance before a recipe can admit or build dependency bytes.
    # Inputs: Actual NLTK, Crawl4AI and Semgrep recipes with duplicate, nonfinite, untyped or oversized metadata.
    # Outputs: Every consumer raises ValueError without including private input values in its diagnostic.
    def test_all_packaging_recipes_reject_ambiguous_metadata(self):
        for module, reader, attribute, filename in (
            (build, build.distribution, "SOURCE_ROOT", "distribution.json"),
            (crawl4ai_source_build, crawl4ai_source_build.recipe, "SOURCE_ROOT", "build.json"),
            (semgrep_distribution, semgrep_distribution.recipe, "MANIFEST", None),
        ):
            original = reader()
            literal = json.dumps(original)
            private_value = "private-recipe-value-do-not-log"
            variants = {
                "repeated-schema": literal[:-1] + ',"schema":1}',
                "nested-duplicate": literal[:-1] + ',"extra":{"pin":1,"pin":2}}',
                "nonfinite": literal[:-1] + ',"extra":NaN}',
                "overflow": literal[:-1] + ',"extra":1e999}',
                "fractional-schema": json.dumps(original | {"schema": 1.0}),
                "boolean-schema": json.dumps(original | {"schema": True}),
                "scalar": '"' + private_value + '"',
                "array": "[]",
                "empty": "",
                "malformed": '{"schema":',
                "oversized": literal[:-1] + ',"extra":"' + "x" * (1024 * 1024) + '"}',
            }
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                record = root / (filename or "semgrep.json")
                for case, contents in variants.items():
                    record.write_text(contents, encoding="utf-8")
                    with self.subTest(project=original.get("project", "semgrep"), case=case):
                        with (
                            patch.object(module, attribute, root if filename else record),
                            self.assertRaises(ValueError) as failure,
                        ):
                            reader()
                        self.assertNotIn(private_value, str(failure.exception))

    # Purpose: Preserve the exact metadata byte ceiling without making a prior stat result authoritative.
    # Inputs: One complete JSON recipe padded to the boundary, then one excess byte and invalid UTF-8.
    # Outputs: Exact bounded metadata is accepted unchanged; oversized or undecodable bytes fail.
    def test_packaging_recipe_exact_byte_boundary(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "recipe.json"
            document = b'{"schema":1}'
            payload = document + b" " * (packaging.MAX_RECIPE_BYTES - len(document))
            path.write_bytes(payload)
            self.assertEqual(packaging.read_recipe(path), {"schema": 1})
            for invalid in (payload + b" ", b'{"private":"\xff"}'):
                path.write_bytes(invalid)
                with self.subTest(bytes=len(invalid)), self.assertRaises(ValueError):
                    packaging.read_recipe(path)

    # Purpose: Reject real redirected cache roots and installed-package entries without traversing their targets.
    # Inputs: One owned Windows junction or POSIX link to the inert wheel cache and an isolated alternate tool home.
    # Outputs: Cache, package-root and child-entry admission all refuse the redirect; target bytes remain intact.
    def test_real_redirects_fail_all_package_boundaries(self):
        home = self.root / "redirect-home"
        parent = home / "wheels"
        parent.mkdir(parents=True)
        link = parent / ("nltk-" + self.data["sha256"])
        target = self.cached.parent
        if os.name == "nt":
            subprocess.run(
                ["cmd.exe", "/d", "/c", "mklink", "/J", str(link), str(target)],
                check=True,
                capture_output=True,
                timeout=30,
            )
        else:
            link.symlink_to(target, target_is_directory=True)
        try:
            with (
                patch("tools.crawl4ai_tool.cache_home", return_value=home),
                self.assertRaisesRegex(ValueError, "wheel cache is redirected"),
            ):
                build.wheel_path(home)
            with self.assertRaisesRegex(ValueError, "missing or redirected"):
                build.installed_runtime_sha256(link)
            with self.assertRaisesRegex(ValueError, "contains a redirect"):
                build.installed_runtime_sha256(parent)
        finally:
            if os.name == "nt":
                link.rmdir()
            else:
                link.unlink()
        self.assertEqual(packaging.digest(self.cached), self.data["sha256"])

    # Purpose: Require the runtime's entry source in addition to version metadata when computing source identity.
    # Inputs: A real inert wheel with its package entry source removed while retaining the remaining payload.
    # Outputs: Runtime identity refuses an incomplete package rather than producing an apparently valid digest.
    def test_runtime_identity_requires_complete_source(self):
        files = packaging.wheel_files(self.wheel)
        del files["nltk/__init__.py"]
        mutant = self.root / "incomplete-runtime.whl"
        with zipfile.ZipFile(mutant, "w") as archive:
            for name, payload in files.items():
                archive.writestr(name, payload)
        with patch.object(build, "wheel_path", return_value=mutant), self.assertRaisesRegex(ValueError, "incomplete"):
            build.runtime_source_sha256()

    # Purpose: Reject ambiguous package identities before canonical packaging or source admission can select one header.
    # Inputs: Tiny inert wheels with missing, conflicting, repeated, incorrect or undecodable identity headers.
    # Outputs: Both consumers reject each malformed identity with ValueError and never publish a canonical wheel.
    def test_metadata_identity_is_single_and_unambiguous(self):
        files = packaging.wheel_files(self.wheel)
        member = f"nltk-{self.data['version']}.dist-info/METADATA"
        version = self.data["version"]
        variants = (
            f"Version: {version}\n\n",
            "Name: nltk\n\n",
            f"Name: nltk\nName: other\nVersion: {version}\n\n",
            f"Name: nltk\nName: nltk\nVersion: {version}\n\n",
            f"Name: nltk\nVersion: {version}\nVersion: other\n\n",
            f"Name: other\nVersion: {version}\n\n",
            b"Name: \xff\nVersion: " + version.encode("ascii") + b"\n\n",
        )
        mutant, output = self.root / "identity-variant.whl", self.root / "canonical.whl"
        for metadata in variants:
            replacement = metadata if isinstance(metadata, bytes) else metadata.encode("ascii")
            with zipfile.ZipFile(mutant, "w") as archive:
                for name, content in files.items():
                    archive.writestr(name, replacement if name == member else content)
            data = self.data | {"size": mutant.stat().st_size, "sha256": packaging.digest(mutant)}
            with self.subTest(metadata=metadata):
                with self.assertRaises(ValueError):
                    packaging.canonical_wheel(mutant, output, data, project="nltk")
                self.assertFalse(output.exists())
                with self.assertRaises(ValueError):
                    build.verify_wheel(mutant, data)

    # Purpose: Admit source-artifact extents before reading or expanding even an otherwise valid ZIP.
    # Inputs: An owned tiny wheel with a test-only budget one byte below its actual extent.
    # Outputs: Both digesting and wheel parsing reject the extent before consuming its payload.
    def test_shared_artifact_extent_admission(self):
        with patch.object(packaging, "MAX_SOURCE_BYTES", self.wheel.stat().st_size - 1):
            with self.assertRaisesRegex(ValueError, "artifact exceeds"):
                packaging.digest(self.wheel)
            with self.assertRaisesRegex(ValueError, "wheel exceeds"):
                packaging.wheel_files(self.wheel)
        directory_limited = self.root / "entry-budget.whl"
        for count in (2000, 2001):
            with zipfile.ZipFile(directory_limited, "w") as archive:
                for index in range(count):
                    archive.writestr(f"owned-empty-fixture/part-{index:04}.txt", b"")
            if count == 2000:
                self.assertEqual(len(packaging.wheel_files(directory_limited)), count)
            else:
                with self.assertRaisesRegex(ValueError, "wheel exceeds"):
                    packaging.wheel_files(directory_limited)

    def test_shared_wheel_paths_refuse_redirects_and_replacement(self):
        """Purpose: Keep package admission and publication on owned regular files.
        Inputs: Reparse metadata and occupied output. Outputs: Refusal without mutation.
        """
        redirected = SimpleNamespace(
            st_mode=stat.S_IFREG,
            st_size=self.wheel.stat().st_size,
            st_file_attributes=stat.FILE_ATTRIBUTE_REPARSE_POINT,
        )
        with patch.object(Path, "lstat", return_value=redirected):
            with self.assertRaisesRegex(ValueError, "redirected"):
                packaging.digest(self.wheel)
            with self.assertRaisesRegex(ValueError, "redirected"):
                packaging.wheel_files(self.wheel)
        output = self.root / "occupied-canonical.whl"
        sentinel = b"preserve existing artifact"
        output.write_bytes(sentinel)
        data = {"version": self.data["version"], "source_date_epoch": 1704067200}
        with self.assertRaises(FileExistsError):
            packaging.canonical_wheel(self.wheel, output, data, project="nltk")
        self.assertEqual(output.read_bytes(), sentinel)

    def setUp(self):
        """Purpose: Keep contracts offline. Inputs: Owned fixture. Outputs: Isolated asset/cache and cleanup."""
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        original = build.distribution()
        self.wheel = self.root / original["filename"]
        self.data = source_fixture(self.wheel, original)
        record = self.root / "record"
        record.mkdir()
        (record / "distribution.json").write_text(json.dumps(self.data), encoding="utf-8")
        self.enterContext(patch.object(build, "SOURCE_ROOT", record))
        self.home = self.root / "cache"
        self.enterContext(patch("tools.crawl4ai_tool.cache_home", return_value=self.home))
        cached = self.home / "wheels" / ("nltk-" + self.data["sha256"]) / self.wheel.name
        cached.parent.mkdir(parents=True)
        shutil.copyfile(self.wheel, cached)
        self.cached = cached

    def test_admission_retains_version_license_and_authors(self):
        """Purpose: Avoid fabricated identities. Inputs: Provenance and inert wheel. Outputs: Matching notices."""
        data = build.distribution()
        self.assertEqual(data["version"], "3.10.4")
        self.assertEqual(data["tag"], "v3.10.4-rc1")
        self.assertEqual(data["release_kind"], "release_candidate")
        self.assertIn("GHSA-8mgp-746c-j5xp", data["advisories"])
        files = packaging.wheel_files(build.wheel_path())
        self.assertEqual(files["nltk/VERSION"].decode().strip(), data["version"])
        self.assertEqual(
            files["nltk-3.10.4.dist-info/licenses/LICENSE.txt"],
            (build.ROOT / "docs/licenses/NLTK-3.10.4.txt").read_bytes(),
        )
        self.assertGreater(len(files["nltk-3.10.4.dist-info/licenses/AUTHORS.md"]), 100)
        self.assertFalse(any("FLITZZIP_BUILD_NOTICE" in name for name in files))

    def test_cache_identity_binds_recipe_text_and_artifact_digest(self):
        """Purpose: Invalidate cached environments after provenance or wheel changes.
        Inputs: Controlled recipe identity and admitted wheel digest. Outputs: Exact composite SHA-256 identity.
        """
        recipe_identity = "a" * 64
        expected = hashlib.sha256((recipe_identity + self.data["sha256"]).encode("ascii")).hexdigest()
        with patch("tools.crawl4ai_tool.text_identity", return_value=recipe_identity):
            self.assertEqual(build.identity(), expected)

    def test_installed_runtime_rejects_special_files_on_every_host(self):
        """Purpose: Keep device, socket and pipe entries outside the admitted Python runtime.
        Inputs: Modeled special directory entry. Outputs: Refusal before opening or hashing it.
        """
        root = self.root / "modeled-runtime"
        root.mkdir()
        child = SimpleNamespace(
            path=str(root / "special-entry"),
            stat=lambda follow_symlinks=False: SimpleNamespace(st_mode=stat.S_IFIFO, st_file_attributes=0),
        )

        class Scan:
            """Purpose: Model one scandir page. Inputs: Context entry. Outputs: One inert special-file record."""

            def __enter__(self):
                """Purpose: Enter modeled enumeration. Inputs: None. Outputs: Iterator over one record."""
                return iter((child,))

            def __exit__(self, exc_type, exc_value, traceback):
                """Purpose: Close modeled enumeration. Inputs: Exit state. Outputs: No exception suppression."""
                return False

        with patch.object(build.os, "scandir", return_value=Scan()), self.assertRaisesRegex(ValueError, "special file"):
            build.installed_runtime_sha256(root)

    def test_provenance_and_wheel_tampering_refuse_installation(self):
        """Purpose: Refuse substituted packages. Inputs: Changed metadata/bytes. Outputs: Installation refusal."""
        data = build.distribution()
        wheel = build.wheel_path()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            shutil.copyfile(wheel, root / wheel.name)
            record = root / "distribution.json"
            with patch.object(build, "SOURCE_ROOT", root):
                for changes in (
                    {"schema": True},
                    {"project": "another"},
                    {"version": None},
                    {"tag": None},
                    {"filename": "../wheel.whl"},
                    {"version": "3.10.3"},
                    {"url": "https://example.invalid/wheel.whl"},
                    {"release_kind": "stable"},
                    {"size": True},
                    {"sha256": "not a digest"},
                    {"sha256": None},
                    {"size": data["size"] + 1},
                    {"sha256": hashlib.sha256(b"substituted").hexdigest()},
                ):
                    record.write_text(json.dumps(data | changes), encoding="utf-8")
                    with self.subTest(changes=changes), self.assertRaises(ValueError):
                        build.verify_wheel(wheel, build.distribution())
                record.write_text(json.dumps(data), encoding="utf-8")
                self.assertEqual(build.verify_wheel(root / wheel.name, build.distribution()), root / wheel.name)
                (root / wheel.name).write_bytes(b"substituted")
                with self.assertRaisesRegex(ValueError, "digest or size"):
                    build.verify_wheel(root / wheel.name, build.distribution())

    def test_cache_download_is_hash_locked_atomic_and_reused(self):
        """Purpose: Protect first-use publication. Inputs: Cold cache and pip boundary. Outputs: Exact reuse."""
        self.cached.unlink()

        def download(command, env, timeout):
            """Purpose: Observe artifact-only transport. Inputs: Pip argv/env. Outputs: Owned inert asset."""
            self.assertEqual(command[1:4], ["-m", "pip", "download"])
            for flag in ("--require-hashes", "--only-binary=:all:", "--no-deps"):
                self.assertIn(flag, command)
            self.assertEqual(timeout, 180)
            self.assertEqual(env["FLITZZIP_CRAWL4AI_HOME"], str(self.home))
            request = Path(command[command.index("-r") + 1]).read_text(encoding="utf-8")
            self.assertEqual(request, f"nltk @ {self.data['url']} --hash=sha256:{self.data['sha256']}\n")
            destination = Path(command[command.index("--dest") + 1])
            self.assertFalse(self.cached.exists())
            shutil.copyfile(self.wheel, destination / self.wheel.name)

        with patch("tools.crawl4ai_tool.run", side_effect=download) as transport:
            self.assertEqual(build.wheel_path(self.home), self.cached)
            self.assertEqual(build.wheel_path(self.home), self.cached)
            self.assertEqual(transport.call_count, 1)
        self.assertFalse(list(self.cached.parent.glob("download-*")))
        self.cached.write_bytes(b"corrupt cache")
        with patch("tools.crawl4ai_tool.run") as transport, self.assertRaisesRegex(ValueError, "digest or size"):
            build.wheel_path(self.home)
        transport.assert_not_called()

    def test_failed_download_never_publishes_or_accepts_substituted_bytes(self):
        """Purpose: Reject interrupted or wrong artifacts. Inputs: Transport failures. Outputs: Empty cache."""
        self.cached.unlink()
        for interrupted in (False, True):

            def download(command, env, timeout, fail=interrupted):
                """Purpose: Inject incomplete transport. Inputs: Download destination. Outputs: Failure or junk."""
                if fail:
                    raise RuntimeError("owned interrupted download")
                target = Path(command[command.index("--dest") + 1]) / self.wheel.name
                target.write_bytes(b"substituted upstream asset")

            with (
                self.subTest(interrupted=interrupted),
                patch("tools.crawl4ai_tool.run", side_effect=download),
                self.assertRaises((RuntimeError, ValueError)),
            ):
                build.wheel_path(self.home)
            self.assertFalse(self.cached.exists())
            self.assertFalse(list(self.cached.parent.glob("download-*")))

    def test_wheel_metadata_rejects_escapes_aliases_and_special_members(self):
        """Purpose: Validate complete archives before use. Inputs: Malformed owned wheels. Outputs: Refusals."""
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            cases = (
                ["../escape"],
                ["/absolute"],
                ["drive:stream"],
                ["a.py", "A.py"],
                ["first.py", "folder//second.py"],
                ["trailing./file.py"],
                ["a.py", "a.py"],
            )
            for index, names in enumerate(cases):
                archive = root / f"bad-{index}.whl"
                with zipfile.ZipFile(archive, "w") as wheel, warnings.catch_warnings():
                    warnings.simplefilter("ignore", UserWarning)
                    for name in names:
                        wheel.writestr(name, b"# owned inert fixture\n")
                with self.subTest(names=names), self.assertRaisesRegex(ValueError, "Unsafe or duplicate"):
                    packaging.wheel_files(archive)
            for kind in (0o120777, 0o010000, 0o020000, 0o060000, 0o040000):
                archive = root / f"special-{kind}.whl"
                with zipfile.ZipFile(archive, "w") as wheel:
                    member = zipfile.ZipInfo("model.py")
                    member.external_attr = kind << 16
                    wheel.writestr(member, b"owned inert fixture")
                with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, "Unsafe or duplicate"):
                    packaging.wheel_files(archive)
            with patch.object(packaging, "MAX_SOURCE_BYTES", 1), self.assertRaisesRegex(ValueError, "extent"):
                packaging.wheel_files(archive)
            self.assertFalse((root / "escape").exists())

    def test_digest_sidecar_requires_one_complete_identity(self):
        """Purpose: Reject incomplete builder admission. Inputs: Malformed sidecars. Outputs: Errors."""
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "wheel.sha256"
            for content in ("", "garbage file.whl", hashlib.sha256(b"fixture").hexdigest()):
                path.write_text(content, encoding="ascii")
                with self.assertRaises(ValueError):
                    packaging.expected_digest(path)

    def test_installed_runtime_binds_original_sources_without_import_or_version_fallback(self):
        """Purpose: Reject installed source drift. Inputs: Original owned tree and mutations. Outputs: Exact refusal."""
        expected = build.runtime_source_sha256()
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp)
            for name, contents in packaging.wheel_files(build.wheel_path()).items():
                path = source.joinpath(*name.split("/"))
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(contents)
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

    def test_canonical_wheel_is_host_independent_and_record_valid(self):
        """Purpose: Preserve standard wheel integrity. Inputs: Equivalent platform ZIPs. Outputs: Equal valid wheels."""
        data = {"version": "1.0.0", "source_date_epoch": 1704067200}
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
                packaging.canonical_wheel(original, output, data, project="nltk")
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

    def test_hosted_scanners_discover_nonstandard_manifest_names(self):
        """Purpose: Prevent false empty dependency scans. Inputs: Owned scanners. Outputs: Explicit graph discovery."""
        scanner = (build.ROOT / ".github/workflows/security-code-scanning.yml").read_text(encoding="utf-8")
        for name in ("crawl4ai", "python-build"):
            self.assertIn(f"--lockfile=requirements.txt:tools/requirements/{name}.txt", scanner)
        dependabot = (build.ROOT / ".github/dependabot.yml").read_text(encoding="utf-8")
        self.assertIn("directory: /tools/requirements", dependabot)
