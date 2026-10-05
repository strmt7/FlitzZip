"""Publish location-only TruffleHog findings without retaining secret-bearing records."""

import argparse
import gzip
import hashlib
import io
import json
import re
import stat
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path
from typing import BinaryIO, TextIO

MAX_RECORD_BYTES = 1024 * 1024
MAX_REVIEW_ARCHIVE_BYTES = 8 * 1024 * 1024
MAX_REVIEW_EXPANDED_BYTES = 64 * 1024 * 1024


# Purpose: Read one exact approved member without extracting files or accepting ambiguous archive names.
# Inputs: Authenticated bounded archive bytes and an exact review; TAR expansion is separately bounded.
# Outputs: Complete bounded member bytes; malformed, duplicate, linked or oversized inputs fail closed.
def reviewed_member(data: bytes, review: dict) -> bytes:
    if review.get("archive_format", "zip") == "zip":
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            members = archive.infolist()
            if len(members) > 2000 or len({member.filename for member in members}) != len(members):
                raise ValueError("Public fixture archive has ambiguous member names")
            member = archive.getinfo(review["member"])
            if member.file_size > MAX_RECORD_BYTES:
                raise ValueError("Public fixture member exceeds its byte limit")
            return archive.read(member)
    if review.get("archive_format") != "tar.gz":
        raise ValueError("Public fixture archive format is unsupported")
    with gzip.GzipFile(fileobj=io.BytesIO(data)) as compressed:
        uncompressed = compressed.read(MAX_REVIEW_EXPANDED_BYTES + 1)
    if len(uncompressed) > MAX_REVIEW_EXPANDED_BYTES:
        raise ValueError("Public fixture archive exceeds its expanded byte limit")
    selected = None
    expanded = 0
    names = set()
    with tarfile.open(fileobj=io.BytesIO(uncompressed), mode="r:") as archive:
        for member in archive:
            if len(names) >= 2000 or member.name in names:
                raise ValueError("Public fixture archive has ambiguous member names")
            names.add(member.name)
            if not (member.isfile() or member.isdir()) or member.size < 0:
                raise ValueError("Public fixture archive has unsupported member types")
            if member.size > MAX_REVIEW_EXPANDED_BYTES - expanded:
                raise ValueError("Public fixture archive exceeds its expanded byte limit")
            expanded += member.size
            if member.name != review["member"]:
                continue
            if not member.isfile() or member.size > MAX_RECORD_BYTES:
                raise ValueError("Public fixture member exceeds its byte limit")
            with archive.extractfile(member) as source:
                selected = source.read(MAX_RECORD_BYTES + 1)
            if len(selected) != member.size:
                raise ValueError("Public fixture member is incomplete")
    if selected is None:
        raise ValueError("Public fixture member is missing")
    return selected


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
    """Admit only individually approved, exact public fixtures and documentation examples."""

    # Purpose: Freeze the reviewed identities before reading findings. Inputs: Checkout and scanner image.
    # Outputs: Validated reviews.
    def __init__(self, root: Path, scanner_image: str):
        self.root = root.resolve()
        self.scanner_image = scanner_image
        self.reviews = []
        self.ledger_hashes = {}
        for name in (".github/scanner-secret-reviews.json", ".github/scanner-secret-reviews-crawl4ai.json"):
            if name.endswith("-crawl4ai.json") and not (self.root / name).exists():
                continue
            raw = review_bytes(self.root, name, 65536)
            self.load_ledger(json.loads(raw))
            self.ledger_hashes[name] = hashlib.sha256(raw.replace(b"\r\n", b"\n")).hexdigest()
        self.ledger_sha256 = self.ledger_hashes[".github/scanner-secret-reviews.json"]
        self.total = self.reviewed = 0
        self.validated = set()
        for review in self.reviews:
            self.validate_review(review)
        identifiers = [review["id"] for review in self.reviews]
        if len(set(identifiers)) != len(identifiers):
            raise ValueError("Duplicate public fixture review")

    # Purpose: Admit bounded independent approval cohorts without changing earlier source commitments.
    # Inputs: Parsed ledger with the original strict schema. Outputs: Appends exact reviews or fails closed.
    def load_ledger(self, ledger: object) -> None:
        if (
            not isinstance(ledger, dict)
            or set(ledger) != {"schema", "reviews"}
            or type(ledger["schema"]) is not int
            or ledger["schema"] != 1
            or not isinstance(ledger["reviews"], list)
            or len(ledger["reviews"]) > 100 - len(self.reviews)
        ):
            raise ValueError("Invalid public fixture review ledger")
        self.reviews.extend(ledger["reviews"])

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
        extended_fields = {"archive_format", "source_line"}
        if not isinstance(review, dict) or set(review) not in (fields, fields | extended_fields):
            raise ValueError("Invalid public fixture review fields")
        if set(review) == fields | extended_fields and (
            review["archive_format"] not in ("zip", "tar.gz")
            or type(review["source_line"]) is not int
            or review["source_line"] < 1
        ):
            raise ValueError("Invalid public fixture member format or source location")
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
        source = reviewed_member(data, review)
        if hashlib.sha256(source).hexdigest() != review["member_sha256"]:
            return False
        lines = source.splitlines()
        source_line = review.get("source_line", review["line"])
        if source_line > len(lines) or lines[source_line - 1].count(value) != 1:
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
            "ledger_sha256_by_path": self.ledger_hashes,
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
    except (
        ValueError,
        TypeError,
        KeyError,
        OSError,
        RuntimeError,
        EOFError,
        subprocess.SubprocessError,
        zipfile.BadZipFile,
        tarfile.TarError,
    ):
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
