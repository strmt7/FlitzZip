"""Validate the canonical DevSkim package pin before downloading or installing."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

MANIFEST = Path(__file__).resolve().parents[1] / ".github" / "requirements" / "devskim-packaging.json"
MAX_MANIFEST_BYTES = 16384
MAX_PACKAGE_BYTES = 128 * 1024 * 1024


# Purpose: Reject ambiguous JSON fields instead of silently selecting the last value.
# Inputs: Object fields supplied by the standard JSON decoder.
# Outputs: Unique-key object; duplicated provenance fields raise ValueError.
def unique_fields(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("DevSkim manifest contains duplicate fields")
        result[key] = value
    return result


# Purpose: Admit only a bounded, unambiguous NuGet version, digest, and exact package size.
# Inputs: Canonical JSON pin; tests may supply a separate manifest path.
# Outputs: Validated metadata; invalid evidence fails before download or SDK execution.
def read_manifest(path: Path = MANIFEST) -> dict:
    with path.open("rb") as stream:
        payload = stream.read(MAX_MANIFEST_BYTES + 1)
    if len(payload) > MAX_MANIFEST_BYTES:
        raise ValueError("DevSkim manifest exceeds its size budget")
    pin = json.loads(payload, object_pairs_hook=unique_fields)
    if not isinstance(pin, dict) or set(pin) != {"schema_version", "version", "nupkg_bytes", "nupkg_sha256"}:
        raise ValueError("DevSkim manifest has an unsupported shape")
    if type(pin["schema_version"]) is not int or pin["schema_version"] != 1:
        raise ValueError("DevSkim manifest has an unsupported schema")
    if not isinstance(pin["version"], str) or not re.fullmatch(r"[0-9]{1,4}\.[0-9]{1,4}\.[0-9]{1,4}", pin["version"]):
        raise ValueError("DevSkim version is not a stable numeric NuGet version")
    if type(pin["nupkg_bytes"]) is not int or not 0 < pin["nupkg_bytes"] <= MAX_PACKAGE_BYTES:
        raise ValueError("DevSkim pinned package size is invalid")
    if not isinstance(pin["nupkg_sha256"], str) or not re.fullmatch(r"[0-9A-F]{64}", pin["nupkg_sha256"]):
        raise ValueError("DevSkim package digest is invalid")
    return pin


# Purpose: Derive download limits and identity from one already-validated provenance record.
# Inputs: Validated package pin from read_manifest.
# Outputs: Three safe GitHub environment-file assignments; no paths or credentials.
def environment_lines(pin: dict) -> str:
    return (
        f"DEVSKIM_VERSION={pin['version']}\n"
        f"DEVSKIM_NUPKG_BYTES={pin['nupkg_bytes']}\n"
        f"DEVSKIM_NUPKG_SHA256={pin['nupkg_sha256']}\n"
    )


# Purpose: Check the exact downloaded package, not a replacement copy resolved by NuGet.
# Inputs: Local package and validated size/digest; reads at most the pin plus one byte.
# Outputs: None on exact identity; truncation, growth, or digest changes raise ValueError.
def verify_package(path: Path, pin: dict) -> None:
    digest = hashlib.sha256()
    total = 0
    with path.open("rb") as stream:
        while chunk := stream.read(min(1024 * 1024, pin["nupkg_bytes"] - total + 1)):
            total += len(chunk)
            if total > pin["nupkg_bytes"]:
                raise ValueError("DevSkim package exceeds its pinned size")
            digest.update(chunk)
    if total != pin["nupkg_bytes"] or digest.hexdigest().upper() != pin["nupkg_sha256"]:
        raise ValueError("DevSkim package size or SHA-256 differs from provenance")


# Purpose: Supply CI's canonical download bounds or verify its downloaded artifact.
# Inputs: Exactly one CLI mode and the repository manifest; no package execution.
# Outputs: Safe environment assignments or a verified status; failures are nonzero.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--emit-env", action="store_true")
    mode.add_argument("--package", type=Path)
    args = parser.parse_args()
    pin = read_manifest()
    if args.emit_env:
        print(environment_lines(pin), end="")
    else:
        verify_package(args.package, pin)
        print("DevSkim package size and SHA-256 verified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
