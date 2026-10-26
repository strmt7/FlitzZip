"""Regression contracts for changed-file scanner publication admission."""

import copy
import csv
import hashlib
import io
import json
import subprocess
import sys
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
    # Inputs: Scanner name and a real schema row with synthetic context and SARIF identities.
    # Outputs: One isolated fixture tuple.
    @staticmethod
    def reviewed_fixture(tool="devskim"):
        path = Path(__file__).resolve().parents[1] / ".github/scanner-false-positive-reviews.csv"
        with path.open(encoding="utf-8", newline="") as source:
            row = next(row for row in csv.DictReader(source) if row["tool"] == tool)
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
    # Inputs: Scanner name and small source, caller and SARIF fixtures.
    # Outputs: A valid unchanged ledger plus its source bytes.
    def write_context_fixture(self, tool="devskim"):
        row, blobs, report = self.reviewed_fixture(tool)
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

    # Purpose: Catch hosted-review expiry locally without pretending to execute its detector or renewing approval.
    # Inputs: A reviewed Semgrep source, unchanged acquisition code and a changed dependency context.
    # Outputs: Unrelated edits stay bounded, valid context passes, and changed context fails with the ledger intact.
    def test_hosted_review_context_expires_before_scanning(self):
        row, blobs, _report = self.write_context_fixture("Semgrep OSS")
        policy = self.root / preflight.CURRENT_POLICY
        original = policy.read_bytes()
        selected, hashes = preflight.freeze_current_reviews(self.root, ["unrelated.md"], self.root / "unrelated")
        self.assertEqual(selected, [])
        self.assertEqual(set(hashes), {preflight.CURRENT_POLICY})
        selected, hashes = preflight.freeze_current_reviews(self.root, ["CMakeLists.txt"], self.root / "valid")
        self.assertEqual(selected, [])
        self.assertEqual(hashes[row["path"]], hashlib.sha256(blobs[row["path"]]).hexdigest())
        (self.root / "CMakeLists.txt").write_bytes(b"changed dependency context\n")
        with self.assertRaisesRegex(ValueError, f"stale source or scanner context: {row['alert']}"):
            preflight.freeze_current_reviews(self.root, ["CMakeLists.txt"], self.root / "stale")
        self.assertEqual(policy.read_bytes(), original)
        self.assertEqual((self.root / row["path"]).read_bytes(), blobs[row["path"]])

    # Purpose: Enforce repository, inventory-count and regular-file boundaries before any detector starts.
    # Inputs: Credential-bearing origin, over-count Git metadata and a directory presented as a changed file.
    # Outputs: Every ambiguous input is rejected without creating a source snapshot.
    def test_repository_and_changed_input_admission_bounds(self):
        with (
            patch.object(subprocess, "check_output", return_value=b"https://token@github.com/owner/repository.git\n"),
            self.assertRaisesRegex(ValueError, "credential-free GitHub origin"),
        ):
            preflight.review_repository(self.root)
        with patch.object(subprocess, "check_output", return_value=b"git@github.com:owner/repository.git\n"):
            self.assertEqual(preflight.review_repository(self.root), "owner/repository")
        responses = [b"a" * 40 + b"\n", b"first.py\0second.py\0", b""]
        with (
            patch.object(subprocess, "check_output", side_effect=responses),
            patch.object(preflight, "MAX_FILES", 1),
            self.assertRaisesRegex(ValueError, "Changed input count"),
        ):
            changed_paths(self.root, "HEAD")
        (self.root / "directory.py").mkdir()
        with self.assertRaisesRegex(ValueError, "not a regular file"):
            snapshot_inputs(self.root, ["directory.py"], self.root / "snapshot")

    # Purpose: Refuse review cohorts whose exact source/caller evidence exceeds the bounded cache contract.
    # Inputs: A valid affected review with a deliberately reduced aggregate byte ceiling.
    # Outputs: Review freezing fails before copying its source context.
    def test_current_review_evidence_budget(self):
        row, _blobs, _report = self.write_context_fixture()
        with (
            patch.object(preflight, "MAX_CURRENT_CACHE_BYTES", 1),
            self.assertRaisesRegex(ValueError, "evidence byte budget"),
        ):
            preflight.freeze_current_reviews(self.root, [row["path"]], self.root / "bounded-review")

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

    # Purpose: Reject structurally malformed SARIF without leaking nested indexing exceptions.
    # Inputs: Wrong producer, result type, run type, tool type and driver type.
    # Outputs: Every malformed report raises the same controlled schema error.
    def test_devskim_report_requires_exact_producer_shape(self):
        path = self.root / "devskim.sarif"
        malformed_runs = (
            {"tool": {"driver": {"name": "other"}}, "results": []},
            {"tool": {"driver": {"name": "devskim"}}, "results": None},
            None,
            {"tool": None, "results": []},
            {"tool": {"driver": None}, "results": []},
        )
        for run in malformed_runs:
            path.write_text(json.dumps({"version": "2.1.0", "runs": [run]}), encoding="utf-8")
            with self.subTest(run=run), self.assertRaisesRegex(ValueError, "unexpected producer or result shape"):
                finding_count(path, "devskim")

    # Purpose: Bind both detector binaries to unambiguous repository pins before scanning any source.
    # Inputs: Missing workflow pin, failed version process and a successful but mismatched version response.
    # Outputs: Each state fails closed with an actionable, value-free diagnostic.
    def test_pinned_scanner_version_failures_are_closed(self):
        workflow = self.root / ".github/workflows/security-code-scanning.yml"
        requirements = self.root / ".github/requirements/devskim-packaging.json"
        workflow.parent.mkdir(parents=True)
        requirements.parent.mkdir(parents=True)
        requirements.write_text(json.dumps({"version": "1.2.3"}), encoding="utf-8")
        with patch.object(preflight, "ROOT", self.root), self.assertRaisesRegex(ValueError, "pin is ambiguous"):
            workflow.write_text("no detector pin", encoding="utf-8")
            preflight.require_versions("gitleaks", "devskim")
        workflow.write_text("ghcr.io/gitleaks/gitleaks:v8.30.1@sha256:fixture", encoding="utf-8")
        failure = {"exit_code": 1, "timed_out": False, "output_limit_exceeded": False, "stdout": "", "stderr": ""}
        with (
            patch.object(preflight, "ROOT", self.root),
            patch.object(preflight, "run_scanner", return_value=failure),
            self.assertRaisesRegex(ValueError, "version probe failed"),
        ):
            preflight.require_versions("gitleaks", "devskim")
        mismatch = {
            "exit_code": 0,
            "timed_out": False,
            "output_limit_exceeded": False,
            "stdout": "unrelated version",
            "stderr": "",
        }
        with (
            patch.object(preflight, "ROOT", self.root),
            patch.object(preflight, "run_scanner", return_value=mismatch),
            self.assertRaisesRegex(ValueError, "differs from the repository pin"),
        ):
            preflight.require_versions("gitleaks", "devskim")
        gitleaks = self.root / "gitleaks.exe"
        devskim = self.root / "devskim.exe"
        gitleaks.write_bytes(b"owned gitleaks fixture")
        devskim.write_bytes(b"owned devskim fixture")
        success = (
            {**mismatch, "stdout": "gitleaks 8.30.1"},
            {**mismatch, "stdout": "Microsoft.DevSkim.CLI 1.2.3"},
        )
        with patch.object(preflight, "ROOT", self.root), patch.object(preflight, "run_scanner", side_effect=success):
            observed = preflight.require_versions(str(gitleaks), str(devskim))
        self.assertEqual(set(observed), {"gitleaks", "devskim"})
        self.assertEqual(observed["gitleaks"]["binary_sha256"], hashlib.sha256(gitleaks.read_bytes()).hexdigest())

    # Purpose: Exercise bounded process ownership and exact detector command framing without external scanners.
    # Inputs: A real short Python child plus an owned empty source/report directory and mocked detector results.
    # Outputs: Child evidence is complete and both detector vectors retain their report/options boundaries.
    def test_bounded_scanner_execution_and_command_framing(self):
        result = preflight.run_scanner(
            [sys.executable, "-c", "print('bounded scanner fixture')"],
            timeout=15,
            directory=Path(__file__).resolve().parents[1],
        )
        self.assertEqual(result["exit_code"], 0)
        self.assertIn("bounded scanner fixture", result["stdout"])
        snapshot = self.root / "snapshot"
        reports = self.root / "reports"
        snapshot.mkdir()
        reports.mkdir()
        status = {"exit_code": 0, "timed_out": False, "output_limit_exceeded": False}
        with patch.object(preflight, "run_scanner", side_effect=[status, status]) as scanner:
            observed = preflight.scan_snapshot(snapshot, reports, "gitleaks", "devskim")
        self.assertEqual(set(observed), {"gitleaks", "devskim"})
        self.assertEqual(scanner.call_count, 2)
        gitleaks_command = scanner.call_args_list[0].args[0]
        devskim_command = scanner.call_args_list[1].args[0]
        self.assertEqual(gitleaks_command[gitleaks_command.index("--report-path") + 1], str(reports / "gitleaks.json"))
        self.assertEqual(devskim_command[devskim_command.index("-O") + 1], str(reports / "devskim.sarif"))
        self.assertTrue((reports / "devskim-options.json").is_file())

    # Purpose: Exercise no-change, provisioning failure and redirected-output admission before detector execution.
    # Inputs: Controlled path inventories, explicit empty/tool arguments and a simulated output junction.
    # Outputs: No-change returns cleanly; provisioning and unsafe output states fail before source copying.
    def test_main_early_admission_boundaries(self):
        with (
            patch.object(preflight, "ROOT", self.root),
            patch.object(preflight, "changed_paths", return_value=[]),
            patch.object(preflight, "validate_report_directory"),
            patch("sys.argv", ["scanner_preflight"]),
            redirect_stdout(io.StringIO()) as output,
        ):
            self.assertEqual(preflight.main(), 0)
        self.assertIn("no changed publication inputs", output.getvalue())
        with (
            patch.object(preflight, "ROOT", self.root),
            patch.object(preflight, "changed_paths", return_value=["source.py"]),
            patch.object(preflight, "validate_report_directory"),
            patch.object(
                preflight, "provision_missing_scanners", side_effect=ValueError("Pinned detectors unavailable")
            ),
            patch("sys.argv", ["scanner_preflight", "--gitleaks", "", "--devskim", ""]),
            self.assertRaisesRegex(ValueError, "Pinned detectors unavailable"),
        ):
            preflight.main()
        with (
            patch.object(preflight, "ROOT", self.root),
            patch.object(preflight, "changed_paths", return_value=["source.py"]),
            patch.object(preflight, "validate_report_directory"),
            patch.object(preflight, "require_versions", return_value={}),
            patch.object(Path, "is_junction", return_value=True),
            patch("sys.argv", ["scanner_preflight", "--gitleaks", "gitleaks", "--devskim", "devskim"]),
            self.assertRaisesRegex(ValueError, "output crosses a reparse point"),
        ):
            preflight.main()

    # Purpose: Detect source drift between review-context and detector snapshots before either scanner runs.
    # Inputs: One real source file with a deliberately mismatched frozen review digest.
    # Outputs: Main rejects the split snapshot without starting a detector.
    def test_main_rejects_snapshot_drift(self):
        (self.root / "source.py").write_text("owned source", encoding="utf-8")
        with (
            patch.object(preflight, "ROOT", self.root),
            patch.object(preflight, "changed_paths", return_value=["source.py"]),
            patch.object(preflight, "validate_report_directory"),
            patch.object(preflight, "require_versions", return_value={}),
            patch.object(preflight, "freeze_current_reviews", return_value=([], {"source.py": "0" * 64})),
            patch.object(preflight, "read_policy", return_value=b"policy"),
            patch.object(preflight, "read_source_policy", return_value=b"source policy"),
            patch("sys.argv", ["scanner_preflight", "--gitleaks", "gitleaks", "--devskim", "devskim"]),
            self.assertRaisesRegex(ValueError, "Source changed between detector and review snapshots"),
        ):
            preflight.main()

    # Purpose: Exercise the real CLI failure boundary without requiring either external detector.
    # Inputs: An invalid Git comparison base handled by the module entry point.
    # Outputs: Exit one and a controlled diagnostic without traceback.
    def test_real_module_entry_point_reports_controlled_failure(self):
        completed = subprocess.run(
            [sys.executable, "-m", "tools.scanner_preflight", "--base=not-a-complete-ref"],
            cwd=Path(__file__).resolve().parents[1],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        self.assertEqual(completed.returncode, 1)
        self.assertIn("Scanner preflight failed:", completed.stderr)
        self.assertNotIn("Traceback", completed.stderr)


if __name__ == "__main__":
    unittest.main()
