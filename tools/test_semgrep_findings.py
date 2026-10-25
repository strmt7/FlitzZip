"""Exact-source Semgrep gate controls using a committed, isolated fixture."""

from __future__ import annotations

import copy
import csv
import hashlib
import io
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools import scanner_hosted_review as review
from tools.semgrep_findings import admit_scan


class SemgrepFindingGateTests(unittest.TestCase):
    # Purpose: Build a real committed scanner decision and two same-run raw reports.
    # Inputs: Isolated Git checkout with source, scanner recipe and review evidence.
    # Outputs: A valid exact decision that can be mutated independently by each test.
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = "tools/downloader.py"
        self.context = sorted((*review.SEMGREP_CONFIGURATION, self.source, "docs/review.md"))
        for name in self.context:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"### Alert 17\nReviewed transport.\n" if name.startswith("docs/") else b"fixture input\n")
        self.row = dict(
            repository="fixture/repository",
            alert="17",
            tool="Semgrep OSS",
            version="1.181.0",
            category="semgrep",
            path=self.source,
            rule="python.lang.security.audit.dynamic-urllib-use-detected.dynamic-urllib-use-detected",
            start_line="3",
            start_column="5",
            end_line="3",
            end_column="18",
            input_sha256="",
            context_paths="|".join(self.context),
            context_sha256="",
            evidence="docs/review.md#alert-17",
        )
        hashes = {name: hashlib.sha256((self.root / name).read_bytes()).hexdigest() for name in self.context}
        self.row["input_sha256"] = hashes[self.source]
        self.row["context_sha256"] = hashlib.sha256(review.canonical(hashes)).hexdigest()
        output = io.StringIO(newline="")
        writer = csv.DictWriter(output, fieldnames=review.CURRENT_FIELDS, lineterminator="\n")
        writer.writeheader()
        writer.writerow(self.row)
        policy = self.root / review.CURRENT_POLICY
        policy.parent.mkdir(parents=True, exist_ok=True)
        policy.write_text(output.getvalue(), encoding="utf-8", newline="\n")
        self.git("init", "-q")
        self.git("add", ".")
        self.git("commit", "-qm", "Exact Semgrep fixture")
        self.commit = self.git("rev-parse", "HEAD")
        self.finding = {
            "check_id": self.row["rule"],
            "path": self.source,
            "start": {"line": 3, "col": 5},
            "end": {"line": 3, "col": 18},
        }
        self.published = {
            "ruleId": self.row["rule"],
            "locations": [
                {
                    "physicalLocation": {
                        "artifactLocation": {"uri": self.source.replace("/", "\\"), "uriBaseId": "%SRCROOT%"},
                        "region": {"startLine": 3, "startColumn": 5, "endLine": 3, "endColumn": 18},
                    }
                }
            ],
        }
        self.report = {"version": "1.181.0", "results": [self.finding]}
        self.sarif = {
            "version": "2.1.0",
            "runs": [
                {"tool": {"driver": {"name": "Semgrep OSS", "semanticVersion": "1.181.0"}}, "results": [self.published]}
            ],
        }

    # Purpose: Run Git with fixture-local identity and no user configuration changes.
    # Inputs: Git argument vector. Outputs: Captured stdout or a test failure.
    def git(self, *arguments):
        return subprocess.run(
            [
                "git",
                "-c",
                "core.autocrlf=false",
                "-c",
                "user.name=Fixture",
                "-c",
                "user.email=test@example.invalid",
                *arguments,
            ],
            cwd=self.root,
            check=True,
            capture_output=True,
            text=True,
            timeout=15,
        ).stdout.strip()

    # Purpose: Exercise the exact committed review without changing either raw report.
    # Inputs: Optional independent JSON/SARIF/status changes. Outputs: Admission or refusal.
    def admit(self, report=None, sarif=None, status=1):
        return admit_scan(
            self.root,
            self.commit,
            "fixture/repository",
            self.report if report is None else report,
            self.sarif if sarif is None else sarif,
            status,
        )

    # Purpose: Preserve a finding while allowing only its exact reviewed identity.
    # Inputs: Real committed source/review and paired scanner documents. Outputs: One alert ID; raw reports unchanged.
    def test_exact_review_admits_only_the_original_raw_result(self):
        original = copy.deepcopy((self.report, self.sarif))
        self.assertEqual(self.admit(), [17])
        self.assertEqual((self.report, self.sarif), original)

    # Purpose: Keep every changed or additional detection blocking after the original admission.
    # Inputs: New rule/location in both scanner outputs or a duplicate. Outputs: Refusal, never partial acceptance.
    def test_new_changed_and_duplicate_findings_block(self):
        for field, value in (
            ("check_id", "python/new-rule"),
            ("path", "tools/other.py"),
            ("end", {"line": 3, "col": 19}),
        ):
            report, sarif = copy.deepcopy((self.report, self.sarif))
            report["results"][0][field] = value
            if field == "check_id":
                sarif["runs"][0]["results"][0]["ruleId"] = value
            elif field == "path":
                sarif["runs"][0]["results"][0]["locations"][0]["physicalLocation"]["artifactLocation"]["uri"] = value
            else:
                sarif["runs"][0]["results"][0]["locations"][0]["physicalLocation"]["region"]["endColumn"] = 19
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.admit(report, sarif)
        report, sarif = copy.deepcopy((self.report, self.sarif))
        report["results"].append(copy.deepcopy(self.finding))
        sarif["runs"][0]["results"].append(copy.deepcopy(self.published))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.admit(report, sarif)

    # Purpose: Reject incomplete, mismatched or failed scanner output rather than infer a clean scan.
    # Inputs: Divergent JSON/SARIF, wrong exit, tool version or URI base. Outputs: Explicit refusal.
    def test_scanner_identity_and_report_integrity_fail_closed(self):
        with self.assertRaisesRegex(ValueError, "exit"):
            self.admit(status=0)
        with self.assertRaisesRegex(ValueError, "exit"):
            self.admit(status=2)
        report = copy.deepcopy(self.report)
        report["results"] = []
        with self.assertRaisesRegex(ValueError, "counts"):
            self.admit(report=report)
        sarif = copy.deepcopy(self.sarif)
        sarif["runs"][0]["tool"]["driver"]["semanticVersion"] = "1.182.0"
        with self.assertRaisesRegex(ValueError, "identities"):
            self.admit(sarif=sarif)
        sarif = copy.deepcopy(self.sarif)
        sarif["version"] = "2.0.0"
        with self.assertRaisesRegex(ValueError, "identity"):
            self.admit(sarif=sarif)
        sarif = copy.deepcopy(self.sarif)
        sarif["runs"][0]["results"][0]["locations"][0]["physicalLocation"]["artifactLocation"]["uriBaseId"] = "file:///"
        with self.assertRaisesRegex(ValueError, "repository-relative"):
            self.admit(sarif=sarif)

    # Purpose: Expire approval on any edited source, evidence or scanner recipe.
    # Inputs: One live file changed after the reviewed commit. Outputs: The prior review cannot pass.
    def test_live_source_and_configuration_mutations_expire_review(self):
        for name in (self.source, "docs/review.md", ".github/workflows/security-code-scanning.yml"):
            path = self.root / name
            original = path.read_bytes()
            try:
                path.write_bytes(original + b"changed\n")
                with self.subTest(name=name), self.assertRaisesRegex(ValueError, "stale"):
                    self.admit()
            finally:
                path.write_bytes(original)


if __name__ == "__main__":
    unittest.main()
