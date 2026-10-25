"""Negative controls for individually approved hosted finding admission."""

import copy
import csv
import hashlib
import io
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

from tools import scanner_hosted_review as review


class HostedReviewTests(unittest.TestCase):
    def test_source_digest_budget_and_mid_read_replacement(self):
        """Purpose: Refuse oversized or concurrently replaced review inputs. Inputs: Private regular source bytes.
        Outputs: No stale/mixed digest is returned and a complete stable retry succeeds.
        """
        name = self.rows[0]["path"]
        path = (self.root / name).resolve()
        original = path.read_bytes()
        path.write_bytes(b"x" * (2 * 1024**2 + 1))
        with self.assertRaisesRegex(ValueError, "exceeds its byte budget"):
            review.source_digest(self.root, name)
        path.write_bytes(original)
        read_bytes = Path.read_bytes

        # Purpose: Replace real bytes after read completion but before the reader's final identity observation.
        # Inputs: Actual canonical reader path. Outputs: Original payload followed by one owned source mutation.
        def replaced(selected):
            payload = read_bytes(selected)
            if selected == path:
                selected.write_bytes(payload + b"changed during read")
            return payload

        with patch.object(Path, "read_bytes", replaced), self.assertRaisesRegex(ValueError, "changed while hashing"):
            review.source_digest(self.root, name)
        path.write_bytes(original)
        self.assertEqual(
            review.source_digest(self.root, name), hashlib.sha256(original.replace(b"\r\n", b"\n")).hexdigest()
        )

    def test_generated_inventory_ignores_data_and_bounds_source_count(self):
        """Purpose: Bound generated caller inspection without allowing data files to redefine source context.
        Inputs: Actual ignored data file and controlled over-budget filesystem enumeration.
        Outputs: Stable source map for data-only changes and refusal before hashing an excess source.
        """
        original = review.derived_sources(self.root)
        (self.root / review.DERIVED_ROOT / "metadata.fixture").write_bytes(b"nonexecutable data")
        self.assertEqual(review.derived_sources(self.root), original)
        walk = [(str(self.root / review.DERIVED_ROOT), [], [f"source-{index}.c" for index in range(513)])]
        with (
            patch.object(review.os, "walk", return_value=iter(walk)),
            patch.object(review, "source_digest", return_value="a" * 64) as hash_source,
            self.assertRaisesRegex(ValueError, "source inventory budget"),
        ):
            review.derived_sources(self.root)
        self.assertEqual(hash_source.call_count, 512)

    def test_legacy_ledger_budget_schema_and_scope_are_explicit(self):
        """Purpose: Keep historical records within their exact approved schema and source scope.
        Inputs: Oversized data, wrong header, invalid digest and an allocation review moved to a different source.
        Outputs: Each malformed record fails before any context or acceptance lookup.
        """
        path = self.root / review.POLICY
        for payload, message in (
            (b"x" * (review.MAX_POLICY + 1), "exceeds its byte budget"),
            (b"unknown\nvalue\n", "unknown schema"),
        ):
            path.write_bytes(payload)
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                review.read_hosted_policy(self.root)
        row = dict(self.rows[0])
        row["input_sha256"] = "g" * 64
        self.write_policy([row])
        with self.assertRaisesRegex(ValueError, "exact source and caller-context digests"):
            review.read_hosted_policy(self.root)
        row = dict(next(item for item in self.rows if item["tool"] == "devskim"))
        row["path"] = self.rows[0]["path"]
        self.write_policy([row])
        with self.assertRaisesRegex(ValueError, "approved serial fixture"):
            review.read_hosted_policy(self.root)

    def test_committed_context_transport_status_is_not_assumed_successful(self):
        """Purpose: Preserve Git's changed-versus-unavailable distinction. Inputs: Actual subprocess status protocol.
        Outputs: Only exact success returns true; changed and failed context remain unaccepted.
        """
        for code in (0, 1, 2):
            with self.subTest(code=code), patch.object(review.subprocess, "run") as git:
                git.return_value.returncode = code
                if code == 2:
                    with self.assertRaisesRegex(ValueError, "cannot be verified"):
                        review.committed_context_matches(self.root, self.commit)
                else:
                    self.assertEqual(review.committed_context_matches(self.root, self.commit), code == 0)

    def test_irrelevant_historical_alerts_do_not_require_generated_native_sources(self):
        """Purpose: Avoid build prerequisites when no exact historical review can match.
        Inputs: Empty, unknown-ID, closed and wrong-tool inventories. Outputs: No context lookup or source admission.
        """
        row = self.rows[0]
        valid = self.alert(row)
        for alerts in (
            [],
            [{"number": -1, "state": "open"}],
            [{**valid, "state": "closed"}],
            [{**valid, "tool": {"name": "different"}}],
        ):
            with (
                self.subTest(alerts=alerts),
                patch.object(review, "context_digest", side_effect=AssertionError("unrelated alert required a build")),
            ):
                self.assertEqual(review.match_historical_hosted_alerts(self.root, alerts, self.commit, [row]), [])
        with (
            patch.object(review, "context_digest", side_effect=ValueError("required exact source context")),
            self.assertRaisesRegex(ValueError, "required exact source context"),
        ):
            review.match_historical_hosted_alerts(self.root, [valid], self.commit, [row])

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


