"""Build the identified Semgrep packaging revision without changing scanner code."""

from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import io
import json
import platform
import re
import stat
import time
import urllib.request
import zipfile
from email.parser import BytesParser
from pathlib import Path, PurePosixPath
from urllib.parse import urlsplit

from tools.python_wheel import read_recipe

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / ".github/requirements/semgrep-distribution.json"
MAX_BYTES = 512 * 1024 * 1024


def sha256(data: bytes) -> str:
    """Purpose: Bind exact artifact bytes. Inputs: Complete bytes. Outputs: SHA-256 in hexadecimal."""
    return hashlib.sha256(data).hexdigest()


def recipe() -> dict:
    """Purpose: Load the narrow reviewed packaging operation. Inputs: Tracked manifest. Outputs: Validated pins."""
    data = read_recipe(MANIFEST)
    if (
        type(data.get("schema")) is not int
        or data["schema"] != 1
        or data.get("upstream_version") != "1.181.0"
        or data.get("version") != "1.181.0+flitzzip.mcp1"
        or data.get("mcp_before") != "1.29.0"
        or data.get("mcp_after") != "1.30.0"
        or set(data.get("wheels", {})) != {"linux", "windows"}
    ):
        raise ValueError("Unsupported Semgrep packaging recipe")
    for target, pin in data["wheels"].items():
        url = urlsplit(pin["url"])
        if (
            url.scheme != "https"
            or url.netloc != "files.pythonhosted.org"
            or url.query
            or url.fragment
            or Path(url.path).name != pin["filename"]
            or not pin["filename"].startswith(f"semgrep-{data['upstream_version']}-")
            or not pin["filename"].endswith("-manylinux_2_34_x86_64.whl" if target == "linux" else "-win_amd64.whl")
            or type(pin["bytes"]) is not int
            or not 0 < pin["bytes"] <= MAX_BYTES
            or any(re.fullmatch(r"[0-9a-f]{64}", pin[key]) is None for key in ("sha256", "derived_sha256"))
        ):
            raise ValueError("Invalid pinned Semgrep wheel")
    return data


