"""Select whole-database C++ analysis without rebuilding unchanged native inputs."""

import argparse
import json
import re
import subprocess
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path, PurePosixPath

from tools.native_build_provenance import INPUT_FILES, INPUT_ROOTS

ROOT = Path(__file__).resolve().parents[1]
MAX_PATH_BYTES = 4 * 1024 * 1024
POLICY_FILES = {
    ".github/workflows/security-code-scanning.yml",
    "tools/cpp_security_plan.py",
    "tools/test_cpp_security_plan.py",
    ".gitattributes",
    ".gitmodules",
}
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx", ".ipp", ".inl", ".rc", ".cmake"}


# Purpose: Capture one owned Git stream without allocating an unlimited path inventory.
# Inputs: Binary pipe and fixed metadata limit; no corpus or source payload is read.
# Outputs: Complete bounded bytes or a failure that prevents admission of a skipped analysis.
def read_paths(stream) -> bytes:
    data = bytearray()
    while chunk := stream.read(min(65536, MAX_PATH_BYTES + 1 - len(data))):
        data.extend(chunk)
        if len(data) > MAX_PATH_BYTES:
            raise ValueError("C++ security change inventory exceeds its metadata bound")
    return bytes(data)


# Purpose: Read the complete push range, including deleted and renamed native inputs.
# Inputs: Trusted checkout and validated complete base SHA; external diffs and text conversion are disabled.
# Outputs: Canonical UTF-8 inventory; missing history, timeout, excess output and Git errors fail closed.
def changed_paths(root: Path, base: str) -> list[str]:
    command = [
        "git",
        "-C",
        str(root),
        "diff",
        "--no-ext-diff",
        "--no-textconv",
        "--no-renames",
        "--name-only",
        "-z",
        base,
        "HEAD",
    ]
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
        timer = threading.Timer(30, process.kill)
        timer.start()
        try:
            with ThreadPoolExecutor(max_workers=2) as pool:
                output = pool.submit(read_paths, process.stdout)
                errors = pool.submit(read_paths, process.stderr)
                try:
                    names = output.result(timeout=35)
                    error = errors.result(timeout=5)
                    if process.wait(timeout=5) != 0:
                        raise ValueError(
                            "C++ security Git range failed: " + error.decode("utf-8", errors="replace")[:2048]
                        )
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.wait(timeout=5)
        finally:
            timer.cancel()
            timer.join()
    if names and not names.endswith(b"\0"):
        raise ValueError("C++ security Git inventory is not terminated")
    return names.decode("utf-8", errors="strict").rstrip("\0").split("\0") if names else []


# Purpose: Match all native receipt and query inputs without filtering any analysis database.
# Inputs: Complete normalized changes and explicit broad intent; deleted files need no filesystem access.
# Outputs: Whole-database admission; malformed paths prevent an unchanged-input decision.
def select_cpp(paths: list[str], *, full: bool = False) -> dict:
    changed = False
    native_files = {item.casefold() for item in INPUT_FILES}
    native_roots = tuple(root.casefold() + "/" for root in INPUT_ROOTS)
    for name in paths:
        path = PurePosixPath(name)
        if (
            not name
            or path.is_absolute()
            or path.as_posix() != name
            or "\\" in name
            or ":" in name
            or any(part.casefold() in (".", "..", ".git") for part in path.parts)
        ):
            raise ValueError("C++ security inventory contains a noncanonical path")
        canonical = name.casefold()
        changed |= (
            canonical in native_files
            or canonical.startswith(native_roots)
            or path.suffix.casefold() in SOURCE_SUFFIXES
            or canonical in POLICY_FILES
            or any(
                canonical.startswith(prefix)
                for prefix in (".github/codeql/", ".github/actions/", "fuzz/", ".clusterfuzzlite/")
            )
        )
    return {
        "codeql_cpp": full or changed,
        "codeql_hip_host": full or changed,
        "codeql_configurations": ["cpu", "hip-host"] if full or changed else ["cpu"],
        "whole_database": True,
        "changed_path_count": len(paths),
        "reason": "explicit full qualification"
        if full
        else "C++ inputs changed"
        if changed
        else "native/query inputs unchanged",
    }


