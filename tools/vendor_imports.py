"""Prove explicitly admitted upstream text imports without excluding scanner input.

Only the registered LZ4 component can receive this classification. Its existing
SHA256SUMS is the archive identity; member bytes, not self-declared source hashes,
prove the complete production copy. Any local adaptation removes the component's
classification and leaves the ordinary changed-function gates applicable.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import re
import sys
import tarfile
import zlib
from pathlib import Path, PurePosixPath

if __package__:
    from .json_input import decode_json
    from .native_build_provenance import input_path
else:
    from json_input import decode_json
    from native_build_provenance import input_path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = "third_party/lz4/upstream.json"
SOURCE_ROOT = "third_party/lz4/"
SOURCE_NAMES = frozenset(("lz4.c", "lz4.h", "LICENSE"))
MAX_ARCHIVE_BYTES = 8 * 1024 * 1024
MAX_EXPANDED_BYTES = 32 * 1024 * 1024
MAX_SOURCE_BYTES = 2 * 1024 * 1024
MAX_MEMBERS = 4096


def bounded_input(root: Path, name: str, maximum: int) -> bytes:
    """Read one regular, contained input; reject size drift and concurrent writes."""
    path = input_path(root, name)
    before = path.stat()
    if before.st_size > maximum:
        raise ValueError("Vendor import input exceeds its byte budget")
    with path.open("rb") as stream:
        result = stream.read(maximum + 1)
    after = path.stat()
    if len(result) > maximum or (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError("Vendor import input changed during reading")
    return result


def import_manifest(root: Path) -> dict:
    """Admit only the fixed publisher, complete member set and canonical provenance layout."""
    value = decode_json(bounded_input(root, MANIFEST, 65536))
    fields = {"schema_version", "component", "version", "url", "archive", "files"}
    if not isinstance(value, dict) or set(value) != fields:
        raise ValueError("Vendor import manifest has an unknown schema")
    version = value["version"]
    if type(value["schema_version"]) is not int or value["schema_version"] != 1 or value["component"] != "lz4":
        raise ValueError("Vendor import component or schema is not admitted")
    if not isinstance(version, str) or re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) is None:
        raise ValueError("Vendor import release identity is invalid")
    files = value["files"]
    if not isinstance(files, list) or len(files) != len(SOURCE_NAMES) or any(type(name) is not str for name in files):
        raise ValueError("Vendor import member inventory is incomplete")
    if set(files) != SOURCE_NAMES:
        raise ValueError("Vendor import member inventory is outside its admitted scope")
    if value["archive"] != f"third_party/upstream/lz4/{version}/lz4-{version}.tar.gz":
        raise ValueError("Vendor import archive path differs from its version")
    admitted_urls = {
        f"https://github.com/lz4/lz4/releases/download/v{version}/lz4-{version}.tar.gz",
        f"https://github.com/lz4/lz4/archive/refs/tags/v{version}.tar.gz",
    }
    if not isinstance(value["url"], str) or value["url"] not in admitted_urls:
        raise ValueError("Vendor import publisher differs from its admitted origin")
    return value


def release_sources(root: Path, manifest: dict) -> tuple[str, dict[str, bytes]]:
    """Authenticate the complete archive, then read bounded regular members without extraction."""
    archive = manifest["archive"]
    checksum_path = PurePosixPath(archive).with_name("SHA256SUMS").as_posix()
    checksums = bounded_input(root, checksum_path, 1024).decode("ascii")
    match = re.fullmatch(r"([a-f0-9]{64})  " + re.escape(PurePosixPath(archive).name) + r"\r?\n?", checksums)
    if match is None:
        raise ValueError("Vendor import checksum inventory is not exact")
    compressed = bounded_input(root, archive, MAX_ARCHIVE_BYTES)
    digest = hashlib.sha256(compressed).hexdigest()
    if digest != match[1]:
        raise ValueError("Vendor import archive digest mismatch")
    with gzip.GzipFile(fileobj=io.BytesIO(compressed)) as stream:
        decoded = stream.read(MAX_EXPANDED_BYTES + 1)
    if len(decoded) > MAX_EXPANDED_BYTES:
        raise ValueError("Vendor import archive expansion exceeds its byte budget")
    wanted = {f"lz4-{manifest['version']}/lib/{name}": name for name in SOURCE_NAMES}
    selected: dict[str, bytes] = {}
    seen: set[str] = set()
    with tarfile.open(fileobj=io.BytesIO(decoded), mode="r:") as source:
        for count, member in enumerate(source, 1):
            if count > MAX_MEMBERS:
                raise ValueError("Vendor import archive exceeds its member budget")
            path = PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts or ":" in member.name or "\\" in member.name:
                raise ValueError("Vendor import archive contains an unsafe member path")
            if member.name in seen:
                raise ValueError("Vendor import archive contains duplicate members")
            seen.add(member.name)
            if member.name not in wanted:
                continue
            if not member.isfile() or not 0 <= member.size <= MAX_SOURCE_BYTES:
                raise ValueError("Vendor import source member is linked or oversized")
            with source.extractfile(member) as stream:
                payload = stream.read(MAX_SOURCE_BYTES + 1)
            if len(payload) != member.size:
                raise ValueError("Vendor import source member is incomplete")
            selected[wanted[member.name]] = payload.replace(b"\r\n", b"\n")
    if set(selected) != SOURCE_NAMES:
        raise ValueError("Vendor import archive lacks an exact source member")
    return digest, selected


def classify(root: Path = ROOT) -> dict:
    """Classify the complete component; malformed provenance fails instead of granting admission.

    Local adaptations are reported separately and receive no numeric-gate
    exemption. All source files and security findings remain scanner inputs.
    """
    manifest = import_manifest(root)
    archive_digest, upstream = release_sources(root, manifest)
    hashes, adapted = {}, []
    for name in sorted(SOURCE_NAMES):
        path = SOURCE_ROOT + name
        current = bounded_input(root, path, MAX_SOURCE_BYTES).replace(b"\r\n", b"\n")
        hashes[path] = hashlib.sha256(current).hexdigest()
        if current != upstream[name]:
            adapted.append(path)
    return {
        "schema_version": 1,
        "component": "lz4",
        "version": manifest["version"],
        "archive": manifest["archive"],
        "archive_sha256": archive_digest,
        "canonical_paths": [] if adapted else sorted(hashes),
        "adapted_paths": adapted,
        "source_sha256": hashes,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument(
        "--require-canonical", action="store_true", help="Reject local adaptations during import qualification"
    )
    args = parser.parse_args()
    result = classify(args.root)
    if args.require_canonical and result["adapted_paths"]:
        raise ValueError("Vendor import has local adaptations; ordinary function gates remain required")
    print(json.dumps(result, sort_keys=True, allow_nan=False))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, EOFError, ValueError, RecursionError, tarfile.TarError, zlib.error) as error:
        print(f"Vendor import proof failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
