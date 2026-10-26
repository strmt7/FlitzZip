"""Exact-public-metadata review must never admit code, stale inputs or unrelated findings."""

import contextlib
import copy
import csv
import hashlib
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

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
    def test_empty_rows_invalid_digests_and_malformed_public_commit_metadata(self):
        """Purpose: Refuse incomplete reviews and malformed typed metadata without changing raw evidence.
        Inputs: Empty fields, invalid hashes and a correctly located commit string inside invalid JSON.
        Outputs: Explicit policy refusal or no exact match; no broader source admission.
        """
        row = next(csv.DictReader(io.StringIO(self.policy.decode())))
        for field, value, message in (("public_sha256", "", "malformed"), ("input_sha256", "g" * 64, "invalid digest")):
            output = io.StringIO()
            writer = csv.DictWriter(output, fieldnames=review.FIELDS)
            writer.writeheader()
            writer.writerow({**row, field: value})
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, message):
                review.read_reviews(output.getvalue().encode())
        value = "a" * 40
        payload = ('{"commit":"' + value + '"]').encode()
        typed = {"public_value": value, "value_kind": "git_commit", "input_sha256": hashlib.sha256(payload).hexdigest()}
        self.assertFalse(review.public_value_matches(typed, payload, {"charOffset": 10, "charLength": 42}))

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
            ".github/requirements/semgrep-distribution.json",
            "third_party/upstream/nltk/3.10.4-rc1/distribution.json",
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

    def test_coverage_integrity_matches_exact_reviewed_bytes_only(self):
        """Purpose: Bound public collector hashes. Inputs: Real lock and mutations. Outputs: Exact admission only."""
        root = Path(__file__).resolve().parents[1]
        name = "tools/native-coverage-toolchain.json"
        payload = (root / name).read_bytes()
        manifest = json.loads(payload)
        values = {manifest["installer_sha256"], *manifest["files"].values()}
        rows = [row for row in review.read_reviews(review.read_policy(root)) if row["path"] == name]
        self.assertEqual({row["public_value"] for row in rows}, values)
        for row in rows:
            quoted = '"' + row["public_value"] + '"'
            offset = payload.decode("utf-8").index(quoted)
            region = {"charOffset": offset, "charLength": len(quoted)}
            self.assertTrue(review.public_value_matches(row, payload, region))
            self.assertFalse(review.public_value_matches(row, payload + b" ", region))
            self.assertFalse(review.public_value_matches(row, payload, {**region, "charOffset": offset + 1}))

    def test_hip_reader_integrity_requires_exact_official_dependency_role(self):
        """Purpose: Bind native reader pins. Inputs: Real manifest and role mutations. Outputs: Exact matches only."""
        root = Path(__file__).resolve().parents[1]
        name = "tools/hip-coverage-toolchain.json"
        payload = (root / name).read_bytes()
        manifest = json.loads(payload)
        values = {manifest["revision"], manifest["zlib"]["sha256"]}
        rows = [row for row in review.read_reviews(review.read_policy(root)) if row["path"] == name]
        self.assertEqual({row["public_value"] for row in rows}, values)
        for row in rows:
            quoted = '"' + row["public_value"] + '"'
            region = {"charOffset": payload.decode().index(quoted), "charLength": len(quoted)}
            self.assertTrue(review.public_value_matches(row, payload, region))
            self.assertFalse(review.public_value_matches(row, payload + b" ", region))
            self.assertFalse(review.public_value_matches(row, payload, {**region, "charOffset": 0}))
            wrong = payload.replace(b"https://github.com/madler/zlib", b"https://example.invalid/zlib")
            modified = {**row, "input_sha256": hashlib.sha256(wrong).hexdigest()}
            changed_region = {"charOffset": wrong.decode().index(quoted), "charLength": len(quoted)}
            self.assertFalse(review.public_value_matches(modified, wrong, changed_region))

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

    def test_legacy_review_reader_refuses_duplicate_unbounded_or_empty_regions(self):
        """Purpose: Prevent broader historical ledgers from passing structural admission.
        Inputs: Independent component-count, coordinate and duplicate-identity mutations. Outputs: Explicit refusal.
        """
        invalid = [([self.row, self.row], "identity is duplicated")]
        for field, value in (("startLine", "0"), ("endColumn", "10000000"), ("endLine", self.row["startLine"])):
            changed = dict(self.row)
            changed[field] = value
            if field == "endLine":
                changed["endColumn"] = changed["startColumn"]
            invalid.append(([changed], "positive extent" if field == "endLine" else "positive region coordinates"))
        rows = []
        for index in range(22):
            rows.append(
                {
                    **self.row,
                    "path": "cmake/ZstdCoverSelection.c",
                    "evidence": review.SOURCE_EVIDENCE,
                    "startLine": str(index + 1),
                    "startColumn": "1",
                    "endLine": str(index + 1),
                    "endColumn": "2",
                }
            )
        invalid.append((rows, "component counts"))
        for rows, message in invalid:
            output = io.StringIO()
            writer = csv.DictWriter(output, fieldnames=review.SOURCE_FIELDS)
            writer.writeheader()
            writer.writerows(rows)
            with self.subTest(rows=rows), self.assertRaisesRegex(ValueError, message):
                review.read_source_reviews(output.getvalue().encode())

    def test_source_read_byte_budget_stops_before_digest_admission(self):
        """Purpose: Keep oversized live or published sources out of exact-review matching.
        Inputs: An oversized private source and controlled Git size output. Outputs: No digest admission or blob read.
        """
        original = self.source.read_bytes()
        self.source.write_bytes(b"x" * (review.MAX_POLICY_BYTES + 1))
        with self.assertRaisesRegex(ValueError, "Reviewed source exceeds"):
            review.reviewed_source_finding(self.root, self.finding, [self.row])
        self.source.write_bytes(original)
        self.assertEqual(review.reviewed_source_finding(self.root, self.finding, [self.row]), self.row)
        with patch.object(
            review.subprocess, "run", return_value=SimpleNamespace(stdout=str(review.MAX_POLICY_BYTES + 1).encode())
        ) as git:
            with self.assertRaisesRegex(ValueError, "Published reviewed source exceeds"):
                review.committed_source_matches(self.root, "0" * 40, self.row)
            self.assertEqual(git.call_count, 1)

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

    def test_committed_source_size_is_bounded_and_consistent_before_review(self):
        """Purpose: Refuse oversized or inconsistent Git responses before matching public metadata.
        Inputs: Real fixture policy/commit plus controlled source size/body responses. Outputs: No accepted finding.
        """
        real_run = subprocess.run
        for advertised in (b"not-a-size", str(review.MAX_POLICY_BYTES + 1).encode(), b"1"):
            # Purpose: Inject one Git transport inconsistency after the actual policy snapshot is read.
            # Inputs: Fixed production Git command and options. Outputs: Original policy or controlled source response.
            def git(arguments, *, selected=advertised, check, **kwargs):
                self.assertIs(check, True)
                if arguments[:3] == ["git", "cat-file", "-s"] and arguments[-1] == f"{self.commit}:{self.name}":
                    return SimpleNamespace(stdout=selected)
                return real_run(arguments, check=check, **kwargs)

            with (
                self.subTest(advertised=advertised),
                patch.object(review.subprocess, "run", side_effect=git),
                self.assertRaisesRegex(ValueError, "byte budget|inconsistent size"),
            ):
                review.match_current_metadata_alerts(self.root, [self.alert], self.commit, self.policy)

    # Purpose: Preserve the original hosted report and match only its committed metadata value.
    # Inputs: The same current public checksum in each active hosted state. Outputs: Its ID without mutation.
    def test_current_exact_committed_metadata(self):
        for state in ("open", "dismissed"):
            alert = copy.deepcopy(self.alert)
            alert["state"] = alert["most_recent_instance"]["state"] = state
            before = copy.deepcopy(alert)
            self.assertEqual(review.match_current_metadata_alerts(self.root, [alert], self.commit, self.policy), [42])
            self.assertEqual(alert, before)
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
            (("state",), "fixed"),
            (("most_recent_instance", "state"), "open"),
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


