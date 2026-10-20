"""Offline admission, source-delta and security contracts for the portable Crawl4AI source build."""

from __future__ import annotations

import io
import os
import subprocess
import tarfile
import tempfile
import unittest
from contextlib import nullcontext
from pathlib import Path
from unittest.mock import patch

from tools import crawl4ai_download_opener as opener
from tools import crawl4ai_source_build as build


class SourceContracts(unittest.TestCase):
    """Verify exact original provenance, narrow source changes and the real platform opener."""

    def test_provenance_archive_is_visible_to_normal_git_inventory(self):
        """Purpose: Preserve hosted source availability. Inputs: Actual ignore policy/archive. Outputs: Not ignored."""
        archive = (build.SOURCE_ROOT / build.recipe()["source"]).relative_to(build.ROOT).as_posix()
        result = subprocess.run(
            ["git", "-C", str(build.ROOT), "check-ignore", archive], capture_output=True, timeout=10, check=False
        )
        self.assertEqual(result.returncode, 1, "Published source archive must be visible without forced staging")

    def test_source_repair_preserves_all_other_runtime_files_and_original_license(self):
        """Purpose: Bound the repair delta. Inputs: Actual source/recipe. Outputs: Exact unaffected source/notices."""
        data = build.recipe()
        with tempfile.TemporaryDirectory() as owned:
            source = build.extract_source(build.SOURCE_ROOT / data["source"], Path(owned) / "source")
            before = {p.relative_to(source): p.read_bytes() for p in source.rglob("*") if p.is_file()}
            build.repair_source(source, data)
            changed = {
                Path("crawl4ai/async_crawler_strategy.py"),
                Path("crawl4ai/__version__.py"),
                Path("pyproject.toml"),
            }
            for path, contents in before.items():
                if path not in changed:
                    self.assertEqual((source / path).read_bytes(), contents)
            original_license = (build.ROOT / "docs/licenses/Crawl4AI-0.9.4.txt").read_text(encoding="utf-8")
            self.assertEqual((source / "LICENSE").read_text(encoding="utf-8"), original_license)
            notice = (source / "FLITZZIP_BUILD_NOTICE.txt").read_text()
            self.assertIn("downstream build", notice)
            self.assertIn("UncleCode", notice)
            self.assertEqual((source / "crawl4ai/flitzzip_download.py").read_text(), build.OPENER.read_text())
            for path, expected in build.runtime_hashes().items():
                self.assertEqual(build.packaging.digest(source / path), expected)
            with self.assertRaisesRegex(ValueError, "requires renewed review"):
                build.repair_source(source, data)

    def test_tampered_artifacts_and_unknown_source_refused(self):
        """Purpose: Refuse substituted source. Inputs: Wrong digest and changed opener. Outputs: Errors."""
        with patch.object(build.packaging, "digest", return_value="different"), self.assertRaises(ValueError):
            build.recipe()
        with tempfile.TemporaryDirectory() as owned:
            root = Path(owned)
            (root / "crawl4ai").mkdir()
            (root / "crawl4ai/async_crawler_strategy.py").write_text("# changed upstream boundary\n")
            with self.assertRaisesRegex(ValueError, "requires renewed review"):
                build.repair_source(root, build.recipe())
        original = '    safe_name = os.path.basename(filename or "")\n    return os.open(path, flags | os.O_NOFOLLOW)'
        for changed in (original.replace("safe_name =", "renamed ="), original + original):
            with self.subTest(source=changed), self.assertRaisesRegex(ValueError, "requires renewed review"):
                build.patched_strategy(changed)

    def test_recipe_and_identity_bind_every_build_input(self):
        """Purpose: Bind all provenance and repair inputs.
        Inputs: Actual recipe and semantic mutations. Outputs: Stable identity/refusal.
        """
        data = build.recipe()
        self.assertEqual(len(build.identity()), 64)
        variants = (
            data | {"source_url": "https://example.invalid/crawl4ai.tar.gz"},
            data | {"source_date_epoch": data["source_date_epoch"] + 1},
            data | {"unreviewed": True},
        )
        for variant in variants:
            with (
                self.subTest(variant=variant),
                patch.object(build.packaging, "read_recipe", return_value=variant),
                self.assertRaisesRegex(ValueError, "Unsupported Crawl4AI"),
            ):
                build.recipe()

    def test_extraction_rejects_traversal_links_duplicates_and_size_before_writing(self):
        """Purpose: Protect owned extraction. Inputs: Hostile synthetic tar members. Outputs: No published payload."""
        with tempfile.TemporaryDirectory() as owned:
            root = Path(owned)
            for index, name in enumerate(("crawl4ai-0.9.4/../escape", "/absolute", "crawl4ai-0.9.4/name:stream")):
                archive = root / f"bad-{index}.tar.gz"
                with tarfile.open(archive, "w:gz") as output:
                    first = tarfile.TarInfo("crawl4ai-0.9.4/valid")
                    first.size = 1
                    output.addfile(first, io.BytesIO(b"x"))
                    bad = tarfile.TarInfo(name)
                    output.addfile(bad)
                destination = root / f"destination-{index}"
                with self.assertRaises(ValueError):
                    build.extract_source(archive, destination)
                self.assertFalse(destination.exists())
            for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE):
                archive = root / "link.tar.gz"
                with tarfile.open(archive, "w:gz") as output:
                    item = tarfile.TarInfo("crawl4ai-0.9.4/link")
                    item.type = kind
                    item.linkname = "../outside"
                    output.addfile(item)
                with self.assertRaises(ValueError):
                    build.extract_source(archive, root / "link-destination")

            archive = root / "bounded.tar.gz"
            with tarfile.open(archive, "w:gz") as output:
                item = tarfile.TarInfo("crawl4ai-0.9.4/payload")
                item.size = 1
                output.addfile(item, io.BytesIO(b"x"))
            with patch.object(build.packaging, "MAX_SOURCE_BYTES", 0), self.assertRaisesRegex(ValueError, "extent"):
                build.extract_source(archive, root / "bounded-destination")
            occupied = root / "occupied"
            occupied.mkdir()
            with self.assertRaisesRegex(ValueError, "destination must be new"):
                build.extract_source(archive, occupied)

    def test_patch_contracts_and_license_failure_leave_source_unchanged(self):
        """Purpose: Refuse upstream drift before source mutation.
        Inputs: Changed contracts/version/license. Outputs: Error and unchanged tree.
        """
        strategy = '    safe_name = os.path.basename(filename or "")\n    return os.open(path, flags | os.O_NOFOLLOW)\n'
        with self.assertRaisesRegex(ValueError, "download contract changed"):
            build.patched_strategy(strategy)
        with self.assertRaisesRegex(ValueError, "version changed"):
            build.patched_version('__version__ = "9.9.9"', "1.0")
        data = build.recipe()
        with tempfile.TemporaryDirectory() as owned:
            source = build.extract_source(build.SOURCE_ROOT / data["source"], Path(owned) / "source")
            before = {path.relative_to(source): path.read_bytes() for path in source.rglob("*") if path.is_file()}
            config = source / "pyproject.toml"
            config.write_text(
                config.read_text(encoding="utf-8").replace('license = "Apache-2.0"', 'license = "MIT"'),
                encoding="utf-8",
            )
            before[config.relative_to(source)] = config.read_bytes()
            with self.assertRaisesRegex(ValueError, "license packaging changed"):
                build.repair_source(source, data)
            after = {path.relative_to(source): path.read_bytes() for path in source.rglob("*") if path.is_file()}
            self.assertEqual(after, before)

    def test_build_wheel_uses_owned_builder_and_canonical_output(self):
        """Purpose: Exercise offline wheel orchestration.
        Inputs: Owned cache and inert build doubles. Outputs: Exact commands/environment/output.
        """
        data = build.recipe()
        with tempfile.TemporaryDirectory() as owned:
            root = Path(owned)
            home, output = root / "home", root / "wheels"
            commands = []

            def create_builder(directory):
                python = Path(directory) / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
                python.parent.mkdir(parents=True)
                python.write_bytes(b"inert interpreter fixture")

            def extract(_archive, destination):
                destination.mkdir(parents=True)
                return destination

            def canonical(_built, destination, _data, *, project):
                self.assertEqual(project, "crawl4ai")
                destination.write_bytes(b"canonical wheel")

            with (
                patch("tools.crawl4ai_tool.text_identity", return_value="a" * 64),
                patch("tools.crawl4ai_tool.tool_environment", return_value={"BASE": "1"}),
                patch("tools.crawl4ai_tool.installation_lock", side_effect=lambda _path: nullcontext()),
                patch(
                    "tools.crawl4ai_tool.run",
                    side_effect=lambda command, env, *args: commands.append((command, dict(env), args)),
                ),
                patch.object(build.venv, "EnvBuilder") as builder,
                patch.object(build, "extract_source", side_effect=extract),
                patch.object(build, "repair_source") as repair,
                patch.object(build.packaging, "canonical_wheel", side_effect=canonical) as package,
            ):
                builder.return_value.create.side_effect = create_builder
                wheel = build.build_wheel(home, output, data)
            self.assertEqual(wheel.read_bytes(), b"canonical wheel")
            builder.assert_called_once_with(with_pip=True)
            self.assertEqual(repair.call_count, 1)
            self.assertEqual(package.call_count, 1)
            self.assertEqual(len(commands), 3)
            self.assertIn("--require-hashes", commands[0][0])
            self.assertEqual(commands[1][0][-2:], ["pip", "check"])
            self.assertIn("--no-build-isolation", commands[2][0])
            for _command, env, _args in commands:
                self.assertEqual(env["BASE"], "1")
                self.assertEqual(env["SOURCE_DATE_EPOCH"], str(data["source_date_epoch"]))
                self.assertEqual(env["PYTHONHASHSEED"], "0")

    def test_ensure_wheel_reuses_or_builds_only_exact_artifact(self):
        """Purpose: Enforce cache identity.
        Inputs: Missing, exact and changed cached wheels. Outputs: Build/reuse/refusal.
        """
        data = build.recipe()
        with tempfile.TemporaryDirectory() as owned:
            home = Path(owned)
            directory = home / ("crawl4ai-wheel-" + "b" * 16)
            wheel = directory / f"crawl4ai-{data['version']}-py3-none-any.whl"
            expected = "c" * 64
            common = (
                patch.object(build, "recipe", return_value=data),
                patch.object(build, "identity", return_value="b" * 64),
                patch.object(build.packaging, "expected_digest", return_value=expected),
                patch("tools.crawl4ai_tool.installation_lock", side_effect=lambda _path: nullcontext()),
            )
            with common[0], common[1], common[2], common[3], patch.object(build, "build_wheel") as builder:
                directory.mkdir()
                wheel.write_bytes(b"exact")
                with patch.object(build.packaging, "digest", return_value=expected):
                    self.assertEqual(build.ensure_wheel(home), wheel)
                builder.assert_not_called()
            wheel.unlink()
            with (
                patch.object(build, "recipe", return_value=data),
                patch.object(build, "identity", return_value="b" * 64),
                patch.object(build.packaging, "expected_digest", return_value=expected),
                patch("tools.crawl4ai_tool.installation_lock", side_effect=lambda _path: nullcontext()),
                patch.object(build, "build_wheel", side_effect=lambda *_args: wheel),
                patch.object(build.packaging, "digest", return_value=expected),
            ):
                self.assertEqual(build.ensure_wheel(home), wheel)
            wheel.write_bytes(b"changed")
            with (
                patch.object(build, "recipe", return_value=data),
                patch.object(build, "identity", return_value="b" * 64),
                patch.object(build.packaging, "expected_digest", return_value=expected),
                patch("tools.crawl4ai_tool.installation_lock", side_effect=lambda _path: nullcontext()),
                patch.object(build.packaging, "digest", return_value="d" * 64),
                self.assertRaisesRegex(ValueError, "digest mismatch"),
            ):
                build.ensure_wheel(home)

    def test_real_platform_open_and_failure_cleanup(self):
        """Purpose: Exercise the installed helper source. Inputs: Owned regular/unsafe file. Outputs: Exact bytes."""
        with tempfile.TemporaryDirectory() as owned:
            path = Path(owned) / "output"
            with open(path, "wb", opener=opener.open_download) as stream:
                stream.write(b"byte exact")
            self.assertEqual(path.read_bytes(), b"byte exact")
            with self.assertRaises(OSError), open(Path(owned), "wb", opener=opener.open_download):
                pass
            if os.name == "nt":
                link = Path(owned) / "hardlink"
                os.link(path, link)
                for _ in range(20):
                    with self.assertRaises(OSError), open(link, "wb", opener=opener.open_download):
                        pass
                self.assertEqual(path.read_bytes(), b"byte exact")
                link.unlink()
            path.unlink()


if __name__ == "__main__":
    unittest.main()
