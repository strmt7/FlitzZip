"""Regression contracts for changed-file scanner publication admission."""

import copy
import csv
import hashlib
import io
import json
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

from tools import scanner_preflight as preflight
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

    # Purpose: Preserve an independent approved source while changing only its build context.
    # Inputs: Small source, caller and SARIF fixtures. Outputs: A valid unchanged ledger plus its source bytes.
    def write_context_fixture(self):
        row, blobs, report = self.reviewed_fixture()
        report["version"] = "2.1.0"
        blobs["CMakeLists.txt"] = b"add_library(fixture source.cpp)\n"
        row["context_paths"] = "|".join(sorted(blobs))
        row["context_sha256"] = hashlib.sha256(
            canonical({name: hashlib.sha256(data).hexdigest() for name, data in blobs.items()})
        ).hexdigest()
        for name, data in blobs.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        policy = self.root / preflight.CURRENT_POLICY
        policy.parent.mkdir(parents=True, exist_ok=True)
        with policy.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(row))
            writer.writeheader()
            writer.writerow(row)
        return row, blobs, report

    # Purpose: Catch expired build/caller review evidence before push even when the reported source is unchanged.
    # Inputs: An independently approved report, then a CMake-only change with identical source/scanner output.
    # Outputs: The source is rescanned, the changed context fails admission, and raw findings/approval stay intact.
    def test_context_change_rescans_source_and_blocks_expired_approval(self):
        row, blobs, report = self.write_context_fixture()
        policy_before = (self.root / preflight.CURRENT_POLICY).read_bytes()

        # Purpose: Model unchanged detector observations while checking its actual immutable input scope.
        # Inputs: Production-selected snapshot and output directory. Outputs: Independent raw scanner reports.
        def scan(snapshot, directory, _gitleaks, _devskim):
            self.assertEqual((snapshot / row["path"]).read_bytes(), blobs[row["path"]])
            self.assertEqual((snapshot / "CMakeLists.txt").read_bytes(), (self.root / "CMakeLists.txt").read_bytes())
            (directory / "gitleaks.json").write_text("[]")
            (directory / "devskim.sarif").write_text(json.dumps(report))
            return {
                name: {"exit_code": 0, "timed_out": False, "output_limit_exceeded": False}
                for name in ("gitleaks", "devskim")
            }

        with (
            patch.object(preflight, "ROOT", self.root),
            patch.object(preflight, "changed_paths", return_value=["CMakeLists.txt"]),
            patch.object(preflight, "validate_report_directory", return_value=[]),
            patch.object(preflight, "require_versions", return_value={}),
            patch.object(preflight, "read_policy", return_value=b"fixture"),
            patch.object(preflight, "read_source_policy", return_value=b"fixture"),
            patch.object(preflight, "review_repository", return_value=row["repository"]),
            patch.object(preflight, "scan_snapshot", side_effect=scan),
            patch.object(
                preflight,
                "review_findings",
                side_effect=lambda *_args: {
                    "reviewed_metadata": [],
                    "reviewed_source": [],
                    "unresolved_count": 1,
                },
            ),
            patch("sys.argv", ["scanner_preflight", "--gitleaks", "fixture-gitleaks", "--devskim", "fixture-devskim"]),
        ):
            for changed in (False, True):
                if changed:
                    (self.root / "CMakeLists.txt").write_bytes(blobs["CMakeLists.txt"] + b"add_test(new_contract)\n")
                with self.subTest(changed=changed), redirect_stdout(io.StringIO()) as output:
                    self.assertEqual(preflight.main(), int(changed))
                    result = json.loads(output.getvalue())
                    self.assertEqual(result["unresolved_devskim"], int(changed))
                    receipt = json.loads(Path(result["receipt"]).read_text())
                    self.assertEqual(receipt["changed_paths"], ["CMakeLists.txt"])
                    self.assertEqual(receipt["review_affected_paths"], [row["path"]])
                    self.assertEqual(receipt["finding_counts"]["devskim"], 1)
                    self.assertTrue(receipt["source_unchanged"])
                    self.assertEqual(json.loads(Path(result["receipt"]).with_name("devskim.sarif").read_text()), report)
        self.assertEqual((self.root / preflight.CURRENT_POLICY).read_bytes(), policy_before)

    # Purpose: Bound automatic rescanning to affected approved sources rather than every document change.
    # Inputs: An independent review and unrelated, source, caller, build, and ledger change selections.
    # Outputs: Unrelated edits select no source review; each dependency selects the original complete review.
    def test_review_selection_tracks_context_without_broad_scans(self):
        row, _blobs, _report = self.write_context_fixture()
        for index, name in enumerate(["unrelated.md", row["path"], "CMakeLists.txt", preflight.CURRENT_POLICY]):
            with self.subTest(name=name):
                selected, hashes = preflight.freeze_current_reviews(self.root, [name], self.root / f"review-{index}")
                self.assertEqual(selected, [] if index == 0 else [row])
                self.assertIn(preflight.CURRENT_POLICY, hashes)
                self.assertEqual(row["path"] in hashes, index != 0)
        with patch.object(preflight, "MAX_FILES", 1), self.assertRaisesRegex(ValueError, "snapshot budget"):
            snapshot_inputs(self.root, [row["path"], "CMakeLists.txt"], self.root / "oversized")
        self.assertFalse((self.root / "oversized").exists())

    # Purpose: Allow removal of obsolete reviewed code without inventing a finding on a nonexistent source.
    # Inputs: A legitimate source deletion together with its affected build context.
    # Outputs: No scan or approval is claimed for that source; the unchanged ledger remains recorded.
    def test_removed_reviewed_source_does_not_block_build_cleanup(self):
        row, _blobs, _report = self.write_context_fixture()
        (self.root / row["path"]).unlink()
        rows, hashes = preflight.freeze_current_reviews(self.root, ["CMakeLists.txt"], self.root / "review")
        self.assertEqual(rows, [])
        self.assertEqual(set(hashes), {preflight.CURRENT_POLICY})

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
