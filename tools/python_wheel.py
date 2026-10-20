"""Shared admission and deterministic packaging for pure Python wheels."""

from __future__ import annotations

import base64
import csv
import datetime
import hashlib
import io
import json
import re
import stat
import zipfile
from email.parser import BytesParser
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parent.parent
BUILD_LOCK = ROOT / "tools/requirements/python-build.txt"
MAX_SOURCE_BYTES = 64 * 1024 * 1024
MAX_RECIPE_BYTES = 1024 * 1024


def recipe_object(pairs: list[tuple[str, object]]) -> dict:
    """Purpose: Reject conflicting provenance. Inputs: Decoded recipe fields. Outputs: Unique object or refusal."""
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Python packaging recipe contains duplicate fields")
        result[key] = value
    return result


def reject_recipe_fraction(value: str) -> None:
    """Purpose: Require integral recipe numbers. Inputs: JSON numeric token. Outputs: Refusal without its value."""
    raise ValueError("Python packaging recipe numbers must be integers")


def read_recipe(path: Path) -> dict:
    """Purpose: Admit bounded, unambiguous packaging metadata before any artifact is acquired or built.
    Inputs: Recipe path; supported recipes use only integral numeric fields and a top-level object.
    Outputs: Decoded object or ValueError; duplicate, nonfinite, fractional and oversized inputs fail closed.
    """
    with path.open("rb") as stream:
        payload = stream.read(MAX_RECIPE_BYTES + 1)
    if not payload or len(payload) > MAX_RECIPE_BYTES:
        raise ValueError("Python packaging recipe exceeds its metadata boundary")
    data = json.loads(
        payload.decode("utf-8"),
        object_pairs_hook=recipe_object,
        parse_float=reject_recipe_fraction,
        parse_constant=reject_recipe_fraction,
    )
    if not isinstance(data, dict):
        raise ValueError("Python packaging recipe must be an object")
    return data


def digest(path: Path) -> str:
    """Purpose: Measure bounded artifacts. Inputs: File. Outputs: SHA-256 or size refusal."""
    if path.stat().st_size > MAX_SOURCE_BYTES:
        raise ValueError("Python artifact exceeds its reviewed size budget")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expected_digest(path: Path) -> str:
    """Purpose: Read exact artifact admission. Inputs: SHA-256 sidecar. Outputs: Single digest or refusal."""
    fields = path.read_text(encoding="ascii").split()
    if len(fields) != 2 or not re.fullmatch(r"[a-f0-9]{64}", fields[0]):
        raise ValueError("Malformed Python artifact digest")
    return fields[0]


def wheel_files(path: Path) -> dict[str, bytes]:
    """Purpose: Admit complete pure-wheel payloads before use.
    Inputs: Bounded ZIP file. Outputs: Regular canonical members; rejects redirects, aliases and oversized content.
    """
    if path.stat().st_size > MAX_SOURCE_BYTES:
        raise ValueError("Python wheel exceeds its reviewed extent")
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        if len(entries) > 2000 or sum(item.file_size for item in entries) > MAX_SOURCE_BYTES:
            raise ValueError("Python wheel exceeds its reviewed extent")
        seen = set()
        for item in entries:
            name = item.filename
            relative = PurePosixPath(name)
            kind = (item.external_attr >> 16) & 0o170000
            if (
                not relative.parts
                or name != item.orig_filename
                or relative.is_absolute()
                or relative.as_posix() != name.rstrip("/")
                or ".." in relative.parts
                or "\\" in name
                or ":" in name
                or any(part != part.rstrip(" .") for part in relative.parts)
                or kind not in (0, stat.S_IFREG, stat.S_IFDIR)
                or (kind == stat.S_IFDIR and not item.is_dir())
                or (kind == stat.S_IFREG and item.is_dir())
                or name.casefold() in seen
            ):
                raise ValueError("Unsafe or duplicate Python wheel member")
            seen.add(name.casefold())
        return {item.filename: archive.read(item) for item in entries if not item.is_dir()}


def canonical_wheel(built: Path, destination: Path, data: dict, *, project: str) -> None:
    """Purpose: Make pure wheels reproducible without accepting ambiguous package identities.
    Inputs: Standard wheel and recipe. Outputs: Canonical wheel; missing or repeated identity headers fail first.
    """
    files = wheel_files(built)
    info = f"{project}-{data['version']}.dist-info/"
    metadata_path = info + "METADATA"
    metadata = BytesParser().parsebytes(files[metadata_path])
    names = metadata.get_all("Name", [])
    if (
        len(names) != 1
        or not isinstance(names[0], str)
        or names[0].casefold() != project
        or metadata.get_all("Version") != [data["version"]]
    ):
        raise ValueError(f"{project} wheel identity mismatch")
    # Setuptools writes platform line endings; normalize generated metadata only.
    files[metadata_path] = files[metadata_path].replace(b"\r\n", b"\n")
    record_path = info + "RECORD"
    files.pop(record_path)
    record = io.StringIO()
    writer = csv.writer(record, lineterminator="\n")
    for name, contents in sorted(files.items()):
        value = base64.urlsafe_b64encode(hashlib.sha256(contents).digest()).rstrip(b"=").decode("ascii")
        writer.writerow((name, "sha256=" + value, len(contents)))
    writer.writerow((record_path, "", ""))
    files[record_path] = record.getvalue().encode("utf-8")
    stamp = datetime.datetime.fromtimestamp(data["source_date_epoch"], datetime.UTC)
    # Stored members avoid cross-host zlib variation; Python code and license payloads stay byte-exact.
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_STORED) as output:
        for name, contents in sorted(files.items()):
            member = zipfile.ZipInfo(name, stamp.timetuple()[:6])
            member.create_system = 3
            member.external_attr = 0o100644 << 16
            output.writestr(member, contents)
