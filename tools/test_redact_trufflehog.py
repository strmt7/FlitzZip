"""Offline regressions for the secret-report publication boundary."""

import ast
import copy
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

from tools.redact_trufflehog import (
    MAX_RECORD_BYTES,
    PublicFixtureReview,
    redact_finding,
    redact_stream,
    review_bytes,
    reviewed_member,
)

FIXTURE_PRIVATE = "synthetic-private-fixture-do-not-publish"


# Purpose: Locate native Bash without WSL. Inputs: Installed Git/tool paths. Outputs: Executable or explicit failure.
def native_bash() -> str:
    bash = shutil.which("bash") if os.name != "nt" else None
    if os.name == "nt" and (git := shutil.which("git")):
        binary = Path(git).resolve()
        candidates = [binary.with_name("bash.exe"), *(parent / "bin/bash.exe" for parent in binary.parents)]
        bash = next((str(path) for path in candidates if path.is_file()), None)
    if bash is None:
        raise AssertionError("Native Bash or Git for Windows Bash is required; WSL must not be used")
    return bash


# Purpose: Build a realistic finding without a live credential or personal identity.
# Inputs: None. Outputs: Fresh mutable TruffleHog Git JSON fixture.
def finding() -> dict:
    return {
        "SourceMetadata": {
            "Data": {
                "Git": {
                    "commit": "a" * 40,
                    "line": 7,
                    "file": "fixture/" + FIXTURE_PRIVATE,
                    "email": FIXTURE_PRIVATE,
                    "repository": FIXTURE_PRIVATE,
                }
            }
        },
        "DetectorType": 2,
        "DetectorName": "AWS",
        "Verified": False,
        "Raw": FIXTURE_PRIVATE,
        "RawV2": FIXTURE_PRIVATE,
        "Redacted": FIXTURE_PRIVATE,
        "VerificationError": FIXTURE_PRIVATE,
        "ExtraData": {"identity": FIXTURE_PRIVATE},
        "StructuredData": {"key": FIXTURE_PRIVATE},
        "FutureField": FIXTURE_PRIVATE,
    }


