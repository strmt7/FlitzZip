"""Measure native CPU test coverage with the pinned Windows debugger-based collector."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import stat
import sys
import time
import uuid
from pathlib import Path
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, build_opener

from tools.native_build_receipt import validate_current
from tools.native_coverage_report import normalize_report

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / "tools/native-coverage-toolchain.json"
SCENARIO_INPUTS = (
    "tools/native_coverage.py",
    "tools/native_coverage_report.py",
    "tools/native-coverage-toolchain.json",
    "mcp/flitzzip_mcp.py",
    "tools/test_memory_benchmark_plan.ps1",
    "tools/test_memory_benchmark_corpus.ps1",
    "tools/bench.ps1",
    "tools/benchmark_statistics.ps1",
    "tools/benchmark_corpus.ps1",
    "tools/format_matrix_smoke.ps1",
)


# Purpose: Hash complete regular files without allocating their full contents.
# Inputs: File path. Outputs: SHA-256 digest; missing or unreadable files fail.
def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


# Purpose: Confine writable artifacts and tool execution to an owned, unredirected checkout directory.
# Inputs: Checkout root and candidate. Outputs: Absolute path, or refusal of traversal and reparse points.
def owned_path(root: Path, path: Path) -> Path:
    root = root.resolve(strict=True)
    candidate = Path(os.path.abspath(path))
    relative = candidate.relative_to(root)
    current = root
    for part in relative.parts:
        current /= part
        if current.exists() or current.is_symlink():
            attributes = current.lstat()
            if stat.S_ISLNK(attributes.st_mode) or getattr(attributes, "st_file_attributes", 0) & 0x400:
                raise ValueError("Coverage paths must not use symbolic links or reparse points")
    return candidate


# Purpose: Verify the complete executable/DLL closure before trusting a cached collector.
# Inputs: Installation directory and reviewed runtime file identities. Outputs: Verified executable or refusal.
def verify_collector(directory: Path, files: dict[str, str]) -> Path:
    if "OpenCppCoverage.exe" not in files:
        raise ValueError("The collector lock has no executable")
    actual = {path.name for path in directory.glob("*.dll")} | {"OpenCppCoverage.exe"}
    if actual != set(files):
        raise ValueError("The collector runtime file inventory differs from its lock")
    for name, expected in files.items():
        if Path(name).name != name or not re.fullmatch(r"[a-f0-9]{64}", expected):
            raise ValueError("Invalid collector runtime identity")
        path = owned_path(directory, directory / name)
        if not path.is_file() or digest(path) != expected:
            raise ValueError(f"Collector file identity mismatch: {name}")
    return directory / "OpenCppCoverage.exe"


# Purpose: Reuse the canonical Windows memory, deadline, output and process-tree containment.
# Inputs: Fixed repository module path. Outputs: Existing bounded command implementation without SDK shadowing.
def process_owner():
    spec = importlib.util.spec_from_file_location("flitzzip_coverage_child", ROOT / "mcp/flitzzip_mcp.py")
    if spec is None or spec.loader is None:
        raise ValueError("The repository process containment contract is unavailable")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# Purpose: Retain bounded diagnostics and reject collector/installer errors without running a replacement command.
# Inputs: Fixed argv, seconds and owned report path. Outputs: Successful invocation or diagnostic-bearing failure.
def run_checked(arguments: list[str], timeout: int, log: Path) -> None:
    result = process_owner().run_bounded_command(
        arguments, timeout_seconds=timeout, max_output_bytes=8 * 1024 * 1024, response_tail_bytes=8 * 1024 * 1024
    )
    log.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if result["exit_code"] != 0 or result["timed_out"] or result["output_limit_exceeded"]:
        raise RuntimeError(f"Coverage command failed; retained diagnostics: {log}")


# Purpose: Keep installer acquisition on authenticated official release transports, including every redirect.
# Inputs: URL supplied by the reviewed manifest or release server. Outputs: Refusal before foreign access.
def validate_release_url(url: str) -> None:
    parsed = urlsplit(url)
    if (
        parsed.scheme != "https"
        or parsed.hostname
        not in {"github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com"}
        or parsed.username is not None
        or parsed.password is not None
        or parsed.port not in (None, 443)
        or parsed.fragment
    ):
        raise ValueError("Collector acquisition requires an official HTTPS release host")


class ReleaseRedirects(HTTPRedirectHandler):
    max_redirections = 3
    max_repeats = 1

    # Purpose: Validate every redirect before urllib opens it, without trusting the initial origin alone.
    # Inputs: Native HTTP redirect metadata. Outputs: Standard redirect request or explicit refusal.
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        validate_release_url(newurl)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


# Purpose: Install only the reviewed official collector bytes into the checkout's tool cache.
# Inputs: Hash-locked manifest and owned cache. Outputs: Verified executable; altered caches remain rejected.
def provision(lock: dict, output: Path) -> Path:
    version = lock["version"]
    if not re.fullmatch(r"[0-9]{1,3}(?:\.[0-9]{1,3}){3}", version):
        raise ValueError("Invalid collector version")
    expected_url = (
        f"https://github.com/OpenCppCoverage/OpenCppCoverage/releases/download/release-{version}/"
        f"OpenCppCoverageSetup-x64-{version}.exe"
    )
    if lock["url"] != expected_url:
        raise ValueError("Collector URL differs from its official versioned installer")
    validate_release_url(lock["url"])
    directory = owned_path(ROOT, ROOT / "out/tools" / ("opencppcoverage-" + lock["version"]))
    if directory.exists():
        return verify_collector(directory, lock["files"])
    installer = owned_path(ROOT, output / f"OpenCppCoverageSetup-{uuid.uuid4().hex}.exe")
    started = time.monotonic()
    with build_opener(ReleaseRedirects()).open(lock["url"], timeout=30) as response, installer.open("xb") as stream:
        remaining = lock["installer_bytes"]
        while remaining:
            if time.monotonic() - started > 120:
                raise TimeoutError("Collector acquisition exceeded its deadline")
            chunk = response.read(min(65536, remaining))
            if not chunk:
                raise ValueError("Collector installer is incomplete")
            stream.write(chunk)
            remaining -= len(chunk)
        if response.read(1):
            raise ValueError("Collector installer exceeded its reviewed size")
    if digest(installer) != lock["installer_sha256"]:
        raise ValueError("Collector installer SHA-256 mismatch")
    run_checked(
        [
            str(installer),
            "/VERYSILENT",
            "/SUPPRESSMSGBOXES",
            "/NORESTART",
            "/SP-",
            "/CURRENTUSER",
            "/TASKS=",
            f"/DIR={directory}",
            f"/LOG={output / 'installer.log'}",
        ],
        180,
        output / "installer-process.json",
    )
    return verify_collector(directory, lock["files"])


# Purpose: Bind measured executables, their symbols and scenario drivers to complete immutable identities.
# Inputs: Existing Debug output and fixed repository scenario paths. Outputs: Complete path-to-digest map.
def measurement_identity() -> dict[str, str]:
    binaries = (
        f"build/Debug/{name}.{extension}" for name in ("flitzzip_tests", "flitzzip_cli") for extension in ("exe", "pdb")
    )
    return {name: digest(owned_path(ROOT, ROOT / name)) for name in (*binaries, *SCENARIO_INPUTS)}


# Purpose: Exercise the native registry and existing CLI failure/readback contracts under one collector tree.
# Inputs: Current CPU Debug build and owned evidence directory. Outputs: Per-scenario diagnostics; any failure stops.
def run_scenarios(output: Path) -> None:
    current = validate_current(ROOT, "Debug", False)
    if current["recipe"]["FLITZZIP_ENABLE_HIP"] != "OFF":
        raise ValueError("CPU coverage scenarios require HIP OFF")
    binary = ROOT / "build/Debug"
    scenarios = [
        ("registry", [str(binary / "flitzzip_tests.exe")]),
        ("cli-help", [str(binary / "flitzzip_cli.exe"), "--help"]),
        ("cli-version", [str(binary / "flitzzip_cli.exe"), "--version"]),
    ]
    for name, script in (
        ("plan", "test_memory_benchmark_plan.ps1"),
        ("corpus", "test_memory_benchmark_corpus.ps1"),
        ("formats", "format_matrix_smoke.ps1"),
    ):
        scenarios.append(
            (
                "cli-" + name,
                [
                    "powershell.exe",
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(ROOT / "tools" / script),
                    "-Configuration",
                    "Debug",
                ],
            )
        )
    for name, arguments in scenarios:
        run_checked(arguments, 900 if name == "registry" else 300, output / f"{name}-process.json")


# Purpose: Measure the native registry and CLI contracts in their existing symbols-enabled CPU build.
# Inputs: Current successful build receipt and verified collector.
# Outputs: Normalized Cobertura and source-bound metadata.
def collect(collector: Path, lock: dict, output: Path) -> None:
    before = validate_current(ROOT, "Debug", False)
    if before["recipe"]["FLITZZIP_ENABLE_HIP"] != "OFF":
        raise ValueError("The native CPU coverage lane requires HIP OFF; GPU qualification runs separately")
    binary = ROOT / "build/Debug"
    identities = measurement_identity()
    raw = output / "native-raw.xml"
    run_checked(
        [
            str(collector),
            "--quiet",
            "--cover_children",
            "--modules",
            str(binary / "*"),
            "--sources",
            str(ROOT / "src" / "*"),
            "--working_dir",
            str(ROOT),
            "--export_type",
            f"cobertura:{raw}",
            "--",
            sys.executable,
            "-B",
            "-m",
            "tools.native_coverage",
            "--run-scenarios",
        ],
        900,
        output / "native-process.json",
    )
    if validate_current(ROOT, "Debug", False) != before or measurement_identity() != identities:
        raise ValueError("Native source, binary or symbols changed during coverage measurement")
    verify_collector(collector.parent, lock["files"])
    summary = normalize_report(raw, output / "native.xml", ROOT)
    source_files = {
        name
        for name in before["receipt"]["inputs"]["files"]
        if name.startswith("src/") and Path(name).suffix in {".c", ".cpp", ".h", ".hpp"}
    }
    required = {
        "src/core/archive_format.cpp",
        "src/core/path_safety.cpp",
        "src/zip/zip_adapter.cpp",
        "src/cli/main.cpp",
        "src/cli/memory_benchmark.cpp",
        "src/cli/memory_benchmark_source.cpp",
    }
    if not required.issubset(summary["files"]):
        raise ValueError("Coverage missed required native parser, filesystem or ZIP consumers")
    record = {
        "scope": "native-cpu-first-party-lines",
        "configuration": "Debug",
        "gpu_device_coverage": False,
        "native_inputs_sha256": before["inputs_sha256"],
        "native_receipt_sha256": before["receipt_sha256"],
        "collector_version": lock["version"],
        "toolchain_sha256": digest(LOCK),
        "test_pdb_sha256": identities["build/Debug/flitzzip_tests.pdb"],
        "test_binary_sha256": identities["build/Debug/flitzzip_tests.exe"],
        "measurement_inputs": identities,
        "report_sha256": digest(output / "native.xml"),
        "unmeasured_source_files": sorted(source_files - set(summary["files"])),
        **summary,
    }
    (output / "summary.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(
        json.dumps(
            {
                "coverage": "native-cpu",
                "files": len(summary["files"]),
                "lines_covered": summary["lines_covered"],
                "lines_valid": summary["lines_valid"],
            }
        )
    )


# Purpose: Provision or measure coverage without silently rebuilding the product.
# Inputs: Optional provisioning-only flag. Outputs: Verified tool or measured report; unsupported hosts fail explicitly.
def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--provision-only", action="store_true")
    modes.add_argument("--run-scenarios", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if os.name != "nt":
        raise ValueError("Native coverage uses the Windows MSVC/PDB collector")
    output = owned_path(ROOT, ROOT / "out/coverage")
    output.mkdir(parents=True, exist_ok=True)
    if args.run_scenarios:
        run_scenarios(output)
        return
    lock = json.loads(LOCK.read_text(encoding="utf-8-sig"))
    collector = provision(lock, output)
    if args.provision_only:
        print(f"Verified OpenCppCoverage {lock['version']}")
    else:
        collect(collector, lock, output)


if __name__ == "__main__":
    main()
