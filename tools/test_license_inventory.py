"""Negative controls for retained notice bytes and complete vendored-root coverage."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from tools import license_inventory as inventory


class LicenseInventoryTests(unittest.TestCase):
    """Exercise actual repository manifests and failing copies without fetching dependencies."""

    # Purpose: Reject redirected notice sources even when their targets stay inside the owned fixture repository.
    # Inputs: A real Windows junction or POSIX directory link leading to an otherwise valid text file.
    # Outputs: Admission rejects the redirect; cleanup removes only the owned link and leaves its target intact.
    def test_notice_source_reparse_boundary(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target, link = root / "notice-target", root / "notice-link"
            target.mkdir()
            (target / "NOTICE.txt").write_text("owned notice fixture", encoding="utf-8")
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
                with self.assertRaisesRegex(ValueError, "reparse point"):
                    inventory.notice_source(root, "notice-link/NOTICE.txt")
            finally:
                if os.name == "nt":
                    link.rmdir()
                else:
                    link.unlink()
            self.assertEqual((target / "NOTICE.txt").read_text(), "owned notice fixture")

    # Purpose: Require unique, attributed development notices rather than accepting duplicated provenance claims.
    # Inputs: A real-manifest fixture with one repeated development-notice record.
    # Outputs: Audit rejects the ambiguity before making a coverage claim.
    def test_duplicate_development_notice_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.copy_sources(root)
            path = root / inventory.DEVELOPMENT
            manifest = json.loads(path.read_text())
            manifest["notices"].append(dict(manifest["notices"][0]))
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "unique sources"):
                inventory.audit(root)

    # Purpose: Verify the actual inventory entry point preserves its legal and release-qualification limitations.
    # Inputs: Repository notices and real CLI invocation; no third-party runtime is installed or executed.
    # Outputs: Successful JSON reports notice coverage without claiming legal clearance or release verification.
    def test_real_cli_retains_qualification_limits(self):
        root = Path(__file__).resolve().parents[1]
        result = subprocess.run(
            [sys.executable, "-m", "tools.license_inventory"],
            cwd=root,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        record = json.loads(result.stdout)
        self.assertTrue(record["coverage_passed"])
        self.assertFalse(record["legal_clearance_claimed"])
        self.assertFalse(record["release_source_and_rebuild_verified"])

    @staticmethod
    def copy_sources(root: Path):
        """Purpose: Build a minimal fixture from real manifests. Inputs: Temp root. Outputs: Copied notice sources."""
        for manifest in (inventory.NATIVE, inventory.DEVELOPMENT):
            path = root / manifest
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(inventory.ROOT / manifest, path)
            for row in json.loads(path.read_text(encoding="utf-8"))["notices"]:
                target = root / row["source"]
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(inventory.ROOT / row["source"], target)

    def test_real_inventory_and_no_legal_overclaim(self):
        """Purpose: Verify coverage honestly. Inputs: Actual manifests. Outputs: Coverage without clearance claim."""
        result = inventory.audit()
        self.assertTrue(result["coverage_passed"])
        self.assertFalse(result["legal_clearance_claimed"])
        self.assertFalse(result["release_source_and_rebuild_verified"])
        self.assertIn("wimlib", result["native_dependency_roots"])

    def test_unknown_dependency_missing_notice_and_mutation(self):
        """Purpose: Reject incomplete/altered notice sets. Inputs: Real-source fixture. Outputs: Expected failures."""
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.copy_sources(root)
            new = root / "third_party/unreviewed"
            new.mkdir()
            with self.assertRaisesRegex(ValueError, "Unreviewed"):
                inventory.audit(root)
            new.rmdir()
            notice = root / "docs/licenses/Crawl4AI-0.9.5.txt"
            notice.write_text("Apache-2.0 only; attribution omitted", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "differs"):
                inventory.audit(root)
            notice.unlink()
            with self.assertRaisesRegex(ValueError, "absent"):
                inventory.audit(root)

    def test_unlisted_adapted_notice_and_path_escape(self):
        """Purpose: Require provenance for new adaptations. Inputs: Extra notice/unsafe paths. Outputs: Rejection."""
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.copy_sources(root)
            (root / "third_party/notices/unknown.txt").write_text("MIT", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "adapted-tool"):
                inventory.audit(root)
        for name in ("../LICENSE", "/LICENSE", "C:/LICENSE", "docs\\license"):
            with self.assertRaises(ValueError):
                inventory.notice_source(inventory.ROOT, name)


if __name__ == "__main__":
    unittest.main()
