"""Offline admission, source-delta and security contracts for the portable Crawl4AI source build."""

from __future__ import annotations

import io
import os
import subprocess
import tarfile
import tempfile
import unittest
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
            notice = (source / "SUPERZIP_BUILD_NOTICE.txt").read_text()
            self.assertIn("downstream build", notice)
            self.assertIn("UncleCode", notice)
            self.assertEqual((source / "crawl4ai/superzip_download.py").read_text(), build.OPENER.read_text())
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
