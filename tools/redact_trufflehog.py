"""Publish location-only TruffleHog findings without retaining secret-bearing records."""

import argparse
import hashlib
import io
import json
import re
import stat
import subprocess
import sys
import zipfile
from pathlib import Path
from typing import BinaryIO, TextIO

MAX_RECORD_BYTES = 1024 * 1024
MAX_REVIEW_ARCHIVE_BYTES = 8 * 1024 * 1024


# Purpose: Freeze bounded review inputs without redirects. Inputs: Checkout, exact path and byte limit.
# Outputs: Exact bytes.
def review_bytes(root: Path, name: str, limit: int) -> bytes:
    path = root
    for part in name.split("/"):
        path /= part
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise ValueError("Public fixture review input crosses a reparse point")
    before = path.stat()
    with path.open("rb") as source:
        data = source.read(limit + 1)
    after = path.stat()
    if len(data) > limit or (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError("Public fixture review input changed or exceeds its limit")
    return data


class PublicFixtureReview:
    """Admit only individually approved, exact public adversarial test inputs."""

    # Purpose: Freeze the reviewed identities before reading findings. Inputs: Checkout and scanner image.
    # Outputs: Validated reviews.
    def __init__(self, root: Path, scanner_image: str):
        self.root = root.resolve()
        self.scanner_image = scanner_image
        raw = review_bytes(self.root, ".github/scanner-secret-reviews.json", 65536)
        ledger = json.loads(raw)
        if (
            not isinstance(ledger, dict)
            or set(ledger) != {"schema", "reviews"}
            or type(ledger["schema"]) is not int
            or ledger["schema"] != 1
            or not isinstance(ledger["reviews"], list)
            or len(ledger["reviews"]) > 100
        ):
            raise ValueError("Invalid public fixture review ledger")
        self.reviews = ledger["reviews"]
        self.ledger_sha256 = hashlib.sha256(raw.replace(b"\r\n", b"\n")).hexdigest()
        self.total = self.reviewed = 0
        self.validated = set()
        for review in self.reviews:
            self.validate_review(review)
        identifiers = [review["id"] for review in self.reviews]
        if len(set(identifiers)) != len(identifiers):
            raise ValueError("Duplicate public fixture review")

    # Purpose: Reject wildcard/family admission and malformed commitments. Inputs: One review. Outputs: Exact
    # validated schema or failure.
    def validate_review(self, review: dict) -> None:
        fields = {
            "id",
            "evidence",
            "scanner_image",
            "detector",
            "archive_path",
            "archive_sha256",
            "member",
            "member_sha256",
            "line",
            "raw_v2_sha256",
        }
        if not isinstance(review, dict) or set(review) != fields:
            raise ValueError("Invalid public fixture review fields")
        for field in ("archive_path", "member", "evidence"):
            name = review[field]
            if (
                not isinstance(name, str)
                or not re.fullmatch(r"[A-Za-z0-9_./-]+", name)
                or ".." in name.split("/")
                or name.startswith("/")
            ):
                raise ValueError("Review requires a canonical exact path")
        for field in ("archive_sha256", "member_sha256", "raw_v2_sha256"):
            if not isinstance(review[field], str) or not re.fullmatch(r"[0-9a-f]{64}", review[field]):
                raise ValueError("Review requires complete SHA-256 commitments")
        if not re.fullmatch(r"[a-z0-9-]{1,80}", review["id"]) or not isinstance(review["scanner_image"], str):
            raise ValueError("Invalid public fixture review identity")
        if (
            type(review["line"]) is not int
            or review["line"] < 1
            or type(review["detector"]) is not int
            or review["detector"] < 0
        ):
            raise ValueError("Invalid public fixture review location")
        if not (self.root / review["evidence"]).is_file():
            raise ValueError("Public fixture review evidence is missing")

    # Purpose: Verify the actual historical blob and current complete source. Inputs: Review, immutable Git commit
    # and match. Outputs: Exact-source verdict.
    def source_matches(self, review: dict, commit: str, value: bytes) -> bool:
        key = (review["id"], commit)
        path = review["archive_path"]
        current = review_bytes(self.root, path, MAX_REVIEW_ARCHIVE_BYTES)
        if hashlib.sha256(current).hexdigest() != review["archive_sha256"]:
            return False
        if key in self.validated:
            return True
        reference = f"{commit}:{path}"
        size = subprocess.check_output(
            ["git", "cat-file", "-s", reference], cwd=self.root, timeout=10, stderr=subprocess.DEVNULL
        )
        if not size.strip().isdigit() or int(size) > MAX_REVIEW_ARCHIVE_BYTES:
            return False
        data = subprocess.check_output(["git", "show", reference], cwd=self.root, timeout=10, stderr=subprocess.DEVNULL)
        if len(data) != int(size) or hashlib.sha256(data).hexdigest() != review["archive_sha256"]:
            return False
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            members = archive.infolist()
            if len(members) > 2000 or len({member.filename for member in members}) != len(members):
                return False
            member = archive.getinfo(review["member"])
            if member.file_size > MAX_RECORD_BYTES:
                return False
            source = archive.read(member)
        if hashlib.sha256(source).hexdigest() != review["member_sha256"]:
            return False
        lines = source.splitlines()
        if review["line"] > len(lines) or lines[review["line"] - 1].count(value) != 1:
            return False
        self.validated.add(key)
        return True

    # Purpose: Count every finding and identify only its exact approved public fixture. Inputs: Validated scanner
    # record. Outputs: Review verdict.
    def consider(self, record: dict) -> bool:
        self.total += 1
        source = record["SourceMetadata"]["Data"]["Git"]
        value = record.get("RawV2")
        if record["Verified"] or not record.get("VerificationError") or not isinstance(value, str) or not value:
            return False
        value = value.encode("utf-8")
        for review in self.reviews:
            if (
                self.scanner_image == review["scanner_image"]
                and record["DetectorType"] == review["detector"]
                and source["file"] == review["archive_path"]
                and source["line"] == review["line"]
                and hashlib.sha256(value).hexdigest() == review["raw_v2_sha256"]
                and self.source_matches(review, source["commit"], value)
            ):
                self.reviewed += 1
                return True
        return False

    # Purpose: Preserve value-free adjudication alongside every sanitized raw report. Inputs: Destination.
    # Outputs: Complete counts and identities.
    def publish(self, destination: Path) -> None:
        report = {
            "schema": 1,
            "scanner_image": self.scanner_image,
            "ledger_sha256": self.ledger_sha256,
            "total": self.total,
            "reviewed_public_fixtures": self.reviewed,
            "unresolved": self.total - self.reviewed,
        }
        destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


# Purpose: Validate scanner-owned numeric metadata without accepting booleans or unbounded integers.
# Inputs: Untrusted JSON metadata. Outputs: A bounded nonnegative integer or a value-free error.
def metadata_integer(value: object) -> int:
    if type(value) is not int or not 0 <= value <= (1 << 63) - 1:
        raise ValueError("Invalid numeric finding metadata")
    return value


# Purpose: Retain each finding's detector and reproducible location, never its raw or auxiliary secret data.
# Inputs: An untrusted TruffleHog Git JSON record. Outputs: An allowlisted public report or a value-free error.
def redact_finding(record: object) -> dict:
    if not isinstance(record, dict) or type(record.get("Verified")) is not bool:
        raise ValueError("Invalid finding record")
    detector = metadata_integer(record.get("DetectorType"))
    source = record["SourceMetadata"]["Data"]["Git"]
    if not isinstance(source, dict):
        raise ValueError("Missing Git finding location")
    commit = source.get("commit")
    path = source.get("file")
    if not isinstance(commit, str) or re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", commit) is None:
        raise ValueError("Invalid Git finding commit")
    if not isinstance(path, str) or not path or len(path) > 32767:
        raise ValueError("Invalid Git finding path")
    return {
        "schema_version": 1,
        "detector_id": detector,
        "verified": record["Verified"],
        "verification_error": bool(record.get("VerificationError")),
        "location": {
            "commit": commit,
            "line": metadata_integer(source.get("line")),
            # A filename can itself contain a secret; resolve its digest in a private local scan.
            "path_sha256": hashlib.sha256(path.encode("utf-8")).hexdigest(),
        },
    }


# Purpose: Redact a JSONL stream before any scanner record reaches a log or artifact.
# Inputs: Binary scanner stdout and a text destination. Outputs: Safe JSONL and the finding count; bounds each record.
def redact_stream(source: BinaryIO, destination: TextIO, review: PublicFixtureReview | None = None) -> int:
    count = 0
    while raw := source.readline(MAX_RECORD_BYTES + 1):
        if len(raw) > MAX_RECORD_BYTES:
            raise ValueError("Finding record exceeds the publication limit")
        if not raw.strip():
            continue
        record = json.loads(raw.decode("utf-8"))
        safe = redact_finding(record)
        destination.write(json.dumps(safe, ensure_ascii=True, separators=(",", ":")) + "\n")
        if review is not None:
            review.consider(record)
        count += 1
    return count


# Purpose: Fail closed on unreadable reports without echoing JSON, parser context, paths, or exception values.
# Inputs: Scanner JSONL on stdin. Outputs: Redacted stdout, fixed diagnostics, and nonzero on redaction failure.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scanner-image")
    parser.add_argument("--review-report", type=Path)
    arguments = parser.parse_args()
    try:
        if bool(arguments.scanner_image) != bool(arguments.review_report):
            raise ValueError("Review requires both scanner identity and report")
        review = PublicFixtureReview(Path.cwd(), arguments.scanner_image) if arguments.scanner_image else None
        count = redact_stream(sys.stdin.buffer, sys.stdout, review)
        sys.stdout.flush()
        if review is not None:
            review.publish(arguments.review_report)
    except (ValueError, TypeError, KeyError, OSError, RuntimeError, subprocess.SubprocessError, zipfile.BadZipFile):
        print("TruffleHog report redaction failed; untrusted record details were not published.", file=sys.stderr)
        return 2
    print(f"TruffleHog report redaction completed: {count} finding(s).", file=sys.stderr)
    if review is not None:
        if review.total != review.reviewed:
            return 3
        if review.total:
            return 4
    return 0


if __name__ == "__main__":
    sys.exit(main())
