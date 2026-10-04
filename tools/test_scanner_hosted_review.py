"""Negative controls for individually approved hosted finding admission."""

import copy
import csv
import hashlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import scanner_hosted_review as review


class HostedReviewTests(unittest.TestCase):
    # Purpose: Create independent source/configuration fixtures without a build, scanner or network request.
    # Inputs: Approved schema and records, with synthetic bytes. Outputs: Owned isolated checkout-like fixture.
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.rows = review.read_hosted_policy(Path(__file__).resolve().parents[1])
        for row in self.rows:
            source = self.root / row["path"]
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(b"independent source fixture\n")
            row["input_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
        for name in review.CONFIGURATION:
            source = self.root / name
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(b"independent analysis configuration\n")
        self.capture = patch.object(review, "capture_inputs", return_value={"inputs_sha256": "a" * 64})
        self.capture.start()
        self.addCleanup(self.capture.stop)
        context = review.context_digest(self.root)
        for row in self.rows:
            row["context_sha256"] = context
        approval = self.root / review.APPROVAL
        approval.parent.mkdir(exist_ok=True)
        approval.write_text(
            "scope,records_sha256,evidence\n"
            + "hosted-individual-identities-v1,"
            + review.approval_digest(self.rows)
            + ","
            + review.EVIDENCE
            + "\n",
            encoding="utf-8",
        )
        self.write_policy(self.rows)
        self.commit = "b" * 40

    # Purpose: Serialize a fixture ledger using the public data format, not production admission logic.
    # Inputs: Explicit records. Outputs: Bounded CSV at the fixture's policy location.
    def write_policy(self, rows):
        text = io.StringIO(newline="")
        writer = csv.DictWriter(text, fieldnames=review.FIELDS, quoting=csv.QUOTE_ALL)
        writer.writeheader()
        writer.writerows(rows)
        path = self.root / review.POLICY
        path.parent.mkdir(exist_ok=True)
        path.write_text(text.getvalue(), encoding="utf-8")

    # Purpose: Construct the actual hosted API shape independently from the source-review implementation.
    # Inputs: Explicit approved record. Outputs: One raw, open alert bound to the fixture commit.
    def alert(self, row):
        return {
            "number": int(row["alert"]),
            "state": "open",
            "tool": {"name": row["tool"], "version": row["version"]},
            "rule": {"id": row["rule"]},
            "most_recent_instance": {
                "commit_sha": self.commit,
                "category": row["category"],
                "location": {
                    "path": row["path"],
                    "start_line": int(row["startLine"]),
                    "start_column": int(row["startColumn"]),
                    "end_line": int(row["endLine"]),
                    "end_column": int(row["endColumn"]),
                },
            },
        }

    # Purpose: Retain each of the 54 individual identities and the complete raw state.
    # Inputs: All independently constructed alerts. Outputs: Exact approved IDs without report mutation.
    def test_complete_inventory_keeps_raw_findings(self):
        alerts = [self.alert(row) for row in self.rows]
        before = copy.deepcopy(alerts)
        with patch.object(review, "committed_context_matches", return_value=True):
            self.assertEqual(
                review.match_historical_hosted_alerts(self.root, alerts, self.commit, self.rows),
                [int(row["alert"]) for row in self.rows],
            )
        self.assertEqual(alerts, before)
        self.assertEqual(len(review.read_hosted_policy(self.root)), 54)

    # Purpose: Reject all independent scanner, incident, source, rule and coordinate freshness changes.
    # Inputs: Mutations of an approved critical report. Outputs: No changed report is admitted.
    def test_each_hosted_identity_boundary(self):
        row = self.rows[0]
        original = self.alert(row)
        mutations = [
            ("tool", "name", "other"),
            ("tool", "version", "2.27.2"),
            ("rule", "id", "cpp/other"),
            ("most_recent_instance", "category", "other"),
            ("most_recent_instance", "commit_sha", "c" * 40),
        ]
        with patch.object(review, "committed_context_matches", return_value=True):
            for container, key, value in mutations:
                alert = copy.deepcopy(original)
                alert[container][key] = value
                self.assertEqual(review.match_historical_hosted_alerts(self.root, [alert], self.commit, self.rows), [])
            for key in ("path", "start_line", "start_column", "end_line", "end_column"):
                alert = copy.deepcopy(original)
                loc = alert["most_recent_instance"]["location"]
                loc[key] = "src/unreviewed.cpp" if key == "path" else loc[key] + 1
                self.assertEqual(review.match_historical_hosted_alerts(self.root, [alert], self.commit, self.rows), [])
            for key, value in (("number", 999999), ("state", "dismissed"), ("number", True)):
                alert = {**copy.deepcopy(original), key: value}
                self.assertEqual(review.match_historical_hosted_alerts(self.root, [alert], self.commit, self.rows), [])

    # Purpose: Expire sink approvals when unreported generated callers, native callers or analysis configuration change.
    # Inputs: Independent caller/configuration mutations with unchanged incident locations. Outputs: No stale approval.
    def test_caller_and_configuration_expiry(self):
        alerts = [self.alert(self.rows[0])]
        with patch.object(review, "committed_context_matches", return_value=True):
            self.assertNotEqual(review.match_historical_hosted_alerts(self.root, alerts, self.commit, self.rows), [])
            caller = self.root / review.DERIVED_ROOT / "dictBuilder/zdict.c"
            caller.write_bytes(b"unreported generated caller changed\n")
            self.assertEqual(review.match_historical_hosted_alerts(self.root, alerts, self.commit, self.rows), [])
            caller.unlink()
            with patch.object(review, "capture_inputs", return_value={"inputs_sha256": "c" * 64}):
                self.assertEqual(review.match_historical_hosted_alerts(self.root, alerts, self.commit, self.rows), [])
            (self.root / review.CONFIGURATION[0]).write_bytes(b"query suite changed\n")
            self.assertEqual(review.match_historical_hosted_alerts(self.root, alerts, self.commit, self.rows), [])

    # Purpose: Expire one source independently even when its caller context is externally held constant.
    # Inputs: Changed reviewed bytes with a mocked matching context. Outputs: Its exact report remains blocking.
    def test_reviewed_source_hash_is_required(self):
        row = self.rows[0]
        with (
            patch.object(review, "context_digest", return_value=row["context_sha256"]),
            patch.object(review, "committed_context_matches", return_value=True),
        ):
            (self.root / row["path"]).write_bytes(b"changed reviewed source\n")
            self.assertEqual(
                review.match_historical_hosted_alerts(self.root, [self.alert(row)], self.commit, self.rows), []
            )

    # Purpose: Require published inputs; reject unavailable Git, malformed commits and duplicate records.
    # Inputs: Negative producer/inventory controls. Outputs: No stale or ambiguous admission succeeds.
    def test_publication_and_inventory_boundaries(self):
        alert = self.alert(self.rows[0])
        with patch.object(review, "committed_context_matches", return_value=False):
            self.assertEqual(review.match_historical_hosted_alerts(self.root, [alert], self.commit, self.rows), [])
        with patch.object(review, "committed_context_matches", return_value=True), self.assertRaises(ValueError):
            review.match_historical_hosted_alerts(self.root, [alert, alert], self.commit, self.rows)
        with self.assertRaises(ValueError):
            review.match_historical_hosted_alerts(self.root, [], "HEAD", self.rows)
        with patch.object(review.subprocess, "run") as run:
            run.return_value.returncode = 128
            with self.assertRaises(ValueError):
                review.committed_context_matches(self.root, self.commit)

    # Purpose: Reject ledger broadening, omitted decisions, unknown schema and redirected evidence files.
    # Inputs: Independent invalid records and a simulated junction. Outputs: Policy admission fails.
    def test_policy_cannot_expand_or_hide_findings(self):
        for field, value in (
            ("path", "src/unreviewed.cpp"),
            ("rule", "cpp/other"),
            ("version", "new-scanner"),
            ("evidence", "docs/unreviewed.md"),
            ("endLine", "0"),
            ("disposition", "accepted-without-review"),
            ("alert", "999999"),
            ("startLine", str(int(self.rows[0]["startLine"]) + 1)),
            ("input_sha256", "f" * 64),
        ):
            rows = copy.deepcopy(self.rows)
            rows[0][field] = value
            self.write_policy(rows)
            with self.assertRaises(ValueError):
                review.read_hosted_policy(self.root)
        for rows in (self.rows[:-1], self.rows + [self.rows[0]], self.rows[:-1] + [self.rows[0]]):
            self.write_policy(rows)
            with self.assertRaises(ValueError):
                review.read_hosted_policy(self.root)
        with patch.object(Path, "is_junction", return_value=True), self.assertRaises(ValueError):
            review.derived_sources(self.root)

    # Purpose: Reject malformed provenance pins rather than treating public metadata as an admission bypass.
    # Inputs: Independent schema, inventory, scope and digest mutations. Outputs: No invalid pin is admitted.
    def test_approval_provenance_is_bounded_and_exact(self):
        path = self.root / review.APPROVAL
        original = path.read_text(encoding="utf-8")
        for payload in (
            original + original,
            original.replace("records_sha256", "unknown"),
            original.replace("hosted-individual-identities-v1", "all-findings"),
            original.replace(review.EVIDENCE, "docs/unreviewed.md"),
            original + "x" * 1024,
            original.replace(review.approval_digest(self.rows), "invalid"),
        ):
            path.write_text(payload, encoding="utf-8")
            with self.assertRaises(ValueError):
                review.read_approval_digest(self.root)


if __name__ == "__main__":
    unittest.main()