class CurrentFalsePositiveTests(unittest.TestCase):
    def test_current_policy_type_geometry_and_scope_boundaries(self):
        """Purpose: Reject malformed individual decisions before any source lookup.
        Inputs: Independent identity, producer, digest, path, coordinate and caller-list mutations.
        Outputs: Explicit structural errors; the original valid policy remains unchanged.
        """
        mutations = (
            ("version", "", "record"),
            ("repository", "owner/../repository", "repository"),
            ("alert", "0", "alert or location"),
            ("start_column", "-1", "alert or location"),
            ("end_column", "0", "Reversed"),
            ("input_sha256", "g" * 64, "scanner or digest"),
            ("tool", "unreviewed", "scanner or digest"),
            ("path", "../outside.c", "review path"),
            ("path", "file:///outside.c", "review path"),
            ("context_paths", "|".join(reversed(self.context)), "caller context"),
        )
        original = (self.root / review.CURRENT_POLICY).read_bytes()
        for field, value, message in mutations:
            stream = io.StringIO()
            writer = csv.DictWriter(stream, fieldnames=review.CURRENT_FIELDS)
            writer.writeheader()
            writer.writerow({**self.row, field: value})
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, message):
                review.read_current_reviews(stream.getvalue().encode())
        self.assertEqual((self.root / review.CURRENT_POLICY).read_bytes(), original)

    def test_committed_blob_requires_full_revision_and_consistent_native_size(self):
        """Purpose: Reject ambiguous revisions and truncated Git blob transport.
        Inputs: A ref spelling and independently framed regular-file metadata with a shorter body.
        Outputs: No unverified bytes can enter source matching; raw protocol failure remains explicit.
        """
        with patch.object(review.subprocess, "run") as git:
            with self.assertRaisesRegex(ValueError, "full commit"):
                review.committed_review_blob(self.root, "HEAD", "src/example.c")
            git.assert_not_called()
            git.side_effect = [
                SimpleNamespace(stdout=b"100644 blob " + b"a" * 40 + b" 100\tsrc/example.c\0"),
                SimpleNamespace(stdout=b"short"),
            ]
            with self.assertRaisesRegex(ValueError, "blob size changed"):
                review.committed_review_blob(self.root, self.commit, "src/example.c")
            self.assertEqual(git.call_count, 2)

    def test_unknown_category_and_malformed_locations_cannot_use_a_review(self):
        """Purpose: Keep incomplete raw scanner evidence blocking without looking up unrelated source context.
        Inputs: Missing category, malformed SARIF locations and invalid inventory/revision declarations.
        Outputs: No accepted report or source read; invalid bounded-inventory declarations are rejected.
        """
        alert = copy.deepcopy(self.alert)
        del alert["most_recent_instance"]["category"]
        self.assertEqual(self.match([alert]), [])
        report = {"runs": [{"results": [{"locations": []}, {"locations": [{}, {}]}]}]}
        reader = Mock(side_effect=AssertionError("malformed report reached source lookup"))
        self.assertEqual(review.match_snapshot_false_positives([], report, "fixture/repository", reader), [])
        reader.assert_not_called()
        for alerts, commit in (([], "HEAD"), ([self.alert] * 20001, self.commit)):
            with (
                self.subTest(commit=commit, count=len(alerts)),
                self.assertRaisesRegex(ValueError, "bounded inventory"),
            ):
                review.match_current_false_positives(self.root, alerts, commit, "fixture/repository")

    # Purpose: Exercise committed review identity using an isolated real Git repository.
    # Inputs: Independent source, caller, configuration and reviewed incident fixtures.
    # Outputs: A complete valid decision with no network access or scanner/build invocation.
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.context = sorted((*review.CURRENT_CONFIGURATION, "src/example.c", "src/caller.c", "docs/review.md"))
        for name in self.context:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"### Alert 17\nreviewed fixture\n" if name.startswith("docs/") else b"fixture input\n")
        self.row = dict(
            repository="fixture/repository",
            alert="17",
            tool="CodeQL",
            version="test-version",
            category="/language:c-cpp",
            path="src/example.c",
            rule="cpp/test-rule",
            start_line="1",
            start_column="1",
            end_line="1",
            end_column="6",
            input_sha256="",
            context_sha256="",
            context_paths="|".join(self.context),
            evidence="docs/review.md#alert-17",
        )
        self.write_policy()
        self.git("init", "-q")
        self.commit = self.save_commit()
        self.alert = {
            "number": 17,
            "state": "open",
            "tool": {"name": "CodeQL", "version": "test-version"},
            "rule": {"id": "cpp/test-rule"},
            "most_recent_instance": {
                "category": "/language:c-cpp",
                "commit_sha": self.commit,
                "location": {
                    "path": "src/example.c",
                    "start_line": 1,
                    "start_column": 1,
                    "end_line": 1,
                    "end_column": 6,
                },
            },
        }

    # Purpose: Keep fixture Git configuration and commands local to the owned temporary repository.
    # Inputs: Literal argument vector. Outputs: Captured successful command output or a test failure.
    def git(self, *arguments):
        return subprocess.run(
            [
                "git",
                "-c",
                "core.autocrlf=false",
                "-c",
                "user.name=Review fixture",
                "-c",
                "user.email=fixture@example.invalid",
                *arguments,
            ],
            cwd=self.root,
            capture_output=True,
            text=True,
            check=True,
            timeout=15,
        ).stdout.strip()

    # Purpose: Commit the complete independent fixture for real blob and revision validation.
    # Inputs: Current fixture files. Outputs: New full Git commit identity.
    def save_commit(self):
        self.git("add", ".")
        self.git("commit", "-qm", "Independent review fixture")
        return self.git("rev-parse", "HEAD")

    # Purpose: Serialize an explicit review with hashes computed independently from the matcher.
    # Inputs: Current fixture bytes. Outputs: Complete current CSV with exact context evidence.
    def write_policy(self):
        hashes = {name: hashlib.sha256((self.root / name).read_bytes()).hexdigest() for name in self.context}
        self.row["input_sha256"] = hashes[self.row["path"]]
        encoded = json.dumps(hashes, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
        self.row["context_sha256"] = hashlib.sha256(encoded).hexdigest()
        output = io.StringIO(newline="")
        writer = csv.DictWriter(output, fieldnames=review.CURRENT_FIELDS, lineterminator="\n")
        writer.writeheader()
        writer.writerow(self.row)
        (self.root / review.CURRENT_POLICY).write_text(output.getvalue(), encoding="utf-8", newline="\n")

    # Purpose: Call the production matcher with fixture identities and complete raw reports.
    # Inputs: Optional report array, revision and repository. Outputs: Reviewed IDs from the real implementation.
    def match(self, alerts=None, commit=None, repository="fixture/repository"):
        return review.match_current_false_positives(
            self.root, [self.alert] if alerts is None else alerts, commit or self.commit, repository
        )

    # Purpose: Preserve reports and require real committed source evidence for open and dismissed states.
    # Inputs: A current decision and an unrelated report. Outputs: Only the reviewed ID, without mutation.
    def test_exact_match_keeps_raw_reports_and_other_findings(self):
        for state in ("open", "dismissed"):
            self.alert["state"] = state
            other = copy.deepcopy(self.alert)
            other["number"] = 18
            alerts = [self.alert, other]
            before = copy.deepcopy(alerts)
            self.assertEqual(self.match(alerts), [17])
            self.assertEqual(alerts, before)
        self.assertEqual(self.match(repository="another/repository"), [])

    # Purpose: Admit only a reviewed Semgrep report with its scanner and source context.
    # Inputs: Committed fixture, then rule/version/source/configuration mutations. Outputs: Exact identity or refusal.
    def test_semgrep_review_requires_complete_scanner_and_source_identity(self):
        source = self.root / "src/example.py"
        source.write_bytes(b"from urllib.request import urlopen\n")
        self.context = sorted((*review.SEMGREP_CONFIGURATION, "src/example.py", "docs/review.md"))
        for name in review.SEMGREP_CONFIGURATION:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"scanner fixture\n")
        self.row.update(
            tool="Semgrep OSS",
            version="1.181.0",
            category="semgrep",
            path="src/example.py",
            rule="python.lang.security.audit.dynamic-urllib-use-detected.dynamic-urllib-use-detected",
            context_paths="|".join(self.context),
        )
        self.alert.update(tool={"name": "Semgrep OSS", "version": "1.181.0"}, rule={"id": self.row["rule"]})
        self.alert["most_recent_instance"].update(category="semgrep")
        self.alert["most_recent_instance"]["location"]["path"] = self.row["path"]
        self.write_policy()
        self.commit = self.save_commit()
        self.alert["most_recent_instance"]["commit_sha"] = self.commit
        hashes = {name: hashlib.sha256((self.root / name).read_bytes()).hexdigest() for name in self.context}
        self.assertEqual(self.row["input_sha256"], hashes[self.row["path"]])
        self.assertEqual(self.row["context_sha256"], hashlib.sha256(review.canonical(hashes)).hexdigest())
        self.assertIn("### Alert 17", (self.root / "docs/review.md").read_text(encoding="utf-8"))
        self.assertTrue(review.current_context_matches(self.row, lambda name: (self.root / name).read_bytes()))
        self.assertEqual(self.match(), [17])
        self.alert["tool"]["version"] = "1.182.0"
        self.assertEqual(self.match(), [])
        self.alert["tool"]["version"] = "1.181.0"
        self.row["context_paths"] = "|".join(name for name in self.context if name != review.SEMGREP_CONFIGURATION[0])
        self.write_policy()
        with self.assertRaisesRegex(ValueError, "omits source or scanner"):
            review.read_current_reviews((self.root / review.CURRENT_POLICY).read_bytes())
        self.row["context_paths"] = "|".join(self.context)
        self.write_policy()
        source.write_bytes(b"changed source\n")
        self.assertEqual(self.match(commit=self.save_commit()), [])

    # Purpose: Prove scanner, location, rule and incident identity changes cannot inherit a review.
    # Inputs: Independent one-field mutations including malformed numeric coordinates.
    # Outputs: Every different finding remains unadmitted; duplicate reports fail closed.
    def test_report_identity_expiry(self):
        changes = [
            ("tool", "name", "devskim"),
            ("tool", "version", "next"),
            ("rule", "id", "cpp/other"),
            ("most_recent_instance", "category", "other"),
        ]
        for parent, key, value in changes:
            changed = copy.deepcopy(self.alert)
            changed[parent][key] = value
            self.assertEqual(self.match([changed]), [])
        for key, value in (
            ("path", "src/caller.c"),
            ("start_line", 2),
            ("end_line", 2),
            ("start_column", True),
            ("end_column", 7),
        ):
            changed = copy.deepcopy(self.alert)
            changed["most_recent_instance"]["location"][key] = value
            self.assertEqual(self.match([changed]), [])
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.match([self.alert, self.alert])

    # Purpose: Make independently reviewed CPU/HIP decisions stable under GitHub completion ordering.
    # Inputs: Two explicit category records and alternating latest-instance responses.
    # Outputs: Either reviewed instance matches; another category or changed location remains blocking.
    def test_explicit_analysis_categories_are_order_independent(self):
        other_category = "/language:c-cpp/configuration:hip-host"
        other = {**self.row, "category": other_category}
        output = io.StringIO(newline="")
        writer = csv.DictWriter(output, fieldnames=review.CURRENT_FIELDS, lineterminator="\n")
        writer.writeheader()
        writer.writerows([other, self.row])
        (self.root / review.CURRENT_POLICY).write_text(output.getvalue(), encoding="utf-8", newline="\n")
        self.commit = self.save_commit()
        for category in (self.row["category"], other_category, self.row["category"], other_category):
            self.alert["most_recent_instance"]["category"] = category
            self.assertEqual(self.match(), [17])
        self.alert["most_recent_instance"]["category"] = "/language:c-cpp/configuration:unreviewed"
        self.assertEqual(self.match(), [])
        self.alert["most_recent_instance"]["category"] = other_category
        self.alert["most_recent_instance"]["location"]["end_column"] += 1
        self.assertEqual(self.match(), [])
        self.alert["most_recent_instance"]["location"]["end_column"] -= 1
        self.alert["tool"]["version"] = "next-scanner"
        self.assertEqual(self.match(), [])

    # Purpose: Prevent category alternatives from introducing conflicting source or scanner decisions.
    # Inputs: A second category with each non-category field independently changed.
    # Outputs: Every ambiguous incident fails policy parsing instead of selecting one record by order.
    def test_conflicting_analysis_categories_are_rejected(self):
        for field, value in (
            ("input_sha256", "f" * 64),
            ("context_sha256", "e" * 64),
            ("version", "another-version"),
            ("rule", "cpp/another-rule"),
            ("end_column", "7"),
            ("path", "src/caller.c"),
            ("tool", "devskim"),
        ):
            other = {**self.row, "category": "/language:c-cpp/configuration:hip-host", field: value}
            output = io.StringIO(newline="")
            writer = csv.DictWriter(output, fieldnames=review.CURRENT_FIELDS, lineterminator="\n")
            writer.writeheader()
            writer.writerows([self.row, other])
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "Conflicting"):
                review.read_current_reviews(output.getvalue().encode("utf-8"))

    # Purpose: Expire decisions on source, caller, scanner configuration and evidence changes.
    # Inputs: Successive committed mutations without renewing the review.
    # Outputs: Every mutation keeps the existing report unresolved.
    def test_every_bound_input_expires_the_review(self):
        for name in self.context:
            path = self.root / name
            original = path.read_bytes()
            path.write_bytes(original + b"changed\n")
            commit = self.save_commit()
            self.assertEqual(self.match(commit=commit), [], name)
            path.write_bytes(original)

    # Purpose: Prevent renewed current evidence from accepting an analysis of different source bytes.
    # Inputs: Changed caller and renewed fixture review, but the original analysis revision.
    # Outputs: Old analysis stays unresolved; fresh matching analysis succeeds.
    def test_analysis_must_match_current_executable_context(self):
        (self.root / "src/caller.c").write_bytes(b"changed caller\n")
        self.write_policy()
        commit = self.save_commit()
        self.assertEqual(self.match(commit=commit), [])
        self.alert["most_recent_instance"]["commit_sha"] = commit
        self.assertEqual(self.match(commit=commit), [17])

    # Purpose: Accept a documentation-only revision without demanding an unchanged native rebuild.
    # Inputs: An unrelated documentation commit with the existing source/configuration identity.
    # Outputs: The current exact decision continues to match the qualified earlier analysis.
    def test_unrelated_documentation_does_not_expire_review(self):
        (self.root / "docs/unrelated.md").write_text("Unrelated documentation\n", encoding="utf-8")
        self.assertEqual(self.match(commit=self.save_commit()), [17])

    # Purpose: Bound aggregate review memory even when individual Git files fit their own limits.
    # Inputs: A valid fixture under an intentionally tiny aggregate budget.
    # Outputs: The review fails instead of admitting a partial evidence set.
    def test_aggregate_evidence_budget(self):
        with patch.object(review, "MAX_CURRENT_CACHE_BYTES", 1), self.assertRaisesRegex(ValueError, "aggregate"):
            self.match()

    # Purpose: Reject local policy substitution and incomplete per-incident evidence.
    # Inputs: Uncommitted ledger edits and then a committed missing evidence anchor.
    # Outputs: Neither can establish a reviewed decision.
    def test_uncommitted_ledger_and_missing_evidence_fail(self):
        policy = self.root / review.CURRENT_POLICY
        original = policy.read_bytes()
        policy.write_bytes(original + b"\n")
        with self.assertRaisesRegex(ValueError, "differs"):
            self.match()
        policy.write_bytes(original)
        (self.root / "docs/review.md").write_text("No individual decision\n", encoding="utf-8")
        self.write_policy()
        self.assertEqual(self.match(commit=self.save_commit()), [])

    # Purpose: Reject broadened policy schemas, duplicate identities, unsafe paths and incomplete binding.
    # Inputs: Mutated CSV records from an otherwise valid fixture.
    # Outputs: Each malformed policy fails closed before any alert admission.
    def test_malformed_policies_fail_closed(self):
        original = (self.root / review.CURRENT_POLICY).read_text(encoding="utf-8")
        for payload in (
            original.replace("input_sha256", "ignored_sha256"),
            original + original.splitlines()[-1] + "\n",
            original.replace("src/example.c", "../example.c"),
            original.replace("alert-17", "alert-18"),
            original.replace("tools/devskim_scope.py", "tools/unreviewed_scope.py"),
            original + "x" * review.MAX_POLICY,
        ):
            with self.subTest(payload=payload[:60]), self.assertRaises(ValueError):
                review.read_current_reviews(payload.encode("utf-8"))
        with self.assertRaisesRegex(ValueError, "regular committed"):
            review.committed_review_blob(self.root, self.commit, "src/missing.c")


if __name__ == "__main__":
    unittest.main()
