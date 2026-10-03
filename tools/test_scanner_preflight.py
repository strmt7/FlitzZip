"""Regression contracts for changed-file scanner publication admission."""

import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.scanner_preflight import changed_paths, finding_count, snapshot_inputs


class ScannerPreflightTests(unittest.TestCase):
    # Purpose: Isolate source and scanner-output fixtures from the live checkout.
    # Inputs: None. Outputs: Owned temporary root removed after every test.
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    # Purpose: Preserve staged, unstaged and new filenames exactly, including spaces.
    # Inputs: An owned Git fixture. Outputs: Complete input selection without deleted paths or option interpretation.
    def test_changed_path_selection(self):
        def git(*args):
            subprocess.run(["git", "-C", str(self.root), *args], check=True, capture_output=True)

        git("init", "--quiet")
        for name in ("changed file.py", "deleted.py", "staged.py"):
            (self.root / name).write_text("initial", encoding="utf-8")
        git("add", ".")
        git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "--quiet", "-m", "fixture")
        (self.root / "changed file.py").write_text("changed", encoding="utf-8")
        (self.root / "deleted.py").unlink()
        (self.root / "staged.py").write_text("staged", encoding="utf-8")
        git("add", "staged.py")
        (self.root / "new file.py").write_text("new", encoding="utf-8")
        self.assertEqual(changed_paths(self.root, "HEAD"), ["changed file.py", "new file.py", "staged.py"])
        with self.assertRaises(subprocess.CalledProcessError):
            changed_paths(self.root, "--output=unexpected")

    # Purpose: Reject path escapes, reparse redirects and oversized input without partial qualification.
    # Inputs: Owned source bytes and deliberately inadmissible paths. Outputs: Every unsafe boundary raises.
    def test_snapshot_boundaries(self):
        source = self.root / "source.py"
        source.write_bytes(b"12345")
        for name in ("../outside.py", "C:/outside.py", "..\\outside.py"):
            with self.assertRaises(ValueError):
                snapshot_inputs(self.root, [name], self.root / "snapshot")
        with patch("tools.scanner_preflight.MAX_BYTES", 4), self.assertRaises(ValueError):
            snapshot_inputs(self.root, ["source.py"], self.root / "snapshot")
        with patch.object(Path, "is_junction", return_value=True), self.assertRaises(ValueError):
            snapshot_inputs(self.root, ["source.py"], self.root / "snapshot")
        hashes = snapshot_inputs(self.root, ["source.py"], self.root / "snapshot")
        self.assertEqual((self.root / "snapshot/source.py").read_bytes(), source.read_bytes())
        self.assertEqual(len(hashes["source.py"]), 64)

    # Purpose: Keep every detector result visible, regardless of severity or whether the value resembles metadata.
    # Inputs: Controlled SARIF and malformed/missing/oversized reports. Outputs: Complete counts or hard failure.
    def test_findings_are_never_filtered(self):
        path = self.root / "report.json"
        report = {
            "version": "2.1.0",
            "runs": [
                {
                    "tool": {"driver": {"name": "devskim"}},
                    "results": [{"ruleId": "DS173237"}, {"ruleId": "actual-code-finding", "level": "note"}],
                }
            ],
        }
        path.write_text(json.dumps(report), encoding="utf-8")
        self.assertEqual(finding_count(path, "devskim"), 2)
        for malformed in ({"runs": []}, {"version": "2.1.0", "runs": []}, {"version": "2.1.0", "runs": [{}]}):
            path.write_text(json.dumps(malformed), encoding="utf-8")
            with self.assertRaises((ValueError, KeyError)):
                finding_count(path, "devskim")
        path.write_text("[]", encoding="utf-8")
        self.assertEqual(finding_count(path, "gitleaks"), 0)
        for malformed in ('{"version":"2.1.0","version":"2.1.0"}', "[NaN]", "[Infinity]"):
            path.write_text(malformed, encoding="utf-8")
            with self.assertRaises(ValueError):
                finding_count(path, "gitleaks")
        path.write_text("[]", encoding="utf-8")
        with patch("tools.scanner_preflight.MAX_BYTES", 1), self.assertRaises(ValueError):
            finding_count(path, "gitleaks")
        with self.assertRaises(FileNotFoundError):
            finding_count(self.root / "absent.json", "gitleaks")


if __name__ == "__main__":
    unittest.main()
