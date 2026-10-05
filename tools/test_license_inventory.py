"""Negative controls for retained notice bytes and complete vendored-root coverage."""

from __future__ import annotations

import json
import shutil
import tempfile
import unittest
from pathlib import Path

from tools import license_inventory as inventory


class LicenseInventoryTests(unittest.TestCase):
    """Exercise actual repository manifests and failing copies without fetching dependencies."""

    def copy_sources(self, root: Path):
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
            notice = root / "docs/licenses/Crawl4AI-0.9.4.txt"
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