class ScannerReviewCommandTests(unittest.TestCase):
    # Purpose: Own a committed CLI fixture independently of the developer's current policy edits.
    # Inputs: None. Outputs: Cleanup-bound snapshots and exact copies of production modules and review policies.
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.snapshot = self.root / "alerts.json"
        self.snapshot.write_text("[]")
        source = Path(review.__file__).resolve().parents[1]
        self.checkout = self.root / "scanner-cli-source-copy"
        for name in (
            "tools/scanner_metadata_review.py",
            "tools/scanner_hosted_review.py",
            "tools/native_build_provenance.py",
            ".github/scanner-metadata-reviews.csv",
            ".github/scanner-source-reviews.csv",
            ".github/scanner-hosted-reviews.csv",
            ".github/scanner-hosted-approval.csv",
            ".github/scanner-false-positive-reviews.csv",
        ):
            destination = self.checkout / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / name, destination)
            self.assertEqual(destination.read_bytes(), (source / name).read_bytes())
        options = [
            "git",
            "-c",
            "user.name=Coverage Test",
            "-c",
            "user.email=test@example.invalid",
            "-c",
            "commit.gpgsign=false",
            "-c",
            f"core.hooksPath={self.root / 'no-hooks'}",
        ]
        for arguments in (
            ("init", "--quiet", "-b", "main"),
            ("add", "--all"),
            ("commit", "--quiet", "-m", "owned CLI unit fixture"),
        ):
            subprocess.run([*options, *arguments], cwd=self.checkout, check=True, capture_output=True, timeout=15)

    # Purpose: Prove empty findings do not authorize a modified working review ledger.
    # Inputs: An exact private committed fixture, then an uncommitted byte change to its current-decision ledger.
    # Outputs: The actual CLI rejects the mismatch and preserves the raw snapshot instead of returning admission IDs.
    def test_real_cli_rejects_dirty_current_ledger(self):
        ledger = self.checkout / ".github/scanner-false-positive-reviews.csv"
        ledger.write_bytes(ledger.read_bytes() + b"\n")
        process = subprocess.run(
            [
                sys.executable,
                "-B",
                "-m",
                "tools.scanner_metadata_review",
                "--hosted-alerts",
                str(self.snapshot),
                "--current-reviews",
                "--repository",
                "strmt7/FlitzZip",
            ],
            cwd=self.checkout,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        self.assertEqual(process.returncode, 1)
        self.assertIn("ledger differs from the audited commit", process.stderr)
        self.assertEqual(self.snapshot.read_bytes(), b"[]")

    # Purpose: Exercise the actual process entry point with current policy bytes and no scanner or remote API.
    # Inputs: Empty complete snapshot and explicit read modes. Outputs: Exact empty classification and no raw changes.
    def test_real_cli_empty_snapshot_modes_preserve_input(self):
        cases = (
            ([], {"historically_reviewed": [], "blocking": [], "unresolved_count": 0}),
            (["--current-metadata"], {"reviewed_metadata": []}),
            (["--current-reviews", "--repository", "strmt7/FlitzZip"], {"reviewed_findings": []}),
        )
        for options, expected in cases:
            with self.subTest(options=options):
                process = subprocess.run(
                    [
                        sys.executable,
                        "-B",
                        "-m",
                        "tools.scanner_metadata_review",
                        "--hosted-alerts",
                        str(self.snapshot),
                        *options,
                    ],
                    cwd=self.checkout,
                    capture_output=True,
                    text=True,
                    timeout=30,
                    check=False,
                )
                self.assertEqual(process.returncode, 0, process.stderr)
                self.assertEqual(json.loads(process.stdout), expected)
                self.assertEqual(self.snapshot.read_bytes(), b"[]")

    # Purpose: Fail before policy matching on malformed or oversized hosted input.
    # Inputs: Invalid complete-array shapes and one bounded over-budget file. Outputs: No success JSON or review action.
    def test_cli_snapshot_shape_and_size_fail_closed(self):
        for payload, message in (
            (b"{}", "complete alert array"),
            (b"[null]", "complete alert array"),
            (b" " * (16 * 1024**2 + 1), "snapshot exceeds its byte budget"),
        ):
            self.snapshot.write_bytes(payload)
            with (
                self.subTest(message=message),
                patch.object(sys, "argv", ["review", "--hosted-alerts", str(self.snapshot)]),
                patch.object(review, "match_current_metadata_alerts") as match,
                self.assertRaisesRegex(ValueError, message),
            ):
                review.main()
            match.assert_not_called()
            self.assertEqual(self.snapshot.read_bytes(), payload)

    # Purpose: Retain historical source findings as blocking and reject overlapping review identities.
    # Inputs: Controlled classifier results at the CLI routing boundary. Outputs: Exact mode output or explicit refusal.
    def test_review_mode_routing_never_treats_historical_source_review_as_repair(self):
        self.snapshot.write_text(json.dumps([{"number": 9, "state": "open"}, {"number": 10, "state": "open"}]))
        cases = (
            ([], [9], [4], {"historically_reviewed": [4, 9], "blocking": [9, 10], "unresolved_count": 2}),
            (["--current-metadata"], [9], [], {"reviewed_metadata": [9]}),
            (["--current-reviews", "--repository", "strmt7/FlitzZip"], [9], [4], {"reviewed_findings": [4, 9]}),
            ([], [9], [9], None),
            (["--current-reviews", "--repository", "strmt7/FlitzZip"], [9], [9], None),
        )
        for options, first, second, expected in cases:
            output = io.StringIO()
            with (
                self.subTest(options=options, second=second),
                patch.object(
                    sys,
                    "argv",
                    ["review", "--hosted-alerts", str(self.snapshot), "--commit", "0" * 40, *options],
                ),
                patch.object(review, "read_policy", return_value=b"fixture"),
                patch.object(review, "read_source_policy", return_value=b"fixture"),
                patch.object(review, "read_hosted_policy", return_value=b"fixture"),
                patch.object(review, "match_current_metadata_alerts", return_value=first),
                patch.object(review, "match_current_false_positives", return_value=second),
                patch.object(review, "match_historical_source_alerts", return_value=first),
                patch.object(review, "match_historical_hosted_alerts", return_value=second),
                contextlib.redirect_stdout(output),
            ):
                if expected is None:
                    with self.assertRaisesRegex(ValueError, "overlap"):
                        review.main()
                    self.assertEqual(output.getvalue(), "")
                else:
                    review.main()
                    self.assertEqual(json.loads(output.getvalue()), expected)

    # Purpose: Require the exact audited repository before combining current false-positive reviews.
    # Inputs: Missing, URL-shaped and traversal-like repository declarations. Outputs: No classifier invocation.
    def test_current_review_requires_audited_repository(self):
        for repository in ("", "https://github.com/strmt7/FlitzZip", "owner/../repo"):
            with (
                self.subTest(repository=repository),
                patch.object(
                    sys,
                    "argv",
                    [
                        "review",
                        "--hosted-alerts",
                        str(self.snapshot),
                        "--commit",
                        "0" * 40,
                        "--current-reviews",
                        "--repository",
                        repository,
                    ],
                ),
                patch.object(review, "match_current_false_positives") as match,
                self.assertRaisesRegex(ValueError, "audited repository"),
            ):
                review.main()
            match.assert_not_called()


class ReviewEntryPointTests(unittest.TestCase):
    def test_default_commit_uses_owned_checkout_and_keeps_empty_snapshot_unchanged(self):
        """Purpose: Qualify actual module execution and default source identity without accepting any finding.
        Inputs: Owned empty alert array and actual checkout HEAD. Outputs: Empty blocking result and unchanged input.
        """
        root = Path(review.__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            snapshot = Path(directory) / "alerts.json"
            snapshot.write_bytes(b"[]\n")
            result = subprocess.run(
                [sys.executable, "-B", "-m", "tools.scanner_metadata_review", "--hosted-alerts", str(snapshot)],
                cwd=root,
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                json.loads(result.stdout), {"historically_reviewed": [], "blocking": [], "unresolved_count": 0}
            )
            self.assertEqual(snapshot.read_bytes(), b"[]\n")


if __name__ == "__main__":
    unittest.main()
