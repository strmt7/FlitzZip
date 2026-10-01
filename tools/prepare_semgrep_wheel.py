"""Build the reviewed Semgrep dependency-metadata revision; never edit an installation."""

import argparse
import base64
import csv
import hashlib
import io
import json
import os
import re
import stat
import tempfile
import urllib.request
import zipfile
from email import policy
from email.parser import BytesParser
from pathlib import Path, PurePosixPath
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / ".github/requirements/semgrep-packaging.json"
MAX_DOWNLOAD = 100 * 1024 * 1024
MAX_UNPACKED = 1024 * 1024 * 1024
MAX_MEMBER = 512 * 1024 * 1024
METADATA_POLICY = policy.default.clone(linesep="\n", max_line_length=0, refold_source="none")


# Purpose: Admit only an explicitly identified, bounded downstream packaging revision.
# Inputs: repository-controlled JSON manifest. Outputs: validated fields or a fail-closed exception.
def read_manifest(path: Path) -> dict:
    if path.stat().st_size > 16 * 1024:
        raise ValueError("oversized packaging manifest")
    manifest = json.loads(path.read_text(encoding="utf-8"))
    keys = {
        "schema",
        "upstream_version",
        "local_version",
        "filename",
        "url",
        "sha256",
        "original_requirement",
        "tested_requirement",
        "upstream_issue",
    }
    if not isinstance(manifest, dict) or set(manifest) != keys or type(manifest["schema"]) is not int:
        raise ValueError("invalid packaging manifest schema")
    if manifest["schema"] != 1 or any(not isinstance(manifest[key], str) for key in keys - {"schema"}):
        raise ValueError("unsupported packaging manifest values")
    url = urlsplit(manifest["url"])
    if (
        not re.fullmatch(r"\d+\.\d+\.\d+", manifest["upstream_version"])
        or not re.fullmatch(re.escape(manifest["upstream_version"]) + r"\+superzip\.\d+", manifest["local_version"])
        or not re.fullmatch(
            "semgrep-"
            + re.escape(manifest["upstream_version"])
            + r"-[A-Za-z0-9_.]+-[A-Za-z0-9_.]+-manylinux_[A-Za-z0-9_]+\.whl",
            manifest["filename"],
        )
        or not re.fullmatch(r"[a-f0-9]{64}", manifest["sha256"])
        or not re.fullmatch(r"pyjwt\[crypto\]~=\d+\.\d+\.\d+", manifest["original_requirement"])
        or not re.fullmatch(r"pyjwt\[crypto\]==\d+\.\d+\.\d+", manifest["tested_requirement"])
        or url.scheme != "https"
        or url.netloc != "files.pythonhosted.org"
        or not url.path.startswith("/packages/")
        or PurePosixPath(url.path).name != manifest["filename"]
        or url.query
        or url.fragment
    ):
        raise ValueError("invalid packaging identity or origin")
    return manifest


# Purpose: Hash one bounded regular input without loading it in memory.
# Inputs: path is an owned wheel file. Outputs: SHA-256 hex; fails for a link or oversize input.
def file_digest(path: Path) -> str:
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_UNPACKED:
        raise ValueError("wheel must be a bounded regular file")
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


# Purpose: Apply exactly the reviewed distribution version and dependency changes.
# Inputs: metadata bytes and trusted manifest. Outputs: new metadata; rejects unexpected upstream fields.
def revise_metadata(data: bytes, manifest: dict) -> bytes:
    original = BytesParser(policy=METADATA_POLICY).parsebytes(data)
    requirements = original.get_all("Requires-Dist", [])
    if (
        original.get_all("Name") != ["semgrep"]
        or original.get_all("Version") != [manifest["upstream_version"]]
        or requirements.count(manifest["original_requirement"]) != 1
        or sum(value.lower().startswith("pyjwt") for value in requirements) != 1
    ):
        raise ValueError("upstream metadata no longer matches the reviewed patch")
    revised = type(original)(policy=METADATA_POLICY)
    for name, value in original.raw_items():
        if name == "Version":
            value = manifest["local_version"]
        elif name == "Requires-Dist" and value == manifest["original_requirement"]:
            value = manifest["tested_requirement"]
        revised[name] = value
    revised.set_payload(original.get_payload(decode=True))
    return revised.as_bytes()