class ReportRedactionTests(unittest.TestCase):
    # Purpose: Prove secrets, auxiliary fields, paths, and identities cannot pass the allowlist.
    # Inputs: A realistic private-marker fixture. Outputs: Assertions on the exact public schema and location.
    def test_only_allowlisted_metadata_is_published(self):
        record = finding()
        before = copy.deepcopy(record)
        safe = redact_finding(record)
        self.assertEqual(record, before)
        self.assertNotIn(FIXTURE_PRIVATE, json.dumps(safe))
        self.assertEqual(
            safe,
            {
                "schema_version": 1,
                "detector_id": 2,
                "verified": False,
                "verification_error": True,
                "location": {
                    "commit": "a" * 40,
                    "line": 7,
                    "path_sha256": hashlib.sha256(("fixture/" + FIXTURE_PRIVATE).encode()).hexdigest(),
                },
            },
        )

    # Purpose: Preserve all valid records rather than stopping at the first finding.
    # Inputs: JSONL with varied verification states and blank lines. Outputs: Equal record count and distinct locations.
    def test_stream_preserves_each_finding(self):
        records = [finding() for _ in range(3)]
        for index, record in enumerate(records):
            record["Verified"] = bool(index % 2)
            record["SourceMetadata"]["Data"]["Git"]["line"] = index
        output = io.StringIO()
        count = redact_stream(io.BytesIO(("\n" + "\n\n".join(map(json.dumps, records))).encode()), output)
        self.assertEqual(count, 3)
        results = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual([item["location"]["line"] for item in results], [0, 1, 2])
        self.assertEqual([item["verified"] for item in results], [False, True, False])

    # Purpose: Treat a successful empty scanner stream as zero findings, without fabricating records.
    # Inputs: Empty or whitespace-only streams. Outputs: Zero counts and empty artifacts.
    def test_empty_stream(self):
        for data in (b"", b"\n \r\n"):
            output = io.StringIO()
            self.assertEqual(redact_stream(io.BytesIO(data), output), 0)
            self.assertEqual(output.getvalue(), "")

    # Purpose: Reject invalid location and detector metadata without string coercion.
    # Inputs: Malformed scalar, compound, and out-of-range fixtures. Outputs: Value-free failures.
    def test_metadata_is_strict(self):
        for value in (None, True, -1, 1 << 63, "2", {}, []):
            for key in ("DetectorType", "line"):
                record = finding()
                container = record if key == "DetectorType" else record["SourceMetadata"]["Data"]["Git"]
                container[key] = value
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    redact_finding(record)
        for value in (None, 1, "true", []):
            record = finding()
            record["Verified"] = value
            with self.assertRaises(ValueError):
                redact_finding(record)
        for key, value in (("commit", FIXTURE_PRIVATE), ("file", ""), ("file", "x" * 32768)):
            record = finding()
            record["SourceMetadata"]["Data"]["Git"][key] = value
            with self.assertRaises(ValueError):
                redact_finding(record)

    # Purpose: Enforce the per-record memory ceiling without discarding the valid boundary case.
    # Inputs: A valid record padded to the limit and oversized input. Outputs: Accepted exact limit; rejected excess.
    def test_record_limit(self):
        record = json.dumps(finding()).encode()
        padded = record + b" " * (MAX_RECORD_BYTES - len(record) - 1) + b"\n"
        self.assertEqual(redact_stream(io.BytesIO(padded), io.StringIO()), 1)
        with self.assertRaises(ValueError):
            redact_stream(io.BytesIO(b"x" * (MAX_RECORD_BYTES + 1)), io.StringIO())

    # Purpose: Verify the real CLI never prints malformed source or parser exceptions.
    # Inputs: Invalid UTF-8, JSON, nested data, and schema. Outputs: Nonzero exit and no private marker or traceback.
    def test_cli_fails_closed(self):
        script = Path(__file__).with_name("redact_trufflehog.py")
        for data in (FIXTURE_PRIVATE.encode(), b"\xff", b"null", b"{}", b"[" * 2000):
            result = subprocess.run(
                [sys.executable, str(script)], input=data, capture_output=True, timeout=10, check=False
            )
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, b"")
            self.assertNotIn(FIXTURE_PRIVATE.encode(), result.stderr)
            self.assertNotIn(b"Traceback", result.stderr)

    # Purpose: Verify the actual CLI preserves safe findings and reports a count on stderr.
    # Inputs: One valid synthetic finding. Outputs: Valid sanitized JSON and zero formatter exit.
    def test_cli_success(self):
        script = Path(__file__).with_name("redact_trufflehog.py")
        result = subprocess.run(
            [sys.executable, str(script)],
            input=json.dumps(finding()).encode(),
            capture_output=True,
            timeout=10,
            check=False,
        )
        self.assertEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout), redact_finding(finding()))
        self.assertNotIn(FIXTURE_PRIVATE.encode(), result.stdout + result.stderr)
        self.assertIn(b"1 finding(s)", result.stderr)

    # Purpose: Prove the actual CI pipeline reports both statuses and fails closed even with inherited errexit.
    # Inputs: Mock Docker stdout/status, real Bash and the real redactor; no network or Docker daemon is used.
    # Outputs: Preserved safe findings and diagnostics; unresolved findings or scanner errors cannot pass.
    def test_ci_pipeline_preserves_failures(self):
        bash = native_bash()
        script = Path(__file__).with_name("scan_trufflehog.sh").read_text(encoding="utf-8")
        prelude = 'docker() { printf "%s" "$SCANNER_FIXTURE"; return "$SCANNER_STATUS"; }\n'
        prelude += 'python() { "$PYTHON_FOR_TEST" "$REDACTOR_PATH" "${@:2}"; }\n'
        cases = [
            (0, "", 0, 0),
            (0, json.dumps(finding()), 3, 1),
            (183, json.dumps(finding()), 3, 1),
            (183, "", 0, 0),
            (182, "", 0, 0),
            (125, "", 0, 0),
            (0, FIXTURE_PRIVATE, 2, 0),
            (183, FIXTURE_PRIVATE, 2, 0),
        ]
        for scanner_status, fixture, redactor_status, count in cases:
            with (
                self.subTest(scanner_status=scanner_status, redactor_status=redactor_status),
                tempfile.TemporaryDirectory(prefix="superzip-scanner-test-") as directory,
            ):
                report = Path(directory) / "reports/secrets/trufflehog-git.jsonl"
                report.parent.mkdir(parents=True)
                ledger = Path(directory) / ".github/scanner-secret-reviews.json"
                ledger.parent.mkdir()
                ledger.write_text(json.dumps({"schema": 1, "reviews": []}), encoding="utf-8")
                environment = dict(
                    os.environ,
                    SCANNER_FIXTURE=fixture,
                    SCANNER_STATUS=str(scanner_status),
                    PYTHON_FOR_TEST=Path(sys.executable).as_posix(),
                    REDACTOR_PATH=Path(__file__).with_name("redact_trufflehog.py").resolve().as_posix(),
                )
                result = subprocess.run(
                    [bash, "--noprofile", "--norc", "-e", "-o", "pipefail", "-c", prelude + script],
                    cwd=directory,
                    env=environment,
                    capture_output=True,
                    timeout=20,
                    check=False,
                )
                self.assertEqual(result.returncode, int(scanner_status != 0 or redactor_status != 0 or count != 0))
                self.assertIn(
                    f"TruffleHog exit code: {scanner_status}; report redaction exit code: {redactor_status}".encode(),
                    result.stdout,
                )
                self.assertNotIn(FIXTURE_PRIVATE.encode(), result.stdout + result.stderr)
                records = [json.loads(line) for line in report.read_text(encoding="utf-8").splitlines()]
                self.assertEqual(len(records), count)
                self.assertNotIn(FIXTURE_PRIVATE, json.dumps(records))


