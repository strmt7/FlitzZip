"""Retain a complete, bounded LLVM export inside the caller's owned Windows job."""

from __future__ import annotations

import argparse
import ctypes
import subprocess
from pathlib import Path

from tools.bootstrap_rocm_sdk import reject_link_chain
from tools.native_coverage import ROOT, owned_path

MAX_EXPORT_BYTES = 8 * 1024**2


# Purpose: Require inherited lifetime and aggregate memory containment before launching the export descendant.
# Inputs: Current Windows process. Outputs: Refuses standalone invocation outside a job object.
def require_job() -> None:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetCurrentProcess.restype = ctypes.c_void_p
    kernel32.IsProcessInJob.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
    kernel32.IsProcessInJob.restype = ctypes.c_int
    contained = ctypes.c_int()
    if not kernel32.IsProcessInJob(kernel32.GetCurrentProcess(), None, ctypes.byref(contained)) or not contained.value:
        raise RuntimeError("LLVM export requires an inherited Windows job; use tools.hip_coverage")


# Purpose: Preserve native JSON without raising response-size limits or accepting unbounded artifact writes.
# Inputs: Existing LLVM tool, ELF and merged profile; unused output under this checkout.
# Outputs: At most eight MiB of native bytes; oversized or failed exports retain diagnostics and fail.
def capture(tool: Path, image: Path, profile: Path, output: Path) -> None:
    require_job()
    for path in (tool, image, profile):
        reject_link_chain(path)
        if not path.is_file():
            raise ValueError("Missing LLVM export input")
    output = owned_path(ROOT, output)
    command = [str(tool), "export", str(image), f"-instr-profile={profile}"]
    with (
        output.open("xb") as destination,
        subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS,
        ) as child,
    ):
        total = 0
        try:
            while chunk := child.stdout.read(65536):
                total += len(chunk)
                if total > MAX_EXPORT_BYTES:
                    raise ValueError("LLVM coverage export exceeds eight MiB")
                destination.write(chunk)
            if child.wait() != 0:
                raise RuntimeError("LLVM coverage export failed")
            if not total:
                raise ValueError("LLVM coverage export is empty")
        except BaseException:
            child.kill()
            raise


# Purpose: Expose only the fixed LLVM export operation to the owning bounded runner.
# Inputs: Four explicit paths. Outputs: Complete native artifact or nonzero process exit.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("tool", "image", "profile", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    arguments = parser.parse_args()
    capture(arguments.tool, arguments.image, arguments.profile, arguments.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
