"""Exact-public-metadata review must never admit code, stale inputs or unrelated findings."""

import copy
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


class ScannerSourceReviewTests(unittest.TestCase):
    # Purpose: Bind regression fixtures to the approved ledger and real source without running detectors again.
    # Inputs: None. Outputs: Owned frozen source fixture and an independently constructed exact SARIF location.
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        checkout = Path(__file__).resolve().parents[1]
        self.policy = review.read_source_policy(checkout)
        self.rows = review.read_source_reviews(self.policy)
        self.row = self.rows[0]
        self.source = self.root / self.row["path"]
        self.source.parent.mkdir(parents=True)
        self.source.write_bytes((checkout / self.row["path"]).read_bytes())
        self.finding = {
            "ruleId": self.row["rule"],
            "locations": [
                {
                    "physicalLocation": {
                        "artifactLocation": {"uri": self.row["path"]},
                        "region": {key: int(self.row[key]) for key in review.SOURCE_FIELDS[3:7]},
                    }
                }
            ],
        }

    # Purpose: Preserve every raw result while admitting only the approved complete source/location identity.
    # Inputs: One approved finding and an unreviewed sibling. Outputs: One reviewed, one unresolved and identical SARIF.
    def test_lossless_exact_admission(self):
        report = self.root / "raw.sarif"
        report.write_text(json.dumps({"runs": [{"results": [self.finding, {"ruleId": "DS117838"}]}]}))
        before = report.read_bytes()
        result = review.review_findings(
            self.root, report, b",".join(key.encode() for key in review.FIELDS) + b"\n", self.policy
        )
        self.assertEqual((len(result["reviewed_source"]), result["unresolved_count"]), (1, 1))
        self.assertEqual(report.read_bytes(), before)
        self.assertEqual(
            review.review_findings(self.root, report, b",".join(key.encode() for key in review.FIELDS) + b"\n")[
                "unresolved_count"
            ],
            2,
        )

    # Purpose: Prevent the next iteration from inheriting a disposition for any changed byte, rule or coordinate.
    # Inputs: Independent source and finding mutations. Outputs: Every mutation remains unreviewed.
    def test_all_freshness_boundaries(self):
        self.assertIsNotNone(review.reviewed_source_finding(self.root, self.finding, self.rows))
        original = self.source.read_bytes()
        self.source.write_bytes(original + b"/* changed source */\n")
        self.assertIsNone(review.reviewed_source_finding(self.root, self.finding, self.rows))
        self.source.write_bytes(original)
        for key in review.SOURCE_FIELDS[3:7]:
            changed = copy.deepcopy(self.finding)
            changed["locations"][0]["physicalLocation"]["region"][key] += 1
            self.assertIsNone(review.reviewed_source_finding(self.root, changed, self.rows))
        for changed in (
            {**self.finding, "ruleId": "DS117838"},
            {**self.finding, "locations": []},
            {**self.finding, "locations": self.finding["locations"] * 2},
        ):
            self.assertIsNone(review.reviewed_source_finding(self.root, changed, self.rows))

    # Purpose: Keep Git newline normalization portable while refusing redirected review files or sources.
    # Inputs: Identical LF/CRLF source and simulated junctions. Outputs: Normalized identity passes; redirects raise.
    def test_newlines_and_redirects(self):
        from unittest.mock import patch

        payload = self.source.read_bytes().replace(b"\r\n", b"\n")
        for content in (payload, payload.replace(b"\n", b"\r\n")):
            self.source.write_bytes(content)
            self.assertIsNotNone(review.reviewed_source_finding(self.root, self.finding, self.rows))
        with patch.object(Path, "is_junction", return_value=True), self.assertRaises(ValueError):
            review.read_source_policy(self.root)
        with patch.object(Path, "is_junction", return_value=True), self.assertRaises(ValueError):
            review.reviewed_source_finding(self.root, self.finding, self.rows)

    # Purpose: Reject broader rules, paths, evidence, malformed regions and duplicate/excess approvals.
    # Inputs: Negative ledger controls. Outputs: Every unauthorized policy expansion fails closed.
    def test_policy_boundaries(self):
        for payload in (
            self.policy.replace(self.row["path"].encode(), b"src/parser.cpp"),
            self.policy.replace(self.row["rule"].encode(), b"DS117838"),
            self.policy.replace(review.SOURCE_EVIDENCE.encode(), b"docs/unreviewed.md"),
            self.policy + self.policy.splitlines(keepends=True)[1],
            b"unknown\nvalue\n",
            b"x" * (review.MAX_POLICY_BYTES + 1),
        ):
            with self.assertRaises(ValueError):
                review.read_source_reviews(payload)
        self.assertEqual(len(self.rows), 21)


if __name__ == "__main__":
    unittest.main()