class PublicFixtureReviewTests(unittest.TestCase):
    """Use the actual upstream archive and actual Git blobs, without publishing its match."""

    # Purpose: Prepare immutable Git evidence once. Inputs: Canonical review/archive. Outputs: Owned local Git fixture.
    @classmethod
    def setUpClass(cls):
        source_root = Path(__file__).resolve().parents[1]
        cls.ledger = json.loads((source_root / ".github/scanner-secret-reviews.json").read_text(encoding="utf-8"))
        cohort = json.loads((source_root / ".github/scanner-secret-reviews-crawl4ai.json").read_text(encoding="utf-8"))
        cls.ledger["reviews"].extend(cohort["reviews"])
        cls.review = cls.ledger["reviews"][0]
        cls.directory = tempfile.TemporaryDirectory(prefix="superzip-public-fixture-")
        cls.root = Path(cls.directory.name)
        cls.archive = cls.root / cls.review["archive_path"]
        cls.archive_bytes = (source_root / cls.review["archive_path"]).read_bytes()
        for review in cls.ledger["reviews"]:
            archive_path = cls.root / review["archive_path"]
            archive_path.parent.mkdir(parents=True, exist_ok=True)
            archive_path.write_bytes((source_root / review["archive_path"]).read_bytes())
            evidence = cls.root / review["evidence"]
            evidence.parent.mkdir(parents=True, exist_ok=True)
            evidence.write_text("Isolated public fixture review evidence", encoding="utf-8")
        (cls.root / ".github").mkdir()
        with zipfile.ZipFile(cls.archive) as archive:
            line = archive.read(cls.review["member"]).decode().splitlines()[cls.review["line"] - 1]
        url, _ = ast.literal_eval(line.strip().rstrip(","))
        cls.value = url.rsplit("@", 1)[0]
        if hashlib.sha256(cls.value.encode()).hexdigest() != cls.review["raw_v2_sha256"]:
            raise AssertionError("Canonical public fixture match identity changed")
        environment = dict(
            os.environ,
            GIT_AUTHOR_NAME="Contract Fixture",
            GIT_COMMITTER_NAME="Contract Fixture",
            GIT_AUTHOR_EMAIL="fixture@example.invalid",
            GIT_COMMITTER_EMAIL="fixture@example.invalid",
        )
        for arguments in (
            ["init", "--quiet"],
            ["add", "."],
            ["-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "Public fixture"],
        ):
            subprocess.run(
                ["git", *arguments], cwd=cls.root, env=environment, capture_output=True, check=True, timeout=15
            )
        cls.commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=cls.root, timeout=10).decode().strip()

    # Purpose: Remove only the owned temporary Git fixture. Inputs: Class-owned directory. Outputs: Closed fixture.
    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    # Purpose: Restore the exact review for independent controls. Inputs: Canonical ledger. Outputs: Clean owned
    # fixture.
    def setUp(self):
        self.archive.write_bytes(self.archive_bytes)
        self.write_ledger(self.ledger)

    # Purpose: Publish a fixture-local review. Inputs: Test ledger. Outputs: Exact JSON in the owned fixture.
    def write_ledger(self, ledger):
        (self.root / ".github/scanner-secret-reviews.json").write_text(json.dumps(ledger), encoding="utf-8")

    # Purpose: Construct the exact upstream scanner match privately. Inputs: Canonical review. Outputs: Fresh
    # scanner record.
    def record(self):
        return {
            "SourceMetadata": {
                "Data": {
                    "Git": {"commit": self.commit, "line": self.review["line"], "file": self.review["archive_path"]}
                }
            },
            "DetectorType": self.review["detector"],
            "RawV2": self.value,
            "Verified": False,
            "VerificationError": "fixture",
        }

    # Purpose: Preserve all records and separately count exact dispositions. Inputs: Reviewed plus unknown
    # finding. Outputs: Honest counts/raw report.
    def test_exact_review_and_mixed_findings(self):
        reviewer = PublicFixtureReview(self.root, self.review["scanner_image"])
        output = io.StringIO()
        records = [self.record(), finding()]
        self.assertEqual(redact_stream(io.BytesIO("\n".join(map(json.dumps, records)).encode()), output, reviewer), 2)
        self.assertEqual((reviewer.total, reviewer.reviewed), (2, 1))
        self.assertEqual(
            [json.loads(line) for line in output.getvalue().splitlines()], list(map(redact_finding, records))
        )
        reviewer.publish(self.root / "review.json")
        self.assertEqual(json.loads((self.root / "review.json").read_text())["unresolved"], 1)

    # Purpose: Reject nearby or newly verified matches. Inputs: Changed detector/location/value/status/scanner.
    # Outputs: No admission.
    def test_match_boundaries(self):
        for field, value in (
            ("DetectorType", 18),
            ("RawV2", self.value + "/changed"),
            ("Verified", True),
            ("VerificationError", ""),
        ):
            record = self.record()
            record[field] = value
            self.assertFalse(PublicFixtureReview(self.root, self.review["scanner_image"]).consider(record))
        for field, value in (("line", 301), ("file", "other/source.zip")):
            record = self.record()
            record["SourceMetadata"]["Data"]["Git"][field] = value
            self.assertFalse(PublicFixtureReview(self.root, self.review["scanner_image"]).consider(record))
        self.assertFalse(
            PublicFixtureReview(self.root, self.review["scanner_image"] + "-changed").consider(self.record())
        )

    # Purpose: Expire source changes even after an earlier match. Inputs: Altered current archive or member
    # commitment. Outputs: Rejection.
    def test_source_identity_and_cache_freshness(self):
        reviewer = PublicFixtureReview(self.root, self.review["scanner_image"])
        self.assertTrue(reviewer.consider(self.record()))
        self.archive.write_bytes(self.archive_bytes + b"changed")
        self.assertFalse(reviewer.consider(self.record()))
        self.archive.write_bytes(self.archive_bytes)
        for field in ("archive_sha256", "member_sha256"):
            ledger = copy.deepcopy(self.ledger)
            ledger["reviews"][0][field] = "0" * 64
            self.write_ledger(ledger)
            self.assertFalse(PublicFixtureReview(self.root, self.review["scanner_image"]).consider(self.record()))

    # Purpose: Require a real analyzed Git blob. Inputs: Missing immutable commit. Outputs: Explicit failure
    # rather than current-tree admission.
    def test_historical_git_blob_required(self):
        record = self.record()
        record["SourceMetadata"]["Data"]["Git"]["commit"] = "a" * 40
        with self.assertRaises(subprocess.CalledProcessError):
            PublicFixtureReview(self.root, self.review["scanner_image"]).consider(record)

    # Purpose: Reject broad or malformed reviews. Inputs: Wildcards, bad hashes, extra fields and duplicate ids.
    # Outputs: Schema failure.
    def test_ledger_rejections(self):
        for field, value in (
            ("archive_path", "third_party/**"),
            ("member", "../source"),
            ("line", True),
            ("raw_v2_sha256", "short"),
            ("evidence", "docs/missing.md"),
            ("unexpected", "extra"),
        ):
            ledger = copy.deepcopy(self.ledger)
            ledger["reviews"][0][field] = value
            self.write_ledger(ledger)
            with self.assertRaises(ValueError):
                PublicFixtureReview(self.root, self.review["scanner_image"])
        ledger = copy.deepcopy(self.ledger)
        ledger["reviews"].append(copy.deepcopy(ledger["reviews"][0]))
        self.write_ledger(ledger)
        with self.assertRaises(ValueError):
            PublicFixtureReview(self.root, self.review["scanner_image"])
        for ledger in ([], {"schema": True, "reviews": []}, {"schema": 1, "reviews": [], "extra": 1}):
            self.write_ledger(ledger)
            with self.assertRaises(ValueError):
                PublicFixtureReview(self.root, self.review["scanner_image"])

    # Purpose: Verify review files are bounded before parsing. Inputs: Exact and excessive byte extents.
    # Outputs: Exact-limit bytes remain readable; excess bytes fail without publishing input.
    def test_review_input_byte_limit(self):
        path = self.root / "bounded-review-input"
        path.write_bytes(b"x" * 16)
        self.assertEqual(review_bytes(self.root, path.name, 16), b"x" * 16)
        with self.assertRaises(ValueError):
            review_bytes(self.root, path.name, 15)

    # Purpose: Exercise the real formatter and Bash gate together. Inputs: Private reviewed/unknown records and
    # scanner exits. Outputs: Exact admission only.
    def test_real_pipeline_review_and_error_states(self):
        script = Path(__file__).with_name("scan_trufflehog.sh").read_text(encoding="utf-8")
        prelude = 'docker() { printf "%s" "$SCANNER_FIXTURE"; return "$SCANNER_STATUS"; }\n'
        prelude += 'python() { "$PYTHON_FOR_TEST" "$REDACTOR_PATH" "${@:2}"; }\n'
        (self.root / "reports/secrets").mkdir(parents=True, exist_ok=True)
        for status, records, expected in (
            (183, [self.record()], 0),
            (0, [self.record()], 1),
            (182, [self.record()], 1),
            (183, [self.record(), finding()], 1),
        ):
            environment = dict(
                os.environ,
                SCANNER_FIXTURE="\n".join(map(json.dumps, records)),
                SCANNER_STATUS=str(status),
                PYTHON_FOR_TEST=Path(sys.executable).as_posix(),
                REDACTOR_PATH=Path(__file__).with_name("redact_trufflehog.py").resolve().as_posix(),
            )
            result = subprocess.run(
                [native_bash(), "--noprofile", "--norc", "-e", "-o", "pipefail", "-c", prelude + script],
                cwd=self.root,
                env=environment,
                capture_output=True,
                timeout=20,
            )
            self.assertEqual(result.returncode, expected, "Incorrect public fixture pipeline verdict")
            self.assertNotIn(self.value.encode(), result.stdout + result.stderr)
            raw = (self.root / "reports/secrets/trufflehog-git.jsonl").read_text()
            self.assertEqual(len(raw.splitlines()), len(records))
            self.assertNotIn(self.value, raw)

    # Purpose: Reproduce every approved public example from its original complete member, including attribution shifts.
    # Inputs: Canonical TAR archive and exact reported/source/hash commitments; no values reach test output.
    # Outputs: Only the four exact matches pass; adjacent locations, source lines and verified findings remain blocking.
    def test_public_archive_examples_and_location_boundaries(self):
        reviews = [row for row in self.ledger["reviews"] if row.get("archive_format") == "tar.gz"]
        self.assertEqual(len(reviews), 4)
        reviewer = PublicFixtureReview(self.root, self.review["scanner_image"])
        for review in reviews:
            with tarfile.open(self.root / review["archive_path"], "r:gz") as archive:
                source = archive.extractfile(review["member"]).read().decode("utf-8")
            line = source.splitlines()[review["source_line"] - 1]
            candidates = [
                line[start.start() : end]
                for start in re.finditer(r"https?://", line)
                for end in range(start.end(), len(line) + 1)
            ]
            values = [
                value for value in candidates if hashlib.sha256(value.encode()).hexdigest() == review["raw_v2_sha256"]
            ]
            self.assertEqual(len(values), 1, "Exact public example source match changed")
            record = self.record()
            record["RawV2"] = values[0]
            location = record["SourceMetadata"]["Data"]["Git"]
            location.update(file=review["archive_path"], line=review["line"])
            self.assertTrue(reviewer.consider(record), "Exact public example was not admitted")
            location["line"] += 1
            self.assertFalse(reviewer.consider(record), "Adjacent reported line was admitted")
            location["line"] = review["line"]
            record["Verified"] = True
            self.assertFalse(reviewer.consider(record), "Verified finding was admitted")
            changed = copy.deepcopy(self.ledger)
            changed["reviews"][changed["reviews"].index(review)]["source_line"] += 1
            self.write_ledger(changed)
            record["Verified"] = False
            self.assertFalse(PublicFixtureReview(self.root, self.review["scanner_image"]).consider(record))
            self.write_ledger(self.ledger)

    # Purpose: Keep independent approval cohorts and their complete hashes visible without resealing earlier data.
    # Inputs: Existing NLTK cohort, approved examples and a malformed second cohort in an owned fixture.
    # Outputs: Both cohorts load and publish exact hashes; malformed additional policy fails closed.
    def test_independent_review_cohort_binding(self):
        self.write_ledger({"schema": 1, "reviews": self.ledger["reviews"][:1]})
        path = self.root / ".github/scanner-secret-reviews-crawl4ai.json"
        primary = (self.root / ".github/scanner-secret-reviews.json").read_bytes()
        try:
            path.write_text(json.dumps({"schema": 1, "reviews": self.ledger["reviews"][1:]}), encoding="utf-8")
            reviewer = PublicFixtureReview(self.root, self.review["scanner_image"])
            self.assertEqual(len(reviewer.reviews), len(self.ledger["reviews"]))
            self.assertEqual(reviewer.ledger_sha256, hashlib.sha256(primary).hexdigest())
            reviewer.publish(self.root / "cohort-report.json")
            report = json.loads((self.root / "cohort-report.json").read_text())
            self.assertEqual(len(report["ledger_sha256_by_path"]), 2)
            self.assertEqual((self.root / ".github/scanner-secret-reviews.json").read_bytes(), primary)
            path.write_text('{"schema": true, "reviews": []}', encoding="utf-8")
            with self.assertRaises(ValueError):
                PublicFixtureReview(self.root, self.review["scanner_image"])
        finally:
            path.unlink()