# Purpose: Validate wheel names and complete RECORD membership before copying any code.
# Inputs: opened upstream ZIP and its dist-info prefix. Outputs: member metadata and expected digests.
def wheel_inventory(archive: zipfile.ZipFile, prefix: str) -> tuple[list, dict]:
    members = archive.infolist()
    if not 1 <= len(members) <= 5000 or sum(x.file_size for x in members) > MAX_UNPACKED:
        raise ValueError("wheel exceeds entry or aggregate size bound")
    names = [member.filename for member in members]
    if len(names) != len(set(names)):
        raise ValueError("duplicate wheel member")
    for member in members:
        path = PurePosixPath(member.filename)
        mode = member.external_attr >> 16
        if (
            path.is_absolute()
            or any(part in ("", ".", "..") for part in member.filename.split("/"))
            or "\\" in member.filename
            or ":" in member.filename
            or member.is_dir()
            or member.file_size > MAX_MEMBER
            or stat.S_IFMT(mode) not in (0, stat.S_IFREG)
        ):
            raise ValueError("unsafe wheel member")
    record_name = prefix + "/RECORD"
    if any(name.endswith(("/RECORD.jws", "/RECORD.p7s")) for name in names):
        raise ValueError("signed wheel requires a separate signing review")
    if record_name not in names or archive.getinfo(record_name).file_size > 1024 * 1024:
        raise ValueError("missing or oversized wheel RECORD")
    records = {}
    for row in csv.reader(io.StringIO(archive.read(record_name).decode("utf-8"))):
        if len(row) != 3 or row[0] in records:
            raise ValueError("invalid or duplicate wheel RECORD row")
        records[row[0]] = (row[1], row[2])
    if set(records) != set(names) or records[record_name] != ("", ""):
        raise ValueError("wheel RECORD does not cover the complete archive")
    return sorted(members, key=lambda member: member.filename), records


# Purpose: Copy unchanged scanner code and notices with verified upstream RECORD hashes.
# Inputs: ZIP streams and one safe member. Outputs: destination hash and byte count; refuses corrupt content.
def copy_member(source, destination, member: zipfile.ZipInfo, name: str, expected: tuple) -> tuple:
    digest = hashlib.sha256()
    count = 0
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.external_attr = member.external_attr
    info.create_system = 3
    with source.open(member) as reader, destination.open(info, "w") as writer:
        while block := reader.read(1024 * 1024):
            count += len(block)
            if count > member.file_size:
                raise ValueError("wheel member exceeds declared length")
            digest.update(block)
            writer.write(block)
    encoded = "sha256=" + base64.urlsafe_b64encode(digest.digest()).rstrip(b"=").decode()
    if expected != (encoded, str(count)) or count != member.file_size:
        raise ValueError("upstream wheel RECORD integrity failure")
    return encoded, str(count)


# Purpose: Write generated metadata using host-independent ZIP storage and timestamps.
# Inputs: safe member name and bounded bytes. Outputs: wheel RECORD row for the generated content.
def write_member(destination, name: str, data: bytes) -> tuple:
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.external_attr = (stat.S_IFREG | 0o644) << 16
    info.create_system = 3
    destination.writestr(info, data)
    encoded = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
    return name, "sha256=" + encoded, str(len(data))


