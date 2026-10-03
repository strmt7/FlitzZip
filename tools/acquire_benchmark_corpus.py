"""Acquire pinned, permission-reviewed complete-row corpus excerpts without staging the full archive."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import time
import zipfile
from datetime import UTC, datetime
from pathlib import Path
from urllib.parse import urlsplit
from urllib.request import Request, build_opener

from tools.benchmark_cache import read_json
from tools.benchmark_comparators import NoReleaseRedirects, require_permission
from tools.benchmark_corpus import metadata_digest
from tools.run_archive_comparison import MAX_FILESYSTEM_BYTES, verify_manifest

MAX_ARCHIVE_BYTES = 32 * 1024 * 1024
MAX_MEMBER_BYTES = 256 * 1024 * 1024
CHUNK_BYTES = 1024 * 1024


# Purpose: Require typed source pins, reviewed HTTPS endpoints and distinct bounded excerpt geometry.
# Inputs: One bounded source-pin JSON object and the existing permission catalog.
# Outputs: Returns the unchanged specification or raises before network, allocation or output creation.
def validate_source(spec: dict) -> dict:
    keys = {
        "schema_version",
        "name",
        "subject",
        "source_url",
        "download_url",
        "archive_bytes",
        "archive_sha256",
        "member",
        "windows",
        "attribution",
    }
    if (
        not isinstance(spec, dict)
        or set(spec) != keys
        or type(spec["schema_version"]) is not int
        or spec["schema_version"] != 1
    ):
        raise ValueError("corpus source fields or schema are invalid")
    for field, maximum in (("name", 120), ("subject", 120), ("attribution", 4000)):
        if not isinstance(spec[field], str) or not spec[field].strip() or len(spec[field]) > maximum:
            raise ValueError(f"corpus source {field} is absent or too long")
    permission = require_permission(spec["subject"], "execute")
    for field in ("source_url", "download_url"):
        if not isinstance(spec[field], str) or spec[field] not in permission["evidence"]:
            raise ValueError("corpus endpoints must be reviewed permission evidence")
        url = urlsplit(spec[field])
        if url.scheme != "https" or not url.hostname or url.username or url.password or url.fragment or url.query:
            raise ValueError("corpus endpoints must be credential-free HTTPS without query or fragment")
    member = spec["member"]
    if not isinstance(member, dict) or set(member) != {"name", "bytes", "sha256"}:
        raise ValueError("corpus member pin is invalid")
    if not isinstance(member["name"], str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,119}", member["name"]):
        raise ValueError("corpus member must have an exact flat pinned name")
    for value, maximum in ((spec["archive_bytes"], MAX_ARCHIVE_BYTES), (member["bytes"], MAX_MEMBER_BYTES)):
        if type(value) is not int or not 1 <= value <= maximum:
            raise ValueError("corpus archive or member size exceeds the acquisition boundary")
    for value in (spec["archive_sha256"], member["sha256"]):
        if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
            raise ValueError("corpus source SHA-256 pin is invalid")
    windows = spec["windows"]
    if not isinstance(windows, list) or not 1 <= len(windows) <= 8:
        raise ValueError("corpus source requires 1-8 distinct byte windows")
    ranges, labels = [], set()
    for window in windows:
        if not isinstance(window, dict) or set(window) != {"label", "offset", "bytes"}:
            raise ValueError("corpus byte-window fields are invalid")
        label = window["label"]
        if (
            not isinstance(label, str)
            or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]{0,63}", label)
            or label.casefold() in labels
        ):
            raise ValueError("corpus window labels are unsafe or repeated")
        labels.add(label.casefold())
        offset, size = window["offset"], window["bytes"]
        if (
            type(offset) is not int
            or type(size) is not int
            or offset < 0
            or size < 1
            or offset + size > member["bytes"]
        ):
            raise ValueError("corpus byte window is outside its pinned member")
        ranges.append((offset, offset + size))
    ranges.sort()
    if sum(end - start for start, end in ranges) > MAX_FILESYSTEM_BYTES or any(
        left[1] > right[0] for left, right in zip(ranges, ranges[1:], strict=False)
    ):
        raise ValueError("corpus windows overlap or exceed the aggregate 64 MiB boundary")
    return spec


# Purpose: Download only the reviewed pinned archive with bounded RAM, network bytes and lifetime.
# Inputs: Validated source specification; caller must run inside the repository's memory-admitted process boundary.
# Outputs: Exact response bytes or an error; chunked delivery is supported and redirects are never followed.
def download_archive(spec: dict) -> bytes:
    deadline = time.monotonic() + 180
    payload = bytearray()
    request = Request(spec["download_url"], headers={"Accept-Encoding": "identity"})
    with build_opener(NoReleaseRedirects()).open(request, timeout=30) as response:
        length = response.headers.get("Content-Length")
        if (
            response.status != 200
            or response.headers.get("Content-Encoding", "identity") != "identity"
            or (length is not None and int(length) != spec["archive_bytes"])
        ):
            raise ValueError("corpus HTTP status, encoding or declared size differs from the pin")
        while block := response.read(CHUNK_BYTES):
            if len(payload) + len(block) > spec["archive_bytes"] or time.monotonic() > deadline:
                raise ValueError("corpus download exceeded its pinned byte or lifetime boundary")
            payload.extend(block)
    if time.monotonic() > deadline:
        raise ValueError("corpus download exceeded its lifetime boundary at EOF")
    if len(payload) != spec["archive_bytes"]:
        raise ValueError("corpus response is incomplete")
    return bytes(payload)


# Purpose: Retain only literal complete LF-delimited rows, including original CRLF when present.
# Inputs: One captured window including its preceding byte when offset is nonzero, plus declared geometry.
# Outputs: Returns exact contiguous payload and source range; empty or incomplete-only windows fail.
def trim_rows(captured: bytearray, window: dict) -> tuple[bytes, int, int]:
    start = 0 if window["offset"] == 0 else captured.find(b"\n") + 1
    end = captured.rfind(b"\n") + 1
    if (start <= 0 and window["offset"] != 0) or end <= start:
        raise ValueError("corpus byte window contains no complete rows")
    capture_offset = max(0, window["offset"] - 1)
    return bytes(captured[start:end]), capture_offset + start, capture_offset + end


# Purpose: Authenticate the complete archive/member while retaining bounded, non-overlapping source excerpts.
# Inputs: Immutable downloaded bytes and validated source pins; all ZIP members and decoded bytes must match exactly.
# Outputs: Literal complete-row excerpts with exact offsets/counts/hashes; corrupt sources fail before writing.
def decode_windows(payload: bytes, spec: dict) -> list[dict]:
    if len(payload) != spec["archive_bytes"] or hashlib.sha256(payload).hexdigest() != spec["archive_sha256"]:
        raise ValueError("corpus archive bytes or SHA-256 differ from the pin")
    buffers = [bytearray() for _ in spec["windows"]]
    total, source_hash = 0, hashlib.sha256()
    deadline = time.monotonic() + 120
    with zipfile.ZipFile(io.BytesIO(payload)) as archive:
        members = archive.infolist()
        if (
            len(members) != 1
            or members[0].filename != spec["member"]["name"]
            or members[0].file_size != spec["member"]["bytes"]
            or members[0].is_dir()
            or members[0].flag_bits & 1
        ):
            raise ValueError("corpus archive member geometry differs from the pin")
        with archive.open(members[0]) as stream:
            while block := stream.read(CHUNK_BYTES):
                next_total = total + len(block)
                if next_total > spec["member"]["bytes"] or time.monotonic() > deadline:
                    raise ValueError("corpus decoded member exceeded its extent or lifetime boundary")
                source_hash.update(block)
                for window, buffer in zip(spec["windows"], buffers, strict=True):
                    begin, end = (
                        max(total, max(0, window["offset"] - 1)),
                        min(next_total, window["offset"] + window["bytes"]),
                    )
                    if begin < end:
                        buffer.extend(block[begin - total : end - total])
                total = next_total
    if time.monotonic() > deadline:
        raise ValueError("corpus decoded member exceeded its lifetime boundary at EOF")
    if total != spec["member"]["bytes"] or source_hash.hexdigest() != spec["member"]["sha256"]:
        raise ValueError("corpus decoded member differs from its pinned SHA-256 or extent")
    rows = []
    for window, buffer in zip(spec["windows"], buffers, strict=True):
        data, start, end = trim_rows(buffer, window)
        rows.append(
            {
                "label": window["label"],
                "payload": data,
                "start": start,
                "end": end,
                "line_count": data.count(b"\n"),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
        )
    return rows


# Purpose: Publish a new corpus directory only after complete source authentication, without replacing existing files.
# Inputs: Reviewed source-pin path and caller-selected new destination; acquisition has a 64 MiB aggregate payload cap.
# Outputs: Writes literal excerpts, attribution, acquisition evidence and the canonical verified comparison manifest.
def acquire(source_pin: Path, destination: Path) -> dict:
    if (
        any(path.is_symlink() or path.is_junction() for path in (destination, *destination.parents))
        or destination.exists()
    ):
        raise ValueError("corpus acquisition refuses existing or linked destinations")
    pin_hash = metadata_digest(source_pin)
    spec = validate_source(read_json(source_pin))
    if metadata_digest(source_pin) != pin_hash:
        raise ValueError("corpus source pins changed during admission")
    excerpts = decode_windows(download_archive(spec), spec)
    if metadata_digest(source_pin) != pin_hash:
        raise ValueError("corpus source pins changed during acquisition")
    files, cases = [], []
    for row in excerpts:
        name = f"corpus-{row['label']}.txt"
        transformation = (
            f"Literal complete rows at source byte range [{row['start']},{row['end']}); "
            "retained line endings and missing values; no padding, repetition or numeric conversion. "
            f"Complete source member SHA-256 {spec['member']['sha256']}; "
            f"archive SHA-256 {spec['archive_sha256']}; acquisition pin SHA-256 {pin_hash}. {spec['attribution']}"
        )
        files.append(
            {
                "name": name,
                "bytes": len(row["payload"]),
                "sha256": row["sha256"],
                "subject": spec["subject"],
                "source_url": spec["source_url"],
                "transformation": transformation,
            }
        )
        cases.append({"name": row["label"], "format": "zst", "files": [name], "tools": ["SuperZip", "Zstd"]})
    manifest = {"schema_version": 1, "name": spec["name"], "files": files, "cases": cases}
    destination.parent.mkdir(parents=True, exist_ok=True)
    if any(path.is_symlink() or path.is_junction() for path in (destination, *destination.parents)):
        raise ValueError("corpus destination became linked before publication")
    destination.mkdir()
    for row, file in zip(excerpts, files, strict=True):
        with (destination / file["name"]).open("xb") as output:
            output.write(row["payload"])
    manifest_path = destination / "manifest.json"
    with manifest_path.open("x", encoding="utf-8", newline="\n") as output:
        json.dump(manifest, output, indent=2)
        output.write("\n")
    with (destination / "ATTRIBUTION.txt").open("x", encoding="utf-8", newline="\n") as output:
        output.write(spec["attribution"] + "\n")
    _, _, metadata = verify_manifest(manifest_path, destination)
    evidence = {
        "source_pin_sha256": pin_hash,
        "source_url": spec["source_url"],
        "download_url": spec["download_url"],
        "archive_bytes": spec["archive_bytes"],
        "archive_sha256": spec["archive_sha256"],
        "member": spec["member"],
        "complete_member_crc_verified": True,
        "acquired_utc": datetime.now(UTC).isoformat(),
        "payload_disk_write_bytes": sum(file["bytes"] for file in files),
        "corpus": metadata,
        "selection": [{key: value for key, value in row.items() if key != "payload"} for row in excerpts],
    }
    with (destination / "acquisition.json").open("x", encoding="utf-8", newline="\n") as output:
        json.dump(evidence, output, indent=2)
        output.write("\n")
    return evidence


# Purpose: Acquire one reviewed pinned corpus without shell commands or automatic benchmark execution.
# Inputs: Explicit source pins and a new destination; the repository runner provides process memory admission.
# Outputs: Prints bounded public acquisition evidence; failures propagate and never overwrite or delete prior data.
def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-pin", required=True, type=Path)
    parser.add_argument("--destination", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(acquire(args.source_pin, args.destination), ensure_ascii=True))


if __name__ == "__main__":
    main()