class ReviewedTarMemberTests(unittest.TestCase):
    # Purpose: Build owned in-memory archive controls without writing or extracting member payloads.
    # Inputs: Explicit names, types and bytes. Outputs: A complete gzip/TAR fixture.
    @staticmethod
    def archive(members):
        data = io.BytesIO()
        with tarfile.open(fileobj=data, mode="w:gz") as archive:
            for name, kind, payload in members:
                info = tarfile.TarInfo(name)
                info.type = kind
                info.size = len(payload)
                archive.addfile(info, io.BytesIO(payload))
        return data.getvalue()

    # Purpose: Bound decompression before TAR metadata parsing and reject ambiguous or redirected source members.
    # Inputs: Small regular, duplicate, linked, absent and over-limit owned archives.
    # Outputs: Exact bytes only; all malformed or excessive controls raise without extraction.
    def test_bounded_member_admission(self):
        review = {"archive_format": "tar.gz", "member": "public.py"}
        regular = [("public.py", tarfile.REGTYPE, b"owned public example")]
        data = self.archive(regular)
        self.assertEqual(reviewed_member(data, review), regular[0][2])
        for entries in (
            regular + regular,
            [("public.py", tarfile.SYMTYPE, b"")],
            [],
            [("public.py", tarfile.REGTYPE, b"x" * (MAX_RECORD_BYTES + 1))],
        ):
            with self.assertRaises(ValueError):
                reviewed_member(self.archive(entries), review)
        with patch("tools.redact_trufflehog.MAX_REVIEW_EXPANDED_BYTES", 512), self.assertRaises(ValueError):
            reviewed_member(data, review)
        with self.assertRaises(ValueError):
            reviewed_member(data, {**review, "archive_format": "unknown"})


if __name__ == "__main__":
    unittest.main()
