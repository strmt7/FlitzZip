"""Publish location-only TruffleHog findings without retaining secret-bearing records."""

import hashlib
import json
import re
import sys
from typing import BinaryIO, TextIO

MAX_RECORD_BYTES = 1024 * 1024


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
def redact_stream(source: BinaryIO, destination: TextIO) -> int:
    count = 0
    while raw := source.readline(MAX_RECORD_BYTES + 1):
        if len(raw) > MAX_RECORD_BYTES:
            raise ValueError("Finding record exceeds the publication limit")
        if not raw.strip():
            continue
        safe = redact_finding(json.loads(raw.decode("utf-8")))
        destination.write(json.dumps(safe, ensure_ascii=True, separators=(",", ":")) + "\n")
        count += 1
    return count


# Purpose: Fail closed on unreadable reports without echoing JSON, parser context, paths, or exception values.
# Inputs: Scanner JSONL on stdin. Outputs: Redacted stdout, fixed diagnostics, and nonzero on redaction failure.
def main() -> int:
    try:
        count = redact_stream(sys.stdin.buffer, sys.stdout)
        sys.stdout.flush()
    except (ValueError, TypeError, KeyError, OSError, RecursionError):
        print("TruffleHog report redaction failed; untrusted record details were not published.", file=sys.stderr)
        return 2
    print(f"TruffleHog report redaction completed: {count} finding(s).", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
