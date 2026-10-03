"""Exact-public-metadata review must never admit code, stale inputs or unrelated findings."""

import csv
import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from tools import scanner_metadata_review as review


class ScannerMetadataReviewTests(unittest.TestCase):
    # Purpose: Isolate reviewed configuration and complete raw findings.
    # Inputs: None. Outputs: Owned temporary tree and canonical CSV fixture.
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.name = "docs/benchmarks/corpora/source-pin.json"
        self.path = self.root / self.name
        self.path.parent.mkdir(parents=True)
        self.value = hashlib.sha256(b"public noncredential fixture").hexdigest()
        self.payload = '{"archive_sha256": "' + self.value + '"}\n'
        self.path.write_bytes(self.payload.encode("utf-8"))
        stream = io.StringIO()
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(review.FIELDS)
        writer.writerow(
            (
                self.name,
                "DS173237",
                hashlib.sha256(self.path.read_bytes()).hexdigest(),
                self.value,
                "docs/security-code-scanning.md#finding-triage",
            )
        )
        self.policy = stream.getvalue().encode()
        self.finding = {
            "ruleId": "DS173237",
            "locations": [
                {
                    "physicalLocation": {
                        "artifactLocation": {"uri": self.name},
                        "region": {"charOffset": self.payload.index('"' + self.value), "charLength": 66},
                    }
                }
            ],
        }
        self.report = self.root / "raw.sarif"

    # Purpose: Run the production reviewer without removing any raw result.
    # Inputs: Findings to serialize. Outputs: Reviewer result and unchanged raw bytes.
    def evaluate(self, findings):
        self.report.write_text(json.dumps({"runs": [{"results": findings}]}), encoding="utf-8")
        before = self.report.read_bytes()
        result = review.review_findings(self.root, self.report, self.policy)
        self.assertEqual(self.report.read_bytes(), before)
        return result

    # Purpose: Admit only the reviewed snapshot while retaining unrelated rules and all raw results.
    # Inputs: One known metadata match and one actual-code diagnostic. Outputs: One reviewed and one unresolved.
    def test_exact_review_and_lossless_report(self):
        result = self.evaluate([self.finding, {"ruleId": "actual-code-diagnostic"}])
        self.assertEqual(len(result["reviewed_metadata"]), 1)
        self.assertEqual(result["unresolved_count"], 1)

    # Purpose: Prevent a reviewed checksum from hiding changed contents or an unrelated flagged value.
    # Inputs: Modified file, changed region and different rule. Outputs: Every mutation stays unresolved.
    def test_stale_snapshot_and_finding_boundaries(self):
        self.path.write_text(self.payload + '"api_key": "new credential"', encoding="utf-8")
        self.assertEqual(self.evaluate([self.finding])["unresolved_count"], 1)
        self.path.write_text(self.payload, encoding="utf-8")
        for modified in (
            {**self.finding, "ruleId": "DS117838"},
            {**self.finding, "locations": []},
            {**self.finding, "locations": self.finding["locations"] * 2},
        ):
            self.assertEqual(self.evaluate([modified])["unresolved_count"], 1)
        self.finding["locations"][0]["physicalLocation"]["region"]["charOffset"] += 1
        self.assertEqual(self.evaluate([self.finding])["unresolved_count"], 1)

    # Purpose: Reject broad, duplicate, malformed or source-code review policy entries.
    # Inputs: Deliberately invalid CSV records. Outputs: Every invalid policy fails closed.
    def test_policy_admission(self):
        for payload in (
            self.policy.replace(self.name.encode(), b"src/parser.cpp"),
            self.policy.replace(self.name.encode(), b"tests/fixtures/sample.json"),
            self.policy.replace(b"DS173237", b"actual-code-diagnostic"),
            self.policy.replace(b"#finding-triage", b"#unreviewed"),
            self.policy + self.policy.splitlines(keepends=True)[1],
            b"unknown\nvalue\n",
            b"x" * (review.MAX_POLICY_BYTES + 1),
        ):
            with self.assertRaises(ValueError):
                review.read_reviews(payload)

    # Purpose: Keep review identity portable across Git newline normalization without changing scanner offsets.
    # Inputs: The identical reviewed text with Windows newlines. Outputs: Exact public checksum remains reviewed.
    def test_newline_normalization(self):
        self.path.write_bytes(self.payload.replace("\n", "\r\n").encode())
        self.assertEqual(len(self.evaluate([self.finding])["reviewed_metadata"]), 1)

    # Purpose: Refuse redirected policy files and detect accidental drift in registered metadata snapshots.
    # Inputs: Real reviewed files plus a mocked junction. Outputs: Current hashes match and redirects fail.
    def test_policy_files_and_registered_snapshots(self):
        from unittest.mock import patch

        root = Path(__file__).resolve().parents[1]
        for row in review.read_reviews(review.read_policy(root)):
            digest = hashlib.sha256((root / row["path"]).read_bytes().replace(b"\r\n", b"\n")).hexdigest()
            self.assertEqual(digest, row["input_sha256"], "Metadata changed: require a fresh provenance review")
        with patch.object(Path, "is_junction", return_value=True), self.assertRaises(ValueError):
            review.read_policy(root)


if __name__ == "__main__":
    unittest.main()
