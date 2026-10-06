"""Exact-public-metadata review must never admit code, stale inputs or unrelated findings."""

import copy
import csv
import hashlib
import io
import json
import subprocess
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

    def test_approved_additional_metadata_roles(self):
        """Purpose: Bound approved role additions. Inputs: Exact paths and mutations. Outputs: Narrow admission."""
        original = self.name
        for name in (
            "tools/benchmark_permissions.json",
            "docs/licenses/development-notices.json",
            ".github/scanner-secret-reviews.json",
            ".github/scanner-secret-reviews-crawl4ai.json",
            ".github/scanner-secret-reviews-crawl4ai-tests.json",
        ):
            self.name = name
            self.path = self.root / name
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self.path.write_bytes(self.payload.encode())
            self.policy = self.policy.replace(original.encode(), name.encode())
            self.finding["locations"][0]["physicalLocation"]["artifactLocation"]["uri"] = name
            self.assertEqual(len(self.evaluate([self.finding])["reviewed_metadata"]), 1)
            self.path.write_text(self.payload + "changed", encoding="utf-8")
            self.assertEqual(self.evaluate([self.finding])["unresolved_count"], 1)
            self.path.write_bytes(self.payload.encode())
            changed = copy.deepcopy(self.finding)
            changed["locations"][0]["physicalLocation"]["region"]["charOffset"] += 1
            self.assertEqual(self.evaluate([changed])["unresolved_count"], 1)
            for other in (
                "tools/other_permissions.json",
                "docs/licenses/other-notices.json",
                "src/manifest.json",
                ".github/scanner-secret-reviews-unapproved.json",
                ".github/scanner-secret-reviews-crawl4ai-more.json",
            ):
                with self.assertRaises(ValueError):
                    review.read_reviews(self.policy.replace(name.encode(), other.encode()))
            original = name

    def test_typed_public_commit_is_exact_role_bound_and_raw_report_retained(self):
        """Purpose: Separate commit provenance from checksums. Inputs: Typed reviewed fixture. Outputs: Narrow match."""
        value = hashlib.sha256(b"public commit fixture").hexdigest()[:40]
        self.name = f"third_party/upstream/nltk/{value}/build.json"
        self.path = self.root / self.name
        self.path.parent.mkdir(parents=True)
        data = {
            "project": "nltk",
            "commit": value,
            "source": "nltk-source.zip",
            "url": "https://codeload.github.com/nltk/nltk/zip/" + value,
        }
        self.payload = json.dumps(data) + "\n"
        self.path.write_bytes(self.payload.encode("utf-8"))
        stream = io.StringIO()
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(review.TYPED_FIELDS)
        writer.writerow(
            (
                self.name,
                "DS173237",
                hashlib.sha256(self.path.read_bytes()).hexdigest(),
                value,
                "git_commit",
                "docs/security-code-scanning.md#finding-triage",
            )
        )
        self.policy = stream.getvalue().encode()
        self.finding["locations"][0]["physicalLocation"] = {
            "artifactLocation": {"uri": self.name},
            "region": {"charOffset": self.payload.index('"' + value + '"'), "charLength": len(value) + 2},
        }
        self.assertEqual(len(self.evaluate([self.finding])["reviewed_metadata"]), 1)
        for replacement in (b"unknown_kind", b"sha256"):
            with self.assertRaises(ValueError):
                review.read_reviews(self.policy.replace(b"git_commit", replacement))
        with self.assertRaises(ValueError):
            review.read_reviews(self.policy.replace(self.name.encode(), b"src/manifest.json"))
        self.path.write_text(self.payload + " ", encoding="utf-8")
        self.assertEqual(self.evaluate([self.finding])["unresolved_count"], 1)
        original_digest = hashlib.sha256(self.payload.encode()).hexdigest().encode()
        self.path.write_bytes(
            self.payload.replace("codeload.github.com/nltk/nltk", "invalid.example/unrelated").encode("utf-8")
        )
        self.policy = self.policy.replace(original_digest, hashlib.sha256(self.path.read_bytes()).hexdigest().encode())
        self.assertEqual(self.evaluate([self.finding])["unresolved_count"], 1)

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

    def test_model_commit_requires_exact_official_metadata_role(self):
        """Purpose: Bind model provenance. Inputs: Typed roles and independent mutations. Outputs: Closed match."""
        value = hashlib.sha256(b"public model commit fixture").hexdigest()[:40]
        roles = (
            {
                "Pythia14M": {
                    "revision": value,
                    "source_kind": "file",
                    "file_name": "model.safetensors",
                    "url": f"https://huggingface.co/EleutherAI/pythia-14m/resolve/{value}/model.safetensors",
                }
            },
            {
                "subjects": {
                    "Pythia14M": {
                        "versions": [value],
                        "evidence": [
                            "https://github.com/EleutherAI/pythia",
                            "https://huggingface.co/EleutherAI/pythia-14m",
                        ],
                    }
                }
            },
        )
        for name, source in zip(review.CORPUS_COMMIT_PATHS, roles, strict=True):
            payload = (json.dumps(source) + "\n").encode()
            row = {
                "path": name,
                "rule": "DS173237",
                "input_sha256": hashlib.sha256(payload).hexdigest(),
                "public_value": value,
                "value_kind": "git_commit",
                "evidence": "docs/security-code-scanning.md#finding-triage",
            }
            region = {"charOffset": payload.decode().index('"' + value + '"'), "charLength": 42}
            stream = io.StringIO()
            writer = csv.DictWriter(stream, fieldnames=review.TYPED_FIELDS, lineterminator="\n")
            writer.writeheader()
            writer.writerow(row)
            self.assertEqual(review.read_reviews(stream.getvalue().encode()), [row])
            self.assertTrue(review.public_value_matches(row, payload, region))
            self.assertFalse(review.public_value_matches(row, payload + b" ", region))
            self.assertFalse(
                review.public_value_matches(row, payload, {**region, "charOffset": region["charOffset"] + 1})
            )
            model = source["Pythia14M"] if name == review.CORPUS_COMMIT_PATHS[0] else source["subjects"]["Pythia14M"]
            for key in model:
                changed = copy.deepcopy(source)
                target = (
                    changed["Pythia14M"] if name == review.CORPUS_COMMIT_PATHS[0] else changed["subjects"]["Pythia14M"]
                )
                del target[key]
                self.assertFalse(review.public_corpus_commit_matches(row, changed), key)
            wrong = payload.replace(b"EleutherAI", b"unknown-publisher")
            self.assertFalse(
                review.public_value_matches({**row, "input_sha256": hashlib.sha256(wrong).hexdigest()}, wrong, region)
            )
            self.assertFalse(review.public_corpus_commit_matches(row, []))
            self.assertFalse(review.public_corpus_commit_matches(row, {}))
            with self.assertRaises(ValueError):
                review.read_reviews(stream.getvalue().replace(name, "src/model.json").encode())


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
        self.source.write_bytes(
            subprocess.check_output(
                ["git", "-C", str(checkout), "show", "509e6077faf6defca2d90e90f9116dbcd1bcb9ea:" + self.row["path"]]
            )
        )
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

    # Purpose: Keep an exactly reviewed source finding blocking rather than crediting a review as a repair.
    # Inputs: One historically reviewed finding and an unreviewed sibling. Outputs: Both unresolved and identical SARIF.
    def test_lossless_exact_admission(self):
        report = self.root / "raw.sarif"
        report.write_text(json.dumps({"runs": [{"results": [self.finding, {"ruleId": "DS117838"}]}]}))
        before = report.read_bytes()
        result = review.review_findings(
            self.root, report, b",".join(key.encode() for key in review.FIELDS) + b"\n", self.policy
        )
        self.assertEqual((len(result["reviewed_source"]), result["unresolved_count"]), (1, 2))
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
        self.assertEqual(len(self.rows), 27)

    # Purpose: Reuse exact local source matching while additionally binding hosted admission to committed bytes.
    # Inputs: An independently assembled GitHub record. Outputs: One approved ID with no raw state mutation.
    def hosted_fixture(self):
        return {
            "number": 42,
            "state": "open",
            "tool": {"name": "devskim", "version": review.SOURCE_TOOL_VERSION},
            "rule": {"id": self.row["rule"]},
            "most_recent_instance": {
                "commit_sha": "a" * 40,
                "category": "devskim",
                "location": {
                    "path": self.row["path"],
                    **dict(
                        zip(
                            ("start_line", "start_column", "end_line", "end_column"),
                            (int(self.row[key]) for key in review.SOURCE_FIELDS[3:7]),
                            strict=True,
                        )
                    ),
                },
            },
        }

    # Purpose: Preserve hosted state and require committed source identity in addition to the local digest.
    # Inputs: Exact and nonmatching published blobs. Outputs: Admission passes only the exact published source.
    def test_hosted_committed_identity(self):
        from unittest.mock import patch

        alert = self.hosted_fixture()
        before = copy.deepcopy(alert)
        with patch.object(review, "committed_source_matches", return_value=True) as committed:
            self.assertEqual(review.match_historical_source_alerts(self.root, [alert], "a" * 40, self.policy), [42])
            committed.assert_called_once()
        self.assertEqual(alert, before)
        with patch.object(review, "committed_source_matches", return_value=False):
            self.assertEqual(review.match_historical_source_alerts(self.root, [alert], "a" * 40, self.policy), [])

    # Purpose: Reject stale analyses, changed producer/category/rule/region and malformed identities independently.
    # Inputs: Independent GitHub record mutations. Outputs: Every unmatched record remains blocking.
    def test_hosted_freshness_boundaries(self):
        from unittest.mock import patch

        mutations = [
            ("tool", "version", "1.0.101"),
            ("tool", "name", "other"),
            ("most_recent_instance", "commit_sha", "b" * 40),
            ("most_recent_instance", "category", "other"),
            ("rule", "id", "DS117838"),
        ]
        for container, key, value in mutations:
            alert = self.hosted_fixture()
            alert[container][key] = value
            self.assertEqual(review.match_historical_source_alerts(self.root, [alert], "a" * 40, self.policy), [])
        for key in ("start_line", "start_column", "end_line", "end_column"):
            alert = self.hosted_fixture()
            alert["most_recent_instance"]["location"][key] += 1
            self.assertEqual(review.match_historical_source_alerts(self.root, [alert], "a" * 40, self.policy), [])
        with patch.object(review, "committed_source_matches", return_value=True):
            alert = self.hosted_fixture()
            with self.assertRaises(ValueError):
                review.match_historical_source_alerts(self.root, [alert, alert], "a" * 40, self.policy)
        with self.assertRaises(ValueError):
            review.match_historical_source_alerts(self.root, [], "HEAD", self.policy)

    # Purpose: Preserve historical review provenance without requiring current source to retain reviewed bytes.
    # Inputs: The historical ledger and its exact recorded revision. Outputs: Matching archival identities only.
    def test_all_registered_source_identities(self):
        checkout = Path(__file__).resolve().parents[1]
        for row in self.rows:
            source = self.root / row["path"]
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(
                subprocess.check_output(
                    ["git", "-C", str(checkout), "show", "509e6077faf6defca2d90e90f9116dbcd1bcb9ea:" + row["path"]]
                )
            )
            finding = {
                "ruleId": row["rule"],
                "locations": [
                    {
                        "physicalLocation": {
                            "artifactLocation": {"uri": row["path"]},
                            "region": {key: int(row[key]) for key in review.SOURCE_FIELDS[3:7]},
                        }
                    }
                ],
            }
            self.assertIsNotNone(review.reviewed_source_finding(self.root, finding, self.rows))
        with self.assertRaises(ValueError):
            review.read_source_reviews(
                self.policy.replace(
                    b'"third_party/lzma_sdk/C/CpuArch.h","DS121708"', b'"third_party/lzma_sdk/C/CpuArch.h","DS161085"'
                )
            )