# Purpose: Produce a separately identified wheel, preserving every code and license byte.
# Inputs: hash-pinned upstream wheel, new output path and reviewed manifest. Outputs: derived SHA-256.
def build_wheel(upstream: Path, output: Path, manifest: dict) -> str:
    if file_digest(upstream) != manifest["sha256"]:
        raise ValueError("upstream wheel SHA-256 mismatch")
    prefix = f"semgrep-{manifest['upstream_version']}.dist-info"
    revised_prefix = f"semgrep-{manifest['local_version']}.dist-info"
    with zipfile.ZipFile(upstream) as source, zipfile.ZipFile(output, "x") as destination:
        members, records = wheel_inventory(source, prefix)
        rows = []
        for member in members:
            name = member.filename
            revised_name = revised_prefix + name[len(prefix) :] if name.startswith(prefix + "/") else name
            if name == prefix + "/RECORD":
                continue
            if name == prefix + "/METADATA":
                if member.file_size > 4 * 1024 * 1024:
                    raise ValueError("oversized wheel metadata")
                original = source.read(member)
                encoded = base64.urlsafe_b64encode(hashlib.sha256(original).digest()).rstrip(b"=").decode()
                if records[name] != ("sha256=" + encoded, str(len(original))):
                    raise ValueError("upstream metadata integrity failure")
                rows.append(write_member(destination, revised_name, revise_metadata(original, manifest)))
            else:
                rows.append((revised_name, *copy_member(source, destination, member, revised_name, records[name])))
        if not any(row[0] == revised_prefix + "/METADATA" for row in rows):
            raise ValueError("missing upstream metadata")
        record = io.StringIO(newline="")
        csv.writer(record, lineterminator="\n").writerows(rows + [(revised_prefix + "/RECORD", "", "")])
        write_member(destination, revised_prefix + "/RECORD", record.getvalue().encode("utf-8"))
    return file_digest(output)


class NoRedirects(urllib.request.HTTPRedirectHandler):
    # Purpose: Prevent the pinned artifact download from changing its origin.
    # Inputs: urllib redirect callback values. Outputs: refusal, never a followed redirect.
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise ValueError("upstream artifact redirect rejected")


# Purpose: Fetch the sole trusted PyPI artifact with timeout, size and hash admission.
# Inputs: reviewed manifest and new disposable destination. Outputs: downloaded bytes; rejects origin changes.
def download_upstream(manifest: dict, destination: Path) -> None:
    if not manifest["url"].startswith("https://files.pythonhosted.org/packages/"):
        raise ValueError("unexpected upstream download origin")
    opener = urllib.request.build_opener(NoRedirects())
    with opener.open(manifest["url"], timeout=60) as response, destination.open("xb") as writer:
        count = 0
        while block := response.read(1024 * 1024):
            count += len(block)
            if count > MAX_DOWNLOAD:
                raise ValueError("upstream artifact exceeds download bound")
            writer.write(block)


# Purpose: Build the same hash-lockable scanner distribution used locally and by CI.
# Inputs: output directory and optional already-downloaded upstream wheel. Outputs: immutable wheel and JSON identity.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "out/scanner-wheels")
    parser.add_argument("--upstream-wheel", type=Path)
    args = parser.parse_args()
    manifest = read_manifest(MANIFEST)
    output_dir = args.output_dir.absolute()
    for parent in (output_dir, *output_dir.parents):
        if parent.is_symlink() or (hasattr(parent, "is_junction") and parent.is_junction()):
            raise ValueError("linked output directory rejected")
    output_dir.mkdir(parents=True, exist_ok=True)
    filename = manifest["filename"].replace(
        "semgrep-" + manifest["upstream_version"] + "-", "semgrep-" + manifest["local_version"] + "-", 1
    )
    target = output_dir / filename
    with tempfile.TemporaryDirectory(prefix="semgrep-wheel-", dir=output_dir) as directory:
        staging = Path(directory)
        upstream = args.upstream_wheel
        if upstream is None:
            upstream = staging / manifest["filename"]
            download_upstream(manifest, upstream)
        derived = staging / filename
        digest = build_wheel(upstream, derived, manifest)
        if target.exists():
            if file_digest(target) != digest:
                raise ValueError("existing derived wheel differs; refusing overwrite")
        else:
            os.link(derived, target)
        print(
            json.dumps(
                {"package": "semgrep", "version": manifest["local_version"], "wheel": filename, "sha256": digest}
            )
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
