"""Validate the single provenance record shared by HIP profiler builds and qualification."""

from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path

LOCK = Path(__file__).resolve().with_name("hip-coverage-toolchain.json")
MAX_LOCK_BYTES = 4096


# Purpose: Reject ambiguous or nonfinite metadata before admitting compiler evidence.
# Inputs: Bounded JSON text. Outputs: One object; malformed reports raise without publication.
def report_object(text: str) -> dict:
    # Purpose: Retain unique fields. Inputs: JSON pairs. Outputs: Object or duplicate rejection.
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("Duplicate native profiling metadata field")
            result[key] = value
        return result

    # Purpose: Reject non-JSON constants. Inputs: Token. Outputs: Always raises.
    def reject_constant(_value):
        raise ValueError("Nonfinite native profiling metadata")

    # Purpose: Reject exponent overflow. Inputs: Number token. Outputs: Finite float or refusal.
    def finite_float(token):
        value = float(token)
        if not math.isfinite(value):
            raise ValueError("Nonfinite native profiling metadata")
        return value

    value = json.loads(text, object_pairs_hook=unique, parse_constant=reject_constant, parse_float=finite_float)
    if not isinstance(value, dict):
        raise ValueError("Native profiling metadata must be an object")
    return value


# Purpose: Validate exact public provenance and the supported profiler ABI without hardcoding a second revision pin.
# Inputs: At most four KiB of manifest bytes. Outputs: Validated record; unknown roles, types or sources fail closed.
def parse_toolchain(payload: bytes) -> dict:
    if not payload or len(payload) > MAX_LOCK_BYTES:
        raise ValueError("HIP coverage toolchain metadata exceeds its byte budget")
    value = report_object(payload.decode("utf-8"))
    expected = {
        "schema_version",
        "repository",
        "revision",
        "source_url",
        "llvm_major",
        "profile_record_bytes",
        "host",
        "device",
        "zlib",
    }
    if set(value) != expected:
        raise ValueError("Unknown HIP coverage provenance schema")
    revision = value["revision"]
    if not isinstance(revision, str) or re.fullmatch(r"[a-f0-9]{40}", revision) is None:
        raise ValueError("HIP coverage requires a complete immutable source revision")
    if (
        value["repository"] != "https://github.com/ROCm/llvm-project"
        or value["source_url"] != value["repository"] + "/commit/" + revision
    ):
        raise ValueError("HIP coverage provenance must identify the official ROCm LLVM commit")
    for field, expected_value in (("schema_version", 1), ("llvm_major", 24), ("profile_record_bytes", 72)):
        if type(value[field]) is not int or value[field] != expected_value:
            raise ValueError("Unsupported HIP coverage provenance version or profiling ABI")
    if value["host"] != "windows-x64" or value["device"] != "amdgpu-elf64le":
        raise ValueError("Unsupported HIP coverage profiling targets")
    dependency = value["zlib"]
    if (
        not isinstance(dependency, dict)
        or set(dependency) != {"version", "url", "bytes", "sha256"}
        or not isinstance(dependency["version"], str)
        or re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", dependency["version"]) is None
        or dependency["url"]
        != "https://github.com/madler/zlib/releases/download/v"
        + dependency["version"]
        + "/zlib-"
        + dependency["version"]
        + ".tar.gz"
        or type(dependency["bytes"]) is not int
        or not 0 < dependency["bytes"] <= 8 * 1024**2
        or not isinstance(dependency["sha256"], str)
        or re.fullmatch(r"[a-f0-9]{64}", dependency["sha256"]) is None
    ):
        raise ValueError("HIP coverage requires pinned official zlib source provenance")
    return value


# Purpose: Read bounded ordinary provenance bytes before following any selected input path.
# Inputs: Explicit manifest or the repository pin. Outputs: Validated metadata; redirected/missing files fail.
def read_toolchain(path: Path = LOCK) -> dict:
    for item in (path, *path.parents, path.absolute(), *path.absolute().parents):
        if item.is_symlink() or item.is_junction():
            raise ValueError("HIP coverage provenance crosses a linked path")
    with path.open("rb") as stream:
        return parse_toolchain(stream.read(MAX_LOCK_BYTES + 1))


# Purpose: Supply validated scalar metadata to CMake without evaluating manifest content as code.
# Inputs: Explicit manifest and named field. Outputs: One scalar or diagnostic; no installation or execution.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, default=LOCK)
    parser.add_argument("--field", choices=("revision", "llvm_major"), required=True)
    arguments = parser.parse_args()
    print(read_toolchain(arguments.lock)[arguments.field])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