class NoRedirect(urllib.request.HTTPRedirectHandler):
    """Reject transport/provenance changes while downloading a fixed official artifact."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        """Purpose: Refuse redirects. Inputs: Untrusted HTTP redirect. Outputs: Error before another request."""
        raise ValueError("Pinned Semgrep wheel downloads cannot redirect")


def download(pin: dict) -> bytes:
    """Purpose: Fetch one wheel within byte/time limits. Inputs: Validated pin. Outputs: Verified bytes."""
    deadline = time.monotonic() + 180
    contents = bytearray()
    request = urllib.request.Request(pin["url"], headers={"Accept-Encoding": "identity"})
    with urllib.request.build_opener(NoRedirect()).open(request, timeout=30) as response:
        if response.status != 200 or int(response.headers.get("Content-Length", "-1")) != pin["bytes"]:
            raise ValueError("Semgrep wheel HTTP status/length differs from its pin")
        while block := response.read(1024 * 1024):
            contents.extend(block)
            if len(contents) > pin["bytes"] or time.monotonic() > deadline:
                raise ValueError("Semgrep wheel download exceeded its byte or lifetime limit")
    if time.monotonic() > deadline:
        raise ValueError("Semgrep wheel download exceeded its lifetime boundary at EOF")
    if len(contents) != pin["bytes"] or sha256(contents) != pin["sha256"]:
        raise ValueError("Semgrep wheel bytes differ from the official pin")
    return bytes(contents)


def read_wheel(contents: bytes) -> dict[str, tuple[bytes, int]]:
    """Purpose: Admit a bounded standard wheel without extraction. Inputs: ZIP bytes. Outputs: Safe unique file map."""
    with zipfile.ZipFile(io.BytesIO(contents)) as archive:
        entries = archive.infolist()
        if len(entries) > 4096 or sum(item.file_size for item in entries) > MAX_BYTES:
            raise ValueError("Semgrep wheel exceeds the reviewed extent")
        files = {}
        for item in entries:
            name = item.filename
            path = PurePosixPath(name)
            mode = item.external_attr >> 16
            if (
                not name
                or len(name) > 2048
                or path.is_absolute()
                or ".." in path.parts
                or any(value in name for value in ("\\", ":", "\0"))
                or name != path.as_posix()
                or item.orig_filename != name
                or name in files
                or item.is_dir()
                or (mode and stat.S_IFMT(mode) not in (0, stat.S_IFREG))
                or item.flag_bits & 1
            ):
                raise ValueError("Unsafe or duplicate Semgrep wheel member")
            files[name] = (archive.read(item), 0o100755 if mode & 0o111 else 0o100644)
    return files


def record_digest(contents: bytes) -> str:
    """Purpose: Encode a standard wheel integrity value. Inputs: Member bytes. Outputs: URL-safe SHA-256 token."""
    return "sha256=" + base64.urlsafe_b64encode(hashlib.sha256(contents).digest()).rstrip(b"=").decode("ascii")


def validate_record(files: dict[str, tuple[bytes, int]], record_name: str) -> None:
    """Purpose: Reject incomplete or invalid wheel records. Inputs: File map. Outputs: Exact integrity check."""
    rows = list(csv.reader(io.StringIO(files[record_name][0].decode("utf-8"))))
    if len(rows) != len(files) or any(len(row) != 3 for row in rows):
        raise ValueError("Semgrep wheel RECORD does not cover the file set")
    seen = set()
    for name, digest, size in rows:
        if name not in files or name in seen:
            raise ValueError("Semgrep wheel RECORD has an unknown or duplicate member")
        seen.add(name)
        contents = files[name][0]
        expected = ("", "") if name == record_name else (record_digest(contents), str(len(contents)))
        if (digest, size) != expected:
            raise ValueError("Semgrep wheel RECORD integrity mismatch")


def derive_wheel(contents: bytes, data: dict) -> bytes:
    """Purpose: Change only the distribution identity and MCP requirement. Inputs: Original wheel and recipe.
    Outputs: Reproducible wheel; scanner code, executables, other requirements and notices remain byte-exact.
    """
    files = read_wheel(contents)
    old_info = f"semgrep-{data['upstream_version']}.dist-info/"
    new_info = f"semgrep-{data['version']}.dist-info/"
    validate_record(files, old_info + "RECORD")
    metadata = files[old_info + "METADATA"][0]
    parsed = BytesParser().parsebytes(metadata)
    if parsed.get_all("Name") != ["semgrep"] or parsed.get_all("Version") != [data["upstream_version"]]:
        raise ValueError("Semgrep wheel metadata differs from its identity")
    newline = "\r\n" if b"\r\n" in metadata else "\n"
    for before, after in (
        (f"Version: {data['upstream_version']}{newline}", f"Version: {data['version']}{newline}"),
        (f"Requires-Dist: mcp=={data['mcp_before']}{newline}", f"Requires-Dist: mcp=={data['mcp_after']}{newline}"),
    ):
        if metadata.count(before.encode()) != 1:
            raise ValueError("Semgrep metadata patch does not match exactly once")
        metadata = metadata.replace(before.encode(), after.encode())
    del files[old_info + "RECORD"]
    files[old_info + "METADATA"] = (metadata, files[old_info + "METADATA"][1])
    files = {
        name.replace(old_info, new_info, 1) if name.startswith(old_info) else name: value
        for name, value in files.items()
    }
    record_name = new_info + "RECORD"
    record = io.StringIO()
    writer = csv.writer(record, lineterminator="\n")
    for name, (payload, _) in sorted(files.items()):
        writer.writerow((name, record_digest(payload), len(payload)))
    writer.writerow((record_name, "", ""))
    files[record_name] = (record.getvalue().encode("utf-8"), 0o100644)
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_STORED) as archive:
        for name, (payload, mode) in sorted(files.items()):
            item = zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0))
            item.create_system = 3
            item.external_attr = mode << 16
            archive.writestr(item, payload)
    return output.getvalue()


def build(output: Path, target: str) -> Path:
    """Purpose: Publish one exact derived wheel in a fresh owned directory. Inputs: Target and output path.
    Outputs: Hash-verified wheel and source/output receipt; refuses replacement and unsupported platforms.
    """
    data = recipe()
    pin = data["wheels"][target]
    output.mkdir(parents=True, exist_ok=False)
    contents = derive_wheel(download(pin), data)
    if sha256(contents) != pin["derived_sha256"]:
        raise ValueError("Derived Semgrep wheel differs from the reviewed reproducible output")
    filename = pin["filename"].replace(f"semgrep-{data['upstream_version']}-", f"semgrep-{data['version']}-", 1)
    wheel = output / filename
    with wheel.open("xb") as stream:
        stream.write(contents)
    (output / "receipt.json").write_text(
        json.dumps(
            {
                "manifest_sha256": sha256(MANIFEST.read_bytes()),
                "target": target,
                "source": pin,
                "output": filename,
                "sha256": sha256(contents),
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    return wheel


def main() -> int:
    """Purpose: Build only a supported scanner distribution. Inputs: CLI output/target. Outputs: Wheel path or error."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--target", choices=("linux", "windows"))
    args = parser.parse_args()
    target = args.target or {"Linux": "linux", "Windows": "windows"}.get(platform.system())
    if target is None or (args.target is None and platform.machine().lower() not in ("amd64", "x86_64")):
        parser.error("The pinned Semgrep distribution requires Windows or Linux x64")
    print(build(args.output, target))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