# Purpose: Resolve complete GitHub push evidence and unconditional scheduled/manual analysis intent.
# Inputs: Event, complete push base and checkout; initial pushes require the full database.
# Outputs: Admission record; absent or invalid history raises instead of silently skipping CodeQL.
def event_plan(event: str, base: str, root: Path = ROOT) -> dict:
    if event in ("schedule", "workflow_dispatch"):
        if base:
            raise ValueError("Only a push may supply a C++ security comparison base")
        return select_cpp([], full=True)
    if event != "push" or re.fullmatch(r"[0-9a-f]{40}", base) is None:
        raise ValueError("C++ security push requires a complete event base SHA")
    if base == "0" * 40:
        return select_cpp([], full=True)
    return select_cpp(changed_paths(root, base))


# Purpose: Publish tested whole-database admission through the runner-owned job-output file.
# Inputs: Explicit event/base and optional output path; no shell expression is evaluated.
# Outputs: Lowercase admission, complete configuration matrix and JSON decision, or nonzero failure.
def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event", choices=("push", "schedule", "workflow_dispatch"), required=True)
    parser.add_argument("--push-base", default="")
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args()
    validate_workflow((ROOT / ".github/workflows/security-code-scanning.yml").read_text(encoding="utf-8"))
    plan = event_plan(args.event, args.push_base)
    if args.github_output:
        with args.github_output.open("a", encoding="utf-8", newline="\n") as output:
            output.write(f"codeql_cpp={str(plan['codeql_cpp']).lower()}\n")
            output.write(f"codeql_hip_host={str(plan['codeql_hip_host']).lower()}\n")
            configurations = json.dumps(plan["codeql_configurations"], separators=(",", ":"))
            output.write(f"codeql_configurations={configurations}\n")
    print(json.dumps(plan, sort_keys=True))


# Purpose: Reject a workflow that combines incompatible build definitions or silently retires a configuration.
# Inputs: The actual repository workflow text; this gate checks the owned C++ job before planner admission.
# Outputs: Returns on the explicit matrix/build/category contract; mutations raise and admit both downstream jobs.
def validate_workflow(text: str) -> None:
    start = text.find("\n  codeql-cpp:\n")
    end = text.find("\n  codeql-actions:\n", start)
    if start < 0 or end < 0:
        raise ValueError("C++ security workflow job boundaries changed")
    job = text[start:end]
    canonical = " ".join(job.split())
    required = (
        "fail-fast: false matrix: configuration:",
        "fromJSON((needs.cpp-security-plan.result == 'success' && "
        'needs.cpp-security-plan.outputs.codeql_configurations) || \'["cpu","hip-host"]\')',
        "queries: security-extended,security-and-quality",
        "'/language:c-cpp' || '/language:c-cpp/configuration:hip-host'",
        "name: codeql-cpp-raw-${{ matrix.configuration }}-${{ github.sha }}",
        "if-no-files-found: error",
    )
    if any(item not in canonical for item in required) or job.count("tools/build.ps1") != 2:
        raise ValueError("C++ security configuration, coverage or report-retention contract changed")
    for step, configuration, arguments in (
        ("Build C++ database input", "cpu", "-CpuOnlyValidation"),
        ("Build HIP host database input", "hip-host", "-HipArch gfx1201 -HipPath $env:HIP_PATH"),
    ):
        expected = (
            f"- name: {step} if: matrix.configuration == '{configuration}' "
            f"shell: pwsh run: tools/build.ps1 -Configuration Release {arguments}"
        )
        if expected not in canonical:
            raise ValueError("C++ security must trace one actual complete build per configuration")


if __name__ == "__main__":
    main()
