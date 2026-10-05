"""Portable, content-based native build inputs; no host paths or Git acceptance claims."""

from __future__ import annotations

import argparse
import hashlib
import json
import stat
import subprocess
import threading
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
INPUT_ROOTS = ("src", "tests", "third_party", "cmake", "resources/app", "resources/brand", "resources/licenses")
INPUT_FILES = (
    "CMakeLists.txt",
    "LICENSE",
    "tools/build.ps1",
    "tools/compile_hip_object.ps1",
    "tools/hip_architecture.ps1",
    "tools/rocm_toolchain.ps1",
    "tools/rocm-sdk-lock.json",
    "tools/process_environment.ps1",
    "tools/cmake_toolchain.ps1",
    "tools/cmake-toolchain.sha256",
    "tools/build_parallelism.ps1",
    "tools/local_resources.ps1",
    "tools/version.ps1",
    "tools/generate_brand_logo_header.ps1",
    "tools/superzip_brand_logo.psm1",
    "tools/generate_license_notices_header.ps1",
    "tools/native_build_provenance.py",
    "tools/native_build_receipt.py",
)
MAX_INPUT_COUNT = 20000
MAX_INPUT_BYTES = 8 * 1024**3


# Purpose: Produce stable, path-free metadata bytes without nonfinite numbers.
# Inputs: JSON-compatible provenance metadata.
# Outputs: Canonical ASCII JSON or a serialization error.
def canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode("ascii")


# Purpose: Contain an input below the checkout and reject links at every component.
# Inputs: Checkout root and canonical repository-relative POSIX filename.
# Outputs: Existing regular file; rejects traversal, aliases, links and missing files.
def input_path(root: Path, name: str) -> Path:
    relative = PurePosixPath(name)
    if not name or relative.is_absolute() or relative.as_posix() != name or "\\" in name or ":" in name:
        raise ValueError("native input must be a canonical repository-relative path")
    if any(part in (".", "..", ".git") for part in relative.parts):
        raise ValueError("native input path contains a forbidden component")
    selected = root.resolve(strict=True)
    for part in relative.parts:
        selected /= part
        info = selected.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise ValueError("native input links are not accepted")
    if not selected.is_file() or selected.stat().st_size > MAX_INPUT_BYTES:
        raise ValueError("native input must be a bounded regular file")
    return selected


# Purpose: Hash bounded file contents without loading archives into memory.
# Inputs: A validated regular input file.
# Outputs: SHA-256 digest; rejects concurrent size or timestamp changes during reading.
def file_digest(path: Path) -> str:
    before = path.stat()
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError("native input changed while hashing")
    return digest.hexdigest()


# Purpose: Bound Git inventory output before buffering it and disable optional helper hooks for this invocation.
# Inputs: Checkout root; the fixed native input projection.
# Outputs: At most four MiB of inventory within thirty seconds; raises on command, output or timeout failure.
def git_inventory(root: Path) -> bytes:
    command = [
        "git",
        "-c",
        "core.fsmonitor=false",
        "-c",
        "core.untrackedCache=false",
        "-C",
        str(root),
        "ls-files",
        "-z",
        "--cached",
        "--others",
        "--exclude-standard",
        "--",
        *INPUT_ROOTS,
        *INPUT_FILES,
    ]
    expired = threading.Event()
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as process:
        # Purpose: End owned Git on deadline. Inputs: Owner/event. Outputs: Marks timeout and kills the process.
        def expire() -> None:
            expired.set()
            process.kill()

        timer = threading.Timer(30, expire)
        timer.start()
        output = bytearray()
        try:
            while block := process.stdout.read(8192):
                if len(output) + len(block) > 4 * 1024 * 1024:
                    raise ValueError("native input inventory exceeds its metadata bound")
                output.extend(block)
            code = process.wait(timeout=5)
            if expired.is_set():
                raise TimeoutError("native input inventory exceeded thirty seconds")
            if code:
                cause = output[-1000:].decode("utf-8", errors="replace")
                raise ValueError(f"Git native inventory failed ({code}): {cause}")
            return bytes(output)
        finally:
            timer.cancel()
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            timer.join()


# Purpose: Inventory actual native build files, including nonignored uncommitted additions.
# Inputs: Git checkout root; fixed native source/resource/tool projection.
# Outputs: Sorted unique relative filenames; bounded Git errors and invalid inventories fail closed.
def native_input_names(root: Path) -> list[str]:
    names = sorted(set(git_inventory(root).decode("utf-8", errors="strict").rstrip("\0").split("\0")))
    if not names or len(names) > MAX_INPUT_COUNT or "CMakeLists.txt" not in names:
        raise ValueError("native input inventory is incomplete or too large")
    return names


# Purpose: Bind a native source snapshot to content rather than a checkout commit.
# Inputs: Root of a Git checkout, including current staged and unstaged native inputs.
# Outputs: Portable file hashes and canonical manifest digest; documents and caches are excluded.
def capture_inputs(root: Path) -> dict:
    names = native_input_names(root)
    files = {name: file_digest(input_path(root, name)) for name in names}
    if native_input_names(root) != names:
        raise ValueError("native input inventory changed during capture")
    return {
        "schema_version": 1,
        "projection": "native-invocation-inputs-v1",
        "files": files,
        "inputs_sha256": hashlib.sha256(canonical(files)).hexdigest(),
    }


# Purpose: Expose the shared snapshot to native PowerShell build and benchmark tools.
# Inputs: Optional checkout root; no compiler execution or installed software mutation.
# Outputs: One JSON snapshot on stdout; failure returns an actionable exception.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    arguments = parser.parse_args()
    print(canonical(capture_inputs(arguments.root)).decode("ascii"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
