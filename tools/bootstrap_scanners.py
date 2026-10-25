"""Provision the repository's pinned Windows scanners into an ignored local cache."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import tempfile
import urllib.request
import uuid
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path
from urllib.parse import urlsplit

from tools.devskim_provenance import read_manifest as read_devskim_pin
from tools.devskim_provenance import verify_package as verify_devskim_package

ROOT = Path(__file__).resolve().parents[1]
GITLEAKS_PIN = ROOT / ".github/requirements/gitleaks-packaging.json"
MAX_PIN_BYTES = 16384
MAX_DOWNLOAD_BYTES = 128 * 1024 * 1024


# Purpose: Reject ambiguous JSON properties in pinned scanner metadata.
# Inputs: Ordered key/value pairs from the standard JSON decoder. Outputs: Unique-key mapping or an error.
def unique_fields(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Scanner pin contains duplicate fields")
        result[key] = value
    return result


# Purpose: Reject ambiguous or malformed Gitleaks provenance before any network or process operation.
# Inputs: Repository lock file. Outputs: Exact version and archive/binary identities.
def read_gitleaks_pin(path: Path = GITLEAKS_PIN) -> dict:
    with path.open("rb") as stream:
        payload = stream.read(MAX_PIN_BYTES + 1)
    if len(payload) > MAX_PIN_BYTES:
        raise ValueError("Gitleaks pin exceeds its size budget")
    pin = json.loads(payload, object_pairs_hook=unique_fields)
    required = {"schema_version", "version", "archive_bytes", "archive_sha256", "binary_bytes", "binary_sha256"}
    if (
        not isinstance(pin, dict)
        or set(pin) != required
        or type(pin["schema_version"]) is not int
        or pin["schema_version"] != 1
    ):
        raise ValueError("Gitleaks pin has an unsupported shape")
    if not isinstance(pin["version"], str) or not re.fullmatch(r"[0-9]{1,4}\.[0-9]{1,4}\.[0-9]{1,4}", pin["version"]):
        raise ValueError("Gitleaks version is invalid")
    for key in ("archive_bytes", "binary_bytes"):
        if type(pin[key]) is not int or not 0 < pin[key] <= MAX_DOWNLOAD_BYTES:
            raise ValueError("Gitleaks size pin is invalid")
    for key in ("archive_sha256", "binary_sha256"):
        if not isinstance(pin[key], str) or not re.fullmatch(r"[0-9A-F]{64}", pin[key]):
            raise ValueError("Gitleaks digest pin is invalid")
    workflow = (ROOT / ".github/workflows/security-code-scanning.yml").read_text(encoding="utf-8")
    versions = set(re.findall(r"ghcr\.io/gitleaks/gitleaks:v([0-9.]+)@sha256:", workflow))
    if versions != {pin["version"]}:
        raise ValueError("Gitleaks package pin differs from the hosted scanner")
    return pin


# Purpose: Confirm one cached/downloaded file has the pinned complete size and digest.
# Inputs: Local regular file, exact byte count and SHA-256. Outputs: Boolean without loading the file into memory.
def matches_file(path: Path, size: int, digest: str) -> bool:
    if path.is_symlink() or not path.is_file() or path.stat().st_size != size:
        return False
    found = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            found.update(chunk)
    return found.hexdigest().upper() == digest


# Purpose: Download one HTTPS release asset with a hard byte cap and verify its exact pinned identity.
# Inputs: Repository-constructed URL, owned destination, expected size/digest. Outputs: Verified file or failure.
def download_verified(url: str, destination: Path, size: int, digest: str) -> None:
    parsed = urlsplit(url)
    if (
        parsed.scheme != "https"
        or parsed.hostname not in {"github.com", "api.nuget.org"}
        or parsed.username is not None
        or parsed.password is not None
        or parsed.port is not None
        or parsed.query
        or parsed.fragment
        or size > MAX_DOWNLOAD_BYTES
    ):
        raise ValueError("Scanner download URL or byte budget is invalid")
    request = urllib.request.Request(url, headers={"User-Agent": "FlitzZip-scanner-bootstrap"})
    found = hashlib.sha256()
    total = 0
    with urllib.request.urlopen(request, timeout=60) as response, destination.open("xb") as output:
        while chunk := response.read(min(1024 * 1024, size - total + 1)):
            total += len(chunk)
            if total > size:
                raise ValueError("Scanner download exceeds its pinned size")
            found.update(chunk)
            output.write(chunk)
    if total != size or found.hexdigest().upper() != digest:
        raise ValueError("Scanner download differs from pinned provenance")


# Purpose: Keep installer writes inside an ordinary repository-owned ignored cache.
# Inputs: Checkout root. Outputs: Existing or newly created scanner cache; reparse redirects are rejected.
def scanner_cache(root: Path = ROOT) -> Path:
    current = root.resolve(strict=True)
    for name in ("out", "tools", "scanners"):
        current /= name
        if current.is_symlink() or current.is_junction():
            raise ValueError("Scanner cache crosses a reparse point")
        current.mkdir(exist_ok=True)
    return current


# Purpose: Extract only the pinned Gitleaks executable without trusting ZIP member paths or archive metadata.
# Inputs: Verified release ZIP, owned destination and binary pin. Outputs: Exact executable bytes.
def extract_gitleaks(archive: Path, destination: Path, pin: dict) -> None:
    with zipfile.ZipFile(archive) as package:
        members = [item for item in package.infolist() if item.filename == "gitleaks.exe"]
        if len(members) != 1 or members[0].file_size != pin["binary_bytes"]:
            raise ValueError("Gitleaks archive lacks the pinned executable")
        found = hashlib.sha256()
        total = 0
        with package.open(members[0]) as source, destination.open("xb") as output:
            while chunk := source.read(1024 * 1024):
                total += len(chunk)
                if total > pin["binary_bytes"]:
                    raise ValueError("Gitleaks executable exceeds its pinned size")
                found.update(chunk)
                output.write(chunk)
    if total != pin["binary_bytes"] or found.hexdigest().upper() != pin["binary_sha256"]:
        raise ValueError("Gitleaks executable differs from pinned provenance")


# Purpose: Reuse an exact cached Gitleaks binary or install the verified release asset atomically.
# Inputs: Owned cache and repository pin. Outputs: Complete executable path.
def provision_gitleaks(cache: Path, pin: dict) -> Path:
    executable = cache / f"gitleaks-{pin['version']}.exe"
    if matches_file(executable, pin["binary_bytes"], pin["binary_sha256"]):
        return executable
    with tempfile.TemporaryDirectory(prefix="gitleaks-", dir=cache) as temporary:
        staging = Path(temporary)
        archive = staging / "release.zip"
        candidate = staging / "gitleaks.exe"
        name = f"gitleaks_{pin['version']}_windows_x64.zip"
        url = f"https://github.com/gitleaks/gitleaks/releases/download/v{pin['version']}/{name}"
        download_verified(url, archive, pin["archive_bytes"], pin["archive_sha256"])
        extract_gitleaks(archive, candidate, pin)
        os.replace(candidate, executable)
    return executable


# Purpose: Locate an SDK-capable .NET host without relying on a particular user's PATH or drive letter.
# Inputs: Windows process environment. Outputs: SDK host, or an actionable prerequisite failure.
def find_dotnet() -> str:
    candidates = []
    if program_files := os.environ.get("PROGRAMFILES"):
        candidates.append(str(Path(program_files) / "dotnet/dotnet.exe"))
    if command := shutil.which("dotnet"):
        candidates.append(command)
    for candidate in dict.fromkeys(candidates):
        if not Path(candidate).is_file():
            continue
        result = subprocess.run([candidate, "--list-sdks"], capture_output=True, text=True, timeout=20, check=False)
        if result.returncode == 0 and result.stdout.strip():
            return candidate
    raise ValueError("A .NET SDK is required to install pinned DevSkim; install the supported SDK first")


# Purpose: Accept only the immutable, direct-child installation named by a bounded cache receipt.
# Inputs: Cache and exact package pin. Outputs: Verified executable path or None when reprovisioning is required.
def cached_devskim(cache: Path, pin: dict) -> Path | None:
    marker = cache / "devskim-active.json"
    if marker.is_symlink() or not marker.is_file() or marker.stat().st_size > MAX_PIN_BYTES:
        return None
    try:
        receipt = json.loads(marker.read_bytes())
    except (OSError, ValueError):
        return None
    if not isinstance(receipt, dict) or set(receipt) != {"folder", "version", "package_sha256", "binary_sha256"}:
        return None
    folder = receipt["folder"]
    if not isinstance(folder, str) or not re.fullmatch(r"devskim-[0-9a-f]{32}", folder):
        return None
    if receipt["version"] != pin["version"] or receipt["package_sha256"] != pin["nupkg_sha256"]:
        return None
    if not isinstance(receipt["binary_sha256"], str) or not re.fullmatch(r"[0-9A-F]{64}", receipt["binary_sha256"]):
        return None
    directory = cache / folder
    executable = directory / "devskim.exe"
    if directory.is_symlink() or directory.is_junction() or executable.is_symlink() or not executable.is_file():
        return None
    digest = hashlib.sha256(executable.read_bytes()).hexdigest().upper()
    return executable if digest == receipt["binary_sha256"] else None


# Purpose: Install only the verified local NuGet package into an isolated .NET tool directory.
# Inputs: SDK host, owned staging root and exact package pin. Outputs: Installed DevSkim executable.
def install_verified_devskim(dotnet: str, staging: Path, pin: dict) -> Path:
    feed = staging / "feed"
    feed.mkdir()
    package = feed / f"microsoft.cst.devskim.cli.{pin['version']}.nupkg"
    url = f"https://api.nuget.org/v3-flatcontainer/microsoft.cst.devskim.cli/{pin['version']}/{package.name}"
    download_verified(url, package, pin["nupkg_bytes"], pin["nupkg_sha256"])
    verify_devskim_package(package, pin)
    config = ET.Element("configuration")
    ET.SubElement(ET.SubElement(config, "packageSources"), "clear")
    ET.SubElement(config.find("packageSources"), "add", key="verified", value=str(feed))
    ET.SubElement(ET.SubElement(config, "fallbackPackageFolders"), "clear")
    ET.ElementTree(config).write(feed / "NuGet.Config", encoding="utf-8", xml_declaration=True)
    environment = dict(os.environ)
    environment.update(
        {
            "NUGET_PACKAGES": str(staging / "packages"),
            "DOTNET_CLI_HOME": str(staging / "home"),
            "DOTNET_CLI_TELEMETRY_OPTOUT": "1",
            "DOTNET_NOLOGO": "1",
        }
    )
    destination = staging / "install"
    result = subprocess.run(
        [
            dotnet,
            "tool",
            "install",
            "--tool-path",
            str(destination),
            "--configfile",
            str(feed / "NuGet.Config"),
            "Microsoft.CST.DevSkim.CLI",
            "--version",
            pin["version"],
        ],
        capture_output=True,
        text=True,
        timeout=300,
        check=False,
        env=environment,
    )
    executable = destination / "devskim.exe"
    if result.returncode != 0 or not executable.is_file():
        raise ValueError("Verified DevSkim package installation failed: " + (result.stderr or result.stdout)[-1200:])
    version = subprocess.run([str(executable), "--version"], capture_output=True, text=True, timeout=20, check=False)
    if (
        version.returncode != 0
        or re.search(r"(?<![0-9.])" + re.escape(pin["version"]) + r"(?![0-9.])", version.stdout + version.stderr)
        is None
    ):
        raise ValueError("Installed DevSkim does not report the pinned version")
    return executable


# Purpose: Reuse a verified DevSkim installation or publish a freshly installed immutable cache directory.
# Inputs: Owned cache and canonical NuGet pin. Outputs: Executable path with an atomic active receipt.
def provision_devskim(cache: Path, pin: dict) -> Path:
    if current := cached_devskim(cache, pin):
        return current
    dotnet = find_dotnet()
    stage = cache / f"staging-{uuid.uuid4().hex}"
    stage.mkdir()
    try:
        executable = install_verified_devskim(dotnet, stage, pin)
        folder = f"devskim-{uuid.uuid4().hex}"
        os.replace(executable.parent, cache / folder)
        installed = cache / folder / "devskim.exe"
        receipt = {
            "folder": folder,
            "version": pin["version"],
            "package_sha256": pin["nupkg_sha256"],
            "binary_sha256": hashlib.sha256(installed.read_bytes()).hexdigest().upper(),
        }
        marker = cache / f"active-{uuid.uuid4().hex}.json"
        marker.write_text(json.dumps(receipt, sort_keys=True), encoding="utf-8")
        os.replace(marker, cache / "devskim-active.json")
        return installed
    finally:
        if stage.resolve().parent != cache.resolve() or stage.is_symlink() or stage.is_junction():
            raise ValueError("Scanner staging cleanup escaped its owned cache")
        shutil.rmtree(stage)


# Purpose: Fill only absent scanner paths with exact repository-pinned Windows binaries.
# Inputs: Explicit existing paths or None, and optional owned cache for offline tests.
# Outputs: Both executable paths; explicit caller selections remain subject to the preflight version gate.
def provision_missing_scanners(gitleaks: str | None, devskim: str | None, cache: Path | None = None) -> tuple[str, str]:
    if gitleaks and devskim:
        return gitleaks, devskim
    if platform.system() != "Windows":
        raise ValueError("Local scanner provisioning requires the supported Windows development host")
    owned = scanner_cache() if cache is None else cache
    if not gitleaks:
        gitleaks = str(provision_gitleaks(owned, read_gitleaks_pin()))
    if not devskim:
        devskim = str(provision_devskim(owned, read_devskim_pin()))
    return gitleaks, devskim
