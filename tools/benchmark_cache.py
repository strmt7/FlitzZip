"""Content-addressed storage for independently reviewed comparator measurements."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import sys
from datetime import datetime
from pathlib import Path

from tools.benchmark_comparators import fetch_release_source, parse_release, require_permission

ROOT = Path(__file__).resolve().parents[1]
MAX_RECORD_BYTES = 1024 * 1024
IDENTITY_FIELDS = {
    "methodology_sha256",
    "corpus_sha256",
    "corpus_subject",
    "tool",
    "tool_version",
    "binary_sha256",
    "binary_dependencies_sha256",
    "extract_reference_sha256",
    "scope",
    "settings",
    "host",
}


# Purpose: Encode measurement metadata deterministically, rejecting invalid JSON numbers.
# Inputs: JSON-compatible metadata without credentials or personal host identifiers.
# Outputs: Canonical UTF-8 bytes; unsupported values raise a JSON error.
def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode("utf-8")


# Purpose: Bind a comparator result to all material measurement inputs, not SuperZip's current commit.
# Inputs: Method/input/binary/reference hashes, dependency hashes, tool/version/scope, settings and host.
# Outputs: Content-addressed SHA-256 key; missing or malformed identity fails closed.
def measurement_id(identity: dict) -> str:
    if not isinstance(identity, dict) or set(identity) != IDENTITY_FIELDS:
        raise ValueError("measurement identity fields are missing or unknown")
    for name in ("methodology_sha256", "corpus_sha256", "binary_sha256", "extract_reference_sha256"):
        if not isinstance(identity[name], str) or not re.fullmatch(r"[0-9a-f]{64}", identity[name]):
            raise ValueError(f"measurement identity has an invalid {name}")
    for name in ("tool", "tool_version", "corpus_subject"):
        if not isinstance(identity[name], str) or not identity[name].strip():
            raise ValueError(f"measurement identity has an invalid {name}")
    for name in ("settings", "host"):
        if not isinstance(identity[name], dict) or not identity[name]:
            raise ValueError(f"measurement identity requires nonempty {name}")
    dependencies = identity["binary_dependencies_sha256"]
    if not isinstance(dependencies, dict) or any(
        not isinstance(name, str)
        or not name
        or name in (".", "..")
        or ":" in name
        or "/" in name
        or "\\" in name
        or not isinstance(value, str)
        or not re.fullmatch(r"[0-9a-f]{64}", value)
        for name, value in dependencies.items()
    ):
        raise ValueError("measurement identity has invalid binary dependency hashes")
    scope = identity["scope"]
    if not isinstance(scope, dict) or set(scope) != {"edition", "modules"}:
        raise ValueError("measurement identity requires exact edition/module scope")
    if scope["edition"] is not None and (not isinstance(scope["edition"], str) or not scope["edition"].strip()):
        raise ValueError("measurement identity has an invalid edition")
    modules = scope["modules"]
    if (
        not isinstance(modules, list)
        or any(not isinstance(module, str) or not module.strip() for module in modules)
        or modules != sorted(set(modules))
    ):
        raise ValueError("measurement identity modules must be unique and sorted")
    return hashlib.sha256(canonical(identity)).hexdigest()


# Purpose: Reject incomplete, undersampled, corrupted or unreviewed cached measurements.
# Inputs: Original per-tool result with collection date, resource review, timings and correctness proof.
# Outputs: No mutation; raises ValueError when the record cannot be reused.
def validate_result(identity: dict, result: dict) -> None:
    if not isinstance(result, dict) or result.get("tool") != identity["tool"]:
        raise ValueError("cached measurement tool does not match its identity")
    if result.get("independent_verified") is not True or result.get("contention_review") != "accepted":
        raise ValueError("cached measurement requires correctness and contention review")
    if result.get("extract_reference_sha256") != identity["extract_reference_sha256"]:
        raise ValueError("cached measurement extraction reference differs from its identity")
    if not isinstance(result.get("measured_at_utc"), str) or not result["measured_at_utc"]:
        raise ValueError("cached measurement must retain its original collection date")
    collected = datetime.fromisoformat(result["measured_at_utc"])
    if collected.utcoffset() is None or collected.utcoffset().total_seconds() != 0:
        raise ValueError("cached measurement collection date must include UTC")
    for name in ("compress_seconds", "extract_seconds"):
        samples = result.get(name)
        if (
            not isinstance(samples, list)
            or len(samples) < 5
            or any(type(value) not in (int, float) or not math.isfinite(value) or value <= 0 for value in samples)
        ):
            raise ValueError("cached measurement requires at least five finite positive samples per direction")
    sizes = result.get("archive_bytes")
    hashes = result.get("archive_sha256")
    count = len(result["compress_seconds"])
    if not isinstance(sizes, list) or len(sizes) != count or any(type(size) is not int or size <= 0 for size in sizes):
        raise ValueError("cached measurement archive sizes are incomplete or invalid")
    if (
        not isinstance(hashes, list)
        or len(hashes) != count
        or any(not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value) for value in hashes)
    ):
        raise ValueError("cached measurement archive hashes are incomplete or invalid")
    if not isinstance(result.get("resource_context"), dict) or not result["resource_context"]:
        raise ValueError("cached measurement must retain its original resource context")


# Purpose: Read bounded cache/configuration JSON without following a file link.
# Inputs: Existing regular JSON file, at most one MiB.
# Outputs: Parsed JSON; oversized, linked or invalid files raise an error.
def read_json(path: Path) -> dict:
    if path.is_symlink() or not path.is_file():
        raise ValueError("measurement JSON is missing, linked or not a regular file")
    with path.open("rb") as stream:
        payload = stream.read(MAX_RECORD_BYTES + 1)
    if len(payload) > MAX_RECORD_BYTES:
        raise ValueError("measurement JSON exceeds one MiB")
    return json.loads(payload)


# Purpose: Locate a cache entry beneath the repository's ignored benchmark output only.
# Inputs: Caller-selected cache root and already validated content-addressed key.
# Outputs: Safe absolute file path; path escape or linked ancestry raises ValueError.
def cache_path(directory: Path, key: str) -> Path:
    if any(path.is_symlink() or path.is_junction() for path in (directory, *directory.parents)):
        raise ValueError("measurement cache must not traverse links or junctions")
    directory = directory.resolve()
    if not directory.is_relative_to((ROOT / "out" / "benchmarks").resolve()):
        raise ValueError("measurement cache must be beneath repository out/benchmarks")
    return directory / f"{key}.json"


# Purpose: Reuse only an exact identity match with intact original measurement evidence.
# Inputs: Cache root and current measurement identity; a miss is not an error.
# Outputs: Original result or None; damaged/mismatched entries fail, never silently reuse.
def lookup(directory: Path, identity: dict) -> dict | None:
    key = measurement_id(identity)
    path = cache_path(directory, key)
    if not path.exists():
        return None
    entry = read_json(path)
    if not isinstance(entry, dict) or entry.get("schema_version") != 1 or entry.get("identity") != identity:
        raise ValueError("cached measurement schema or identity differs")
    result = entry.get("result")
    actual = hashlib.sha256(canonical(result)).hexdigest()
    if entry.get("result_sha256") != actual:
        raise ValueError("cached measurement checksum differs")
    validate_result(identity, result)
    return result


# Purpose: Store a checked result without replacing existing evidence for the same identity.
# Inputs: Cache root, exact identity and original per-tool measurement result.
# Outputs: New cache file; collisions, write errors or incomplete records fail explicitly.
def store_result(directory: Path, identity: dict, result: dict) -> Path:
    key = measurement_id(identity)
    validate_result(identity, result)
    path = cache_path(directory, key)
    entry = {
        "schema_version": 1,
        "identity": identity,
        "result_sha256": hashlib.sha256(canonical(result)).hexdigest(),
        "result": result,
    }
    payload = canonical(entry) + b"\n"
    if len(payload) > MAX_RECORD_BYTES:
        raise ValueError("cached measurement exceeds one MiB")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(payload)
        stream.flush()
        os.fsync(stream.fileno())
    return path


# Purpose: Import or look up a comparator result without executing software or timing work.
# Inputs: Identity JSON, optional original result, ignored cache root and reviewed edition/module scope.
# Outputs: A cache path or original JSON; permission errors and invalid records return nonzero.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--identity", type=Path, required=True)
    parser.add_argument("--result", type=Path)
    parser.add_argument("--cache-root", type=Path, default=ROOT / "out" / "benchmarks" / "cache")
    args = parser.parse_args()
    identity = read_json(args.identity)
    measurement_id(identity)
    latest = parse_release(identity["tool"], fetch_release_source(identity["tool"]))
    if identity["tool_version"] != latest:
        raise ValueError(f"cached {identity['tool']} version is not the current stable {latest}")
    require_permission(identity["corpus_subject"], "publish_results")
    require_permission(
        identity["tool"],
        "publish_results",
        version=identity["tool_version"],
        edition=identity["scope"]["edition"],
        modules=tuple(identity["scope"]["modules"]),
    )
    if args.result:
        print(store_result(args.cache_root, identity, read_json(args.result)))
        return 0
    result = lookup(args.cache_root, identity)
    if result is None:
        print("measurement cache miss", file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2, allow_nan=False))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"measurement cache failed: {error}", file=sys.stderr)
        sys.exit(1)
