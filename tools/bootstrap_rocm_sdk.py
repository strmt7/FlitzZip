"""Provision one complete, immutable ROCm distribution for local and hosted builds."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import sys
import tarfile
import time
import urllib.request
import uuid
from pathlib import Path, PurePosixPath
from urllib.parse import urlsplit

REPO = Path(__file__).resolve().parent.parent
LOCK_PATH = REPO / "tools" / "rocm-sdk-lock.json"
CHUNK = 1024 * 1024
MAX_FILE = 8 * 1024**3
MAX_MEMBERS = 200_000
MAX_UNPACKED = 80 * 1024**3


def hash_file(path: Path) -> str:
    """Purpose: Hash immutable archive bytes with bounded RAM.
    Inputs: Path is a regular file; its ownership remains with the caller.
    Outputs: Returns hexadecimal SHA-256 or propagates file access errors.
    """
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def reject_link_chain(path: Path) -> None:
    """Purpose: Reject redirected provisioning destinations and input paths.
    Inputs: Path may contain existing and not-yet-created components.
    Outputs: Throws for symlinks or junctions; does not modify the filesystem.
    """
    for entry in (path, *path.parents):
        if entry.is_symlink() or entry.is_junction():
            raise ValueError("ROCm provisioning refuses linked path components")


def validate_member(member: tarfile.TarInfo, seen: set[str]) -> PurePosixPath:
    """Purpose: Admit only collision-free, regular Windows SDK files and directories.
    Inputs: Member is untrusted archive metadata; seen stores case-insensitive destination names.
    Outputs: Returns a safe relative path and reserves it; throws before extraction on invalid metadata.
    """
    name = member.name
    if not (member.isfile() or member.isdir()) or member.size < 0 or member.size > MAX_FILE:
        raise ValueError("ROCm archive contains a link, special entry or oversized member")
    if re.search(r'[\\:"<>|?*]', name) or any(ord(c) < 32 for c in name):
        raise ValueError("ROCm archive contains an unsafe Windows path")
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts:
        raise ValueError("ROCm archive attempts path traversal")
    if not path.parts and not member.isdir():
        raise ValueError("ROCm archive root must be a directory")
    for part in path.parts:
        stem = part.split(".", 1)[0].upper()
        if part.endswith((".", " ")) or re.fullmatch(r"CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9]|CONIN\$|CONOUT\$", stem):
            raise ValueError("ROCm archive contains a reserved Windows name")
    key = str(path).casefold()
    if key in seen:
        raise ValueError("ROCm archive contains a case-insensitive destination collision")
    seen.add(key)
    return path


def inspect_archive(archive: Path, lock: dict, existing: Path | None = None) -> dict:
    """Purpose: Verify the pinned whole SDK and every destination before native extraction or adoption.
    Inputs: Archive and lock define exact distribution bytes; existing optionally requires byte-exact installed content.
    Outputs: Returns inventory; rejects unsafe metadata, bounded-size violations or changed existing files.
    """
    reject_link_chain(archive)
    if archive.stat().st_size != lock["archive_bytes"] or hash_file(archive) != lock["sha256"]:
        raise ValueError("ROCm distribution archive size or SHA-256 mismatch")
    seen: set[str] = set()
    count = total = 0
    deadline = time.monotonic() + 900
    with tarfile.open(archive, "r|gz") as stream:
        for member in stream:
            path = validate_member(member, seen)
            count += 1
            total += member.size
            if count > MAX_MEMBERS or total > MAX_UNPACKED or time.monotonic() > deadline:
                raise ValueError("ROCm archive inventory exceeds its resource or lifetime bounds")
            if existing is not None:
                target = existing.joinpath(*path.parts)
                reject_link_chain(target)
                if member.isdir():
                    if not target.is_dir():
                        raise ValueError("Existing ROCm SDK is missing a directory")
                else:
                    if not target.is_file() or target.stat().st_size != member.size:
                        raise ValueError("Existing ROCm SDK contains missing or changed files")
                    content = stream.extractfile(member)
                    if content is None or hashlib.file_digest(content, "sha256").hexdigest() != hash_file(target):
                        raise ValueError("Existing ROCm SDK bytes differ from the pinned complete distribution")
    if count != lock["members"] or total != lock["unpacked_bytes"]:
        raise ValueError("ROCm archive inventory differs from its pinned distribution")
    return {"members": count, "unpacked_bytes": total, "sha256": lock["sha256"]}


def sdk_identity(sdk: Path, version: str) -> dict:
    """Purpose: Record critical build inputs without confusing distribution and component versions.
    Inputs: Sdk contains the complete distribution; version is the whole-SDK lock value.
    Outputs: Returns critical file sizes/hashes; rejects missing, linked or wrongly versioned build inputs.
    """
    files = (
        "share/therock/therock_manifest.json",
        "bin/hipcc.exe",
        "lib/amdhip64.lib",
        "include/hip/hip_version.h",
        "lib/llvm/bin/clang.exe",
        "lib/llvm/amdgcn/bitcode/ocml.bc",
    )
    identity = {}
    for relative in files:
        path = sdk / relative
        reject_link_chain(path)
        identity[relative] = {"bytes": path.stat().st_size, "sha256": hash_file(path)}
    manifest = sdk / files[0]
    if manifest.stat().st_size > CHUNK:
        raise ValueError("ROCm SDK manifest exceeds its metadata bound")
    if json.loads(manifest.read_text(encoding="utf-8")).get("rocm_version") != version:
        raise ValueError("Extracted ROCm distribution version differs from the whole-SDK pin")
    return identity


class NoRedirect(urllib.request.HTTPRedirectHandler):
    """Reject download redirects rather than silently changing provenance or transport."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        """Purpose: Refuse redirects for the pinned distribution URL.
        Inputs: Standard urllib redirect metadata is untrusted and never followed.
        Outputs: Raises an error before any request to the redirect destination.
        """
        raise ValueError("ROCm distribution download redirects are not accepted")


def download_archive(lock: dict, destination: Path) -> None:
    """Purpose: Download the pinned whole distribution over HTTPS with bounded memory and lifetime.
    Inputs: Lock supplies public provenance and exact expected bytes; destination must not already exist.
    Outputs: Publishes verified archive bytes without overwrite; preserves failed partials for diagnosis.
    """
    url = urlsplit(lock["url"])
    if url.scheme != "https" or not url.hostname or url.username or url.password or url.fragment:
        raise ValueError("ROCm distribution URL must be HTTPS without embedded credentials or fragments")
    reject_link_chain(destination)
    if destination.exists():
        raise ValueError("ROCm distribution download refuses to overwrite an existing archive")
    partial = destination.with_name(destination.name + "." + uuid.uuid4().hex + ".partial")
    opener = urllib.request.build_opener(NoRedirect())
    deadline = time.monotonic() + 1200
    total = 0
    request = urllib.request.Request(lock["url"], headers={"Accept-Encoding": "identity"})
    with opener.open(request, timeout=60) as response:
        if int(response.headers.get("Content-Length", "-1")) != lock["archive_bytes"]:
            raise ValueError("ROCm distribution HTTP length differs from the pin")
        with partial.open("xb") as output:
            while block := response.read(CHUNK):
                total += len(block)
                if total > lock["archive_bytes"] or time.monotonic() > deadline:
                    raise ValueError("ROCm distribution download exceeds its byte or lifetime bounds")
                output.write(block)
    if total != lock["archive_bytes"] or hash_file(partial) != lock["sha256"]:
        raise ValueError("ROCm distribution download size or SHA-256 mismatch")
    partial.rename(destination)


def bounded_extract(archive: Path, destination: Path) -> None:
    """Purpose: Extract admitted distribution bytes through the shared Windows process containment implementation.
    Inputs: Archive has passed complete inventory; destination is a fresh owned staging directory.
    Outputs: Populates the SDK or raises with native status; time, output and aggregate RAM are bounded.
    """
    spec = importlib.util.spec_from_file_location("superzip_rocm_bounds", REPO / "mcp" / "superzip_mcp.py")
    if spec is None or spec.loader is None:
        raise RuntimeError("Shared Windows process containment is unavailable")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    result = module.run_bounded_command(
        ["tar.exe", "-xzf", str(archive), "-C", str(destination)], timeout_seconds=900, response_tail_bytes=4000
    )
    if result["exit_code"] != 0 or result["timed_out"] or result["output_limit_exceeded"]:
        raise RuntimeError(f"ROCm SDK extraction failed: {result}")


def provision(lock: dict, parent: Path, archive: Path | None = None) -> Path:
    """Purpose: Publish one complete SDK or byte-validate an existing manual installation without component updates.
    Inputs: Lock is repository-owned provenance; parent is caller-selected; archive optionally avoids another download.
    Outputs: Returns the validated SDK directory and writes a local receipt; never overwrites an existing SDK.
    """
    parent = parent.absolute()
    reject_link_chain(parent)
    parent.mkdir(parents=True, exist_ok=True)
    sdk = parent / "sdk"
    receipt = parent / "verified-distribution.json"
    if receipt.exists():
        reject_link_chain(receipt)
        record = json.loads(receipt.read_text(encoding="utf-8"))
        if record.get("sha256") != lock["sha256"] or not sdk.is_dir():
            raise ValueError("Existing ROCm provisioning receipt does not match the distribution pin")
        reject_link_chain(sdk)
        if record.get("critical_inputs") != sdk_identity(sdk, lock["version"]):
            raise ValueError("Cached ROCm SDK critical build inputs changed; preserve it and provision a fresh parent")
        return sdk
    source = archive.absolute() if archive is not None else parent / lock["archive_name"]
    if not source.exists():
        if archive is not None:
            raise ValueError("The explicitly supplied ROCm archive is missing")
        needed = lock["archive_bytes"] + lock["unpacked_bytes"] + 2 * 1024**3
        if shutil.disk_usage(parent).free < needed:
            raise ValueError("Insufficient disk headroom for complete ROCm distribution provisioning")
        download_archive(lock, source)
    inventory = inspect_archive(source, lock, existing=sdk if sdk.exists() else None)
    if not sdk.exists():
        if shutil.disk_usage(parent).free < lock["unpacked_bytes"] + 2 * 1024**3:
            raise ValueError("Insufficient disk headroom for the complete ROCm SDK")
        stage = parent / ("stage-" + uuid.uuid4().hex)
        stage.mkdir()
        bounded_extract(source, stage)
        sdk_identity(stage, lock["version"])
        stage.rename(sdk)
    identity = sdk_identity(sdk, lock["version"])
    with receipt.open("x", encoding="utf-8") as output:
        json.dump(
            {
                **inventory,
                "version": lock["version"],
                "url": lock["url"],
                "digest_origin": lock["digest_origin"],
                "critical_inputs": identity,
            },
            output,
            indent=2,
        )
    return sdk


def main() -> int:
    """Purpose: Provision the same repository-pinned native Windows SDK for agents, developers and hosted releases.
    Inputs: CLI accepts an archive and custom cache parent; defaults derive from the checkout and shared lock.
    Outputs: Prints the SDK root and returns zero on success; reports failures without changing host settings.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--parent", type=Path)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("SuperZip ROCm provisioning requires native Windows; do not use WSL")
    lock = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
    parent = args.parent or REPO / "out" / f"rocm-core-sdk-{lock['version']}"
    try:
        print(provision(lock, parent, args.archive))
    except (OSError, ValueError, RuntimeError, tarfile.TarError) as error:
        print(f"ROCm SDK provisioning failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