class CurrentHostedMetadataTests(unittest.TestCase):
    # Purpose: Bind hosted metadata review tests to real committed blobs rather than mocked hash decisions.
    # Inputs: Small isolated configuration and approval fixtures. Outputs: One private Git commit and alert.
    def setUp(self):
        ScannerMetadataReviewTests.setUp(self)
        ledger = self.root / review.POLICY
        ledger.parent.mkdir(parents=True)
        ledger.write_bytes(self.policy)
        subprocess.run(["git", "init", "--quiet", str(self.root)], check=True, capture_output=True)
        self.commit_fixture()
        column = self.payload.index('"' + self.value)
        self.alert = {
            "number": 42,
            "state": "dismissed",
            "tool": {"name": "devskim", "version": review.SOURCE_TOOL_VERSION},
            "rule": {"id": "DS173237"},
            "most_recent_instance": {
                "state": "dismissed",
                "commit_sha": self.commit,
                "category": "devskim",
                "location": {
                    "path": self.name,
                    "start_line": 1,
                    "end_line": 1,
                    "start_column": column,
                    "end_column": column + 66,
                },
            },
        }

    # Purpose: Commit only this fixture's public metadata and ledger with private repository-local identity.
    # Inputs: The temporary fixture tree. Outputs: Updates its full commit without affecting the live checkout.
    def commit_fixture(self):
        subprocess.run(
            ["git", "add", "--", self.name, str(review.POLICY)], cwd=self.root, check=True, capture_output=True
        )
        subprocess.run(
            [
                "git",
                "-c",
                "user.name=Fixture",
                "-c",
                "user.email=fixture@example.invalid",
                "commit",
                "--no-gpg-sign",
                "-qm",
                "fixture",
            ],
            cwd=self.root,
            check=True,
            capture_output=True,
        )
        self.commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=self.root, text=True).strip()

    # Purpose: Preserve the original hosted report and match only its committed metadata value.
    # Inputs: One current dismissed public checksum. Outputs: Its ID without mutating any alert or source.
    def test_current_exact_committed_metadata(self):
        before = copy.deepcopy(self.alert)
        self.assertEqual(review.match_current_metadata_alerts(self.root, [self.alert], self.commit, self.policy), [42])
        self.assertEqual(self.alert, before)
        self.path.write_text("uncommitted local content", encoding="utf-8")
        self.assertEqual(review.match_current_metadata_alerts(self.root, [self.alert], self.commit, self.policy), [42])

    # Purpose: Refuse stale producers, locations, categories, states and source paths independently.
    # Inputs: Plausible single-field hosted mutations. Outputs: Every altered finding stays unreviewed.
    def test_current_identity_mutations(self):
        mutations = [
            (("tool", "version"), "other"),
            (("tool", "name"), "CodeQL"),
            (("rule", "id"), "DS121708"),
            (("state",), "open"),
            (("most_recent_instance", "state"), "fixed"),
            (("most_recent_instance", "commit_sha"), "b" * 40),
            (("most_recent_instance", "category"), "other"),
            (("most_recent_instance", "location", "path"), "src/sample.cpp"),
            (("most_recent_instance", "location", "start_column"), 1),
            (("most_recent_instance", "location", "end_column"), 999),
            (("most_recent_instance", "location", "end_line"), 2),
            (("most_recent_instance", "location", "start_line"), True),
        ]
        for keys, value in mutations:
            with self.subTest(keys=keys):
                alert = copy.deepcopy(self.alert)
                target = alert
                for key in keys[:-1]:
                    target = target[key]
                target[keys[-1]] = value
                self.assertEqual(review.match_current_metadata_alerts(self.root, [alert], self.commit, self.policy), [])

    # Purpose: Prevent changed ledger/source bytes from inheriting an earlier exact disposition.
    # Inputs: A modified approval and a new commit retaining stale approval. Outputs: Rejection or no match.
    def test_current_committed_byte_boundaries(self):
        with self.assertRaisesRegex(ValueError, "ledger differs"):
            review.match_current_metadata_alerts(self.root, [self.alert], self.commit, self.policy + b"\n")
        self.path.write_bytes((self.payload + "\n").encode())
        self.commit_fixture()
        self.alert["most_recent_instance"]["commit_sha"] = self.commit
        self.assertEqual(review.match_current_metadata_alerts(self.root, [self.alert], self.commit, self.policy), [])

    # Purpose: Reject duplicate IDs and ambiguous commit identifiers instead of interpreting partial evidence.
    # Inputs: Duplicate alerts, boolean IDs and a ref spelling. Outputs: Explicit validation failures.
    def test_current_invalid_inventory(self):
        for alerts, commit in (
            ([self.alert, self.alert], self.commit),
            ([{**self.alert, "number": True}], self.commit),
            ([], "HEAD"),
        ):
            with self.assertRaises(ValueError):
                review.match_current_metadata_alerts(self.root, alerts, commit, self.policy)

    # Purpose: Keep DevSkim's pinned UTF-16 column contract correct across multiline and non-ASCII metadata.
    # Inputs: A committed CRLF JSON record with a supplementary Unicode character before the checksum.
    # Outputs: The exact public value matches; a byte-count column cannot inherit that disposition.
    def test_current_utf16_multiline_columns(self):
        prefix = '  "label": "\U0001f680", "pin": '
        payload = ("{\r\n" + prefix + '"' + self.value + '"\r\n}\r\n').encode("utf-8")
        old_digest = hashlib.sha256(self.path.read_bytes()).hexdigest()
        self.policy = self.policy.replace(
            old_digest.encode(), hashlib.sha256(payload.replace(b"\r\n", b"\n")).hexdigest().encode()
        )
        self.path.write_bytes(payload)
        (self.root / review.POLICY).write_bytes(self.policy)
        self.commit_fixture()
        self.alert["most_recent_instance"]["commit_sha"] = self.commit
        column = len(prefix.encode("utf-16-le")) // 2
        self.alert["most_recent_instance"]["location"].update(
            start_line=2, end_line=2, start_column=column, end_column=column + 66
        )
        self.assertEqual(review.match_current_metadata_alerts(self.root, [self.alert], self.commit, self.policy), [42])
        self.alert["most_recent_instance"]["location"].update(start_column=len(prefix), end_column=len(prefix) + 66)
        self.assertEqual(review.match_current_metadata_alerts(self.root, [self.alert], self.commit, self.policy), [])


if __name__ == "__main__":
    unittest.main()
