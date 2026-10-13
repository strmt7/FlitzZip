"""Regression contracts for changed-file scanner publication admission."""

import copy
import csv
import hashlib
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.scanner_hosted_review import canonical, match_snapshot_false_positives
from tools.scanner_preflight import changed_paths, finding_count, snapshot_inputs


class ScannerPreflightTests(unittest.TestCase):
    # Purpose: Isolate source and scanner-output fixtures from the live checkout.
    # Inputs: None. Outputs: Owned temporary root removed after every test.
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    # Purpose: Exercise exact current review admission with independent immutable source/configuration bytes.
    # Inputs: A real schema row with synthetic context and SARIF identities. Outputs: One isolated fixture tuple.
    def reviewed_fixture(self):
        path = Path(__file__).resolve().parents[1] / ".github/scanner-false-positive-reviews.csv"
        with path.open(encoding="utf-8", newline="") as source:
            row = next(csv.DictReader(source))
        blobs = {name: b"independent source context\n" for name in row["context_paths"].split("|")}
        blobs[row["evidence"].split("#")[0]] = f"### Alert {row['alert']}\n".encode()
        hashes = {name: hashlib.sha256(payload).hexdigest() for name, payload in blobs.items()}
        row["input_sha256"] = hashes[row["path"]]
        row["context_sha256"] = hashlib.sha256(canonical(hashes)).hexdigest()
        result = {
            "ruleId": row["rule"],
            "locations": [
                {
                    "physicalLocation": {
                        "artifactLocation": {"uri": row["path"].replace("/", "\\")},
                        "region": {
                            name: int(row[key])
                            for name, key in (
                                ("startLine", "start_line"),
                                ("startColumn", "start_column"),
                                ("endLine", "end_line"),
                                ("endColumn", "end_column"),
                            )
                        },
                    }
                }
            ],
        }
        report = {"runs": [{"tool": {"driver": {"name": row["tool"], "version": row["version"]}}, "results": [result]}]}
        return row, blobs, report

    # Purpose: Prevent preflight from broadening approved reviews or hiding their raw findings.
    # Inputs: Exact reviewed report plus changed repository, source, caller, evidence, rule, scanner and location.
    # Outputs: Accepts only the complete original tuple, rejects every mutation and preserves input reports.
    def test_current_snapshot_requires_all_reviewed_identities(self):
        row, blobs, report = self.reviewed_fixture()
        original = copy.deepcopy(report)
        accepted = match_snapshot_false_positives([row], report, row["repository"], blobs.__getitem__)
        self.assertEqual(accepted, [{"run_index": 0, "result_index": 0, "alert": int(row["alert"])}])
        self.assertEqual(report, original)
        self.assertEqual(match_snapshot_false_positives([row], report, "unrelated/repo", blobs.__getitem__), [])
        for name in blobs:
            with self.subTest(context=name):
                altered = {**blobs, name: blobs[name] + b"changed\n"}
                self.assertEqual(
                    match_snapshot_false_positives([row], report, row["repository"], altered.__getitem__), []
                )
        for field in ("name", "version"):
            altered = copy.deepcopy(report)
            altered["runs"][0]["tool"]["driver"][field] += "-changed"
            self.assertEqual(match_snapshot_false_positives([row], altered, row["repository"], blobs.__getitem__), [])
        for field in ("startLine", "startColumn", "endLine", "endColumn"):
            altered = copy.deepcopy(report)
            altered["runs"][0]["results"][0]["locations"][0]["physicalLocation"]["region"][field] += 1
            self.assertEqual(match_snapshot_false_positives([row], altered, row["repository"], blobs.__getitem__), [])
        altered = copy.deepcopy(report)
        altered["runs"][0]["results"][0]["ruleId"] += "-new"
        self.assertEqual(match_snapshot_false_positives([row], altered, row["repository"], blobs.__getitem__), [])

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
