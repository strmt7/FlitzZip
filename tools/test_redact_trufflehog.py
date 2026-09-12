"""Offline regressions for the secret-report publication boundary."""

import copy
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from tools.redact_trufflehog import MAX_RECORD_BYTES, redact_finding, redact_stream

FIXTURE_PRIVATE = "synthetic-private-fixture-do-not-publish"


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
    # Outputs: Preserved safe findings and diagnostics; only an empty successful scan can pass.
    def test_ci_pipeline_preserves_failures(self):
        bash = shutil.which("bash") if os.name != "nt" else None
        if os.name == "nt" and (git := shutil.which("git")):
            binary = Path(git).resolve()
            candidates = [binary.with_name("bash.exe"), *(p / "bin/bash.exe" for p in binary.parents)]
            bash = next((str(path) for path in candidates if path.is_file()), None)
        self.assertIsNotNone(bash, "Native Bash or Git for Windows Bash is required; WSL must not be used")
        script = Path(__file__).with_name("scan_trufflehog.sh").read_text(encoding="utf-8")
        prelude = 'docker() { printf "%s" "$SCANNER_FIXTURE"; return "$SCANNER_STATUS"; }\n'
        prelude += 'python() { "$PYTHON_FOR_TEST" "$REDACTOR_PATH"; }\n'
        cases = [
            (0, "", 0, 0),
            (0, json.dumps(finding()), 0, 1),
            (183, json.dumps(finding()), 0, 1),
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


if __name__ == "__main__":
    unittest.main()
