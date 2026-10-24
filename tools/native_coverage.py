"""Measure native host instructions with the pinned Windows debugger-based collector."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import io
import json
import math
import os
import re
import stat
import sys
import time
import uuid
import zipfile
from pathlib import Path
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, build_opener

from tools.native_build_receipt import validate_current
from tools.native_coverage_report import normalize_report

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / "tools/native-coverage-toolchain.json"
BASE_COLLECTION_TIMEOUT_SECONDS = 900
GUI_SCENARIO_TIMEOUT_SECONDS = 900
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
GUI_SCENARIO_INPUTS = (
    "tools/gui_smoke.ps1",
    "tools/FlitzZip.GuiSmoke.Ui.psm1",
    "tools/FlitzZip.GuiSmoke.Pickers.psm1",
    "tools/FlitzZip.GuiSmoke.Hover.psm1",
    "tools/process_environment.ps1",
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
# Inputs: Fixed argv, seconds and owned report path. Outputs: Retained result or diagnostic-bearing failure.
def run_checked(arguments: list[str], timeout: int, log: Path) -> dict:
    result = process_owner().run_bounded_command(
        arguments,
        timeout_seconds=timeout,
        max_output_bytes=8 * 1024 * 1024,
        response_tail_bytes=8 * 1024 * 1024,
        response_max_characters=8 * 1024 * 1024,
    )
    log.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if result["exit_code"] != 0 or result["timed_out"] or result["output_limit_exceeded"]:
        raise RuntimeError(f"Coverage command failed; retained diagnostics: {log}")
    return result


# Purpose: Describe independent CLI admission failures without starting a workload or relying on a GPU.
# Inputs: Plain input, existing destination and independently framed ZIP fixture paths.
# Outputs: Arguments, required exit status and diagnostic for each contract.
def cli_admission_cases(source: Path, destination: Path, zip_source: Path) -> list[tuple[list[str], int, str | None]]:
    create = ["compress", "--format", "zip", "--output", str(destination), str(source)]
    cases = [
        ([], 2, None),
        (["unknown-command"], 2, None),
        (["\ud800"], 1, "command-line argument contains invalid Unicode"),
        (["compress", "--output"], 1, "missing value for --output"),
        ([*create, "--unknown"], 1, "unknown compress argument"),
        ([*create, "--workers", "4294967296"], 1, "invalid value for --workers"),
        ([*create, "--inflight", "-1"], 1, "invalid value for --inflight"),
        ([*create, "--require-gpu", "--force-cpu"], 1, "mutually exclusive"),
        ([*create, "--neutron-star", "--force-cpu"], 1, "excludes --force-cpu"),
        ([*create, "--neutron-star", "--compression-level", "8"], 1, "requires compression level 9"),
        ([*create, "--neutron-star"], 1, "available only for native FLZIP creation"),
        ([*create, "--format", "tar", "--verify-after-write"], 1, "supported only for FLZIP and ZIP"),
        (["extract", "--unknown"], 1, "unknown extract argument"),
        (["verify", "--unknown"], 1, "unknown verify argument"),
        (["benchmark-suite", "--unknown"], 1, "unknown benchmark-suite argument"),
        (["benchmark-suite", "--size-mib"], 1, "missing value for --size-mib"),
        (["benchmark-suite", "--workers", "4294967296"], 1, "invalid value for --workers"),
        (["benchmark-suite", "--compression-level", "10"], 1, "invalid value for --compression-level"),
        (["gpu-diagnostic", "--unknown"], 1, "unknown gpu-diagnostic argument"),
        (["gpu-diagnostic", "--seconds", "0"], 1, "invalid value for --seconds: 0"),
        (
            ["compress", "--format", "rar", "--output", str(destination), str(source)],
            1,
            "archive format recognized but not yet implemented for create: rar",
        ),
        (
            ["verify", str(source), "--sha256", "--defender-scan"],
            1,
            "read-only verify is currently supported only for FLZIP and ZIP",
        ),
        (
            ["extract", str(source), "--output", str(destination)],
            1,
            "unable to detect archive format",
        ),
        (["gpu-diagnostic", "--buffer-mib", "-1"], 1, "invalid value for --buffer-mib"),
        (["gpu-diagnostic", "--inner-iterations", "4294967296"], 1, "invalid value for --inner-iterations"),
    ]
    for tuning in ([], ["--tune"], ["--tune-levels"]):
        # Parse actual suite controls, then reject before either lane performs a synthetic benchmark.
        cases.append(
            (
                [
                    "benchmark-suite",
                    "--size-mib",
                    "1",
                    "--profile",
                    "RepeatedRecord",
                    "--workers",
                    "1",
                    "--block-size-kib",
                    "256",
                    "--compression-level",
                    "5",
                    *tuning,
                ],
                1,
                "memory benchmark workload must be at least 10240 MiB (10 GiB)",
            )
        )
    for arguments in (
        ["--force-cpu"],
        ["--require-gpu"],
        ["--workers", "1"],
        ["--inflight", "1"],
        ["--block-size-kib", "256"],
    ):
        cases.append(([*create, *arguments], 1, "does not support FLZIP GPU, worker, or block-size flags"))
    for command in ("gpu-info", "dependency-check", "formats"):
        cases.append(([command, "unexpected"], 2, None))
    for command in ("extract", "verify"):
        for options in (["--workers", "2"], ["--inflight", "2"], ["--require-gpu"], ["--force-cpu"]):
            arguments = [command, str(zip_source), *options]
            if command == "extract":
                arguments.extend(["--output", str(destination)])
            label = "ZIP/ZIPX" if command == "extract" else "ZIP"
            cases.append(
                (arguments, 1, label + " compatibility does not support FLZIP GPU, worker, or in-flight flags")
            )
    return cases


# Purpose: Require complete process evidence and the expected failure before admitting a CLI rejection test.
# Inputs: Native executable, case tuple and owned output directory. Outputs: Retained raw result or failure.
def run_cli_admission_case(binary: Path, case: tuple[list[str], int, str | None], log: Path) -> None:
    arguments, expected_exit, diagnostic = case
    result = process_owner().run_bounded_command(
        [str(binary), *arguments],
        timeout_seconds=20,
        max_output_bytes=65536,
        response_tail_bytes=65536,
        working_directory=log.parent,
    )
    log.write_text(json.dumps({"arguments": arguments, "result": result}, indent=2) + "\n", encoding="utf-8")
    if any(result[name] for name in ("timed_out", "output_truncated", "output_limit_exceeded")):
        raise RuntimeError(f"Incomplete CLI admission evidence: {log}")
    if result["exit_code"] != expected_exit or (diagnostic and diagnostic not in result["stderr"]):
        raise RuntimeError(f"CLI admission contract failed: {log}")
    if (expected_exit == 1 and result["stdout"]) or (
        expected_exit == 2 and (result["stderr"] or result["stdout"].splitlines()[:2] != ["FlitzZip CLI", "Usage:"])
    ):
        raise RuntimeError(f"CLI rejection emitted unexpected output: {log}")


# Purpose: Exercise real CLI rejection and destination-preservation boundaries under the native collector.
# Inputs: Existing executable, owned evidence directory and explicit HIP-host selection.
# Outputs: Preserved caller fixtures and raw reports; HIP mode adds real device work and isolated absence checks.
def run_cli_admission(binary: Path, output: Path, *, hip_host: bool = False) -> None:
    directory = owned_path(ROOT, output / ("cli-admission-" + uuid.uuid4().hex))
    directory.mkdir()
    source = directory / "input with spaces.txt"
    destination = directory / "preserved.zip"
    zip_source = directory / "independent.zip"
    fixtures = {
        source: b"Independent CLI admission input\n",
        destination: b"Existing destination must survive failed admission.\n",
    }
    with io.BytesIO() as buffer:
        with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_STORED) as archive:
            archive.writestr(zipfile.ZipInfo("item.txt", (1980, 1, 1, 0, 0, 0)), b"Independent ZIP admission fixture\n")
        fixtures[zip_source] = buffer.getvalue()
    for path, expected in fixtures.items():
        with path.open("xb") as stream:
            stream.write(expected)
    expected_files = {path.name for path in fixtures}
    for number, case in enumerate(cli_admission_cases(source, destination, zip_source)):
        log = directory / f"case-{number:02}.json"
        run_cli_admission_case(binary, case, log)
        expected_files.add(log.name)
        if any(path.read_bytes() != expected for path, expected in fixtures.items()) or (
            {path.name for path in directory.iterdir()} != expected_files
        ):
            raise RuntimeError(f"Rejected CLI request changed caller files: {log}")
    expected_files.update(run_cli_capability_contracts(binary, directory, hip_host))
    if hip_host:
        expected_files.update(run_cli_device_absence_contracts(binary, directory, source, destination))
    if any(path.read_bytes() != expected for path, expected in fixtures.items()) or (
        {path.name for path in directory.iterdir()} != expected_files
    ):
        raise RuntimeError("CLI capability requests changed caller files or produced unexpected output files")


# Purpose: Qualify explicit HIP no-device reporting and required-GPU refusal without altering the host driver.
# Inputs: Admitted HIP binary and caller-owned fixtures; visibility changes only in this serial controller's children.
# Outputs: Raw status/rejection logs and restored visibility; the caller independently verifies its original files.
def run_cli_device_absence_contracts(binary: Path, directory: Path, source: Path, destination: Path) -> list[str]:
    previous = os.environ.get("HIP_VISIBLE_DEVICES")
    logs = []
    os.environ["HIP_VISIBLE_DEVICES"] = "-1"
    try:
        for command, expected_exit in (("gpu-info", 1), ("dependency-check", 12)):
            result = process_owner().run_bounded_command(
                [str(binary), command],
                timeout_seconds=30,
                max_output_bytes=65536,
                response_tail_bytes=65536,
                working_directory=directory,
            )
            log = directory / f"no-device-{command}.json"
            with log.open("x", encoding="utf-8") as stream:
                json.dump({"environment": {"HIP_VISIBLE_DEVICES": "-1"}, "result": result}, stream, indent=2)
                stream.write("\n")
            if (
                type(result.get("exit_code")) is not int
                or result["exit_code"] != expected_exit
                or any(result.get(key) is not False for key in ("timed_out", "output_limit_exceeded"))
                or result.get("stderr") != ""
            ):
                raise RuntimeError(f"CLI no-device command failed: {log}")
            values = cli_key_values(result, command)
            expected = {
                "hip_compiled": "true",
                "hip_runtime_loadable": "true",
                "hip_kernel_loadable": "false",
                "available": "false",
                "device_count": "0",
                "selected_device": "-1",
                "status": "No AMD HIP device is available",
            }
            if command == "dependency-check":
                expected["dependency_status"] = "missing_amd_gpu"
            if any(values.get(key) != value for key, value in expected.items()):
                raise ValueError("CLI no-device status does not match explicit device absence")
            logs.append(log.name)
        log = directory / "no-device-neutron.json"
        case = (
            [
                "compress",
                "--format",
                "flzip",
                "--neutron-star",
                "--require-gpu",
                "--output",
                str(destination),
                str(source),
            ],
            1,
            "No AMD HIP device is available",
        )
        run_cli_admission_case(binary, case, log)
        logs.append(log.name)
    finally:
        if previous is None:
            os.environ.pop("HIP_VISIBLE_DEVICES", None)
        else:
            os.environ["HIP_VISIBLE_DEVICES"] = previous
    return logs


# Purpose: Reject incomplete or ambiguous machine-readable CLI output without losing native diagnostics.
# Inputs: A bounded process result and human-readable operation label. Outputs: Unique key/value fields or refusal.
def cli_key_values(result: dict, label: str) -> dict[str, str]:
    if result.get("output_truncated") is not False or not isinstance(result.get("stdout"), str):
        raise ValueError(f"{label} requires complete key/value evidence")
    pairs = [line.split("=", 1) for line in result["stdout"].splitlines()]
    if not pairs or any(len(pair) != 2 or not pair[0] for pair in pairs):
        raise ValueError(f"{label} requires complete key/value evidence")
    values = dict(pairs)
    if len(values) != len(pairs):
        raise ValueError(f"{label} contains duplicate fields")
    return values


# Purpose: Verify CLI dependency status and required-HIP diagnostic telemetry through real command entry points.
# Inputs: Admitted binary, private working directory and explicit host mode. Outputs: Retained log names or failure.
def run_cli_capability_contracts(binary: Path, directory: Path, hip_host: bool) -> list[str]:
    checks = [("dependency-check", ["dependency-check"], 0 if hip_host else 10)]
    if hip_host:
        checks.append(
            (
                "gpu-diagnostic",
                [
                    "gpu-diagnostic",
                    "--seconds",
                    "1",
                    "--buffer-mib",
                    "16",
                    "--inner-iterations",
                    "1",
                ],
                0,
            )
        )
    logs = []
    for name, arguments, expected_exit in checks:
        result = process_owner().run_bounded_command(
            [str(binary), *arguments],
            timeout_seconds=30,
            max_output_bytes=65536,
            response_tail_bytes=65536,
            working_directory=directory,
        )
        log = directory / f"{name}.json"
        with log.open("x", encoding="utf-8") as stream:
            json.dump({"arguments": arguments, "result": result}, stream, indent=2)
            stream.write("\n")
        if (
            type(result.get("exit_code")) is not int
            or result["exit_code"] != expected_exit
            or any(result.get(key) is not False for key in ("timed_out", "output_limit_exceeded"))
        ):
            raise RuntimeError(f"CLI capability command failed: {log}")
        values = cli_key_values(result, name)
        expected_available = "true" if hip_host else "false"
        mode_fields = ["hip_compiled", "hip_runtime_loadable", "available"]
        if name == "dependency-check":
            mode_fields.append("hip_kernel_loadable")
        if any(values.get(key) != expected_available for key in mode_fields):
            raise ValueError("CLI capability mode does not match the admitted build and device")
        if name == "dependency-check":
            if values.get("dependency_status") != ("ready" if hip_host else "cpu_only_build"):
                raise ValueError("CLI dependency status contradicts its exit code")
        else:
            integer_keys = (
                "diagnostic_bytes",
                "diagnostic_kernel_launches",
                "diagnostic_h2d_bytes",
                "diagnostic_d2h_bytes",
                "diagnostic_device_allocation_bytes",
                "diagnostic_checksum",
            )
            time_keys = ("diagnostic_kernel_ms", "diagnostic_wall_seconds")
            if not set((*integer_keys, *time_keys)).issubset(values):
                raise ValueError("GPU diagnostic CLI telemetry is incomplete")
            integers = {key: int(values[key]) for key in integer_keys}
            times = [float(values[key]) for key in time_keys]
            if (
                values.get("hip_runtime_loadable") != "true"
                or not values.get("gcn_arch")
                or any(integers[key] <= 0 for key in integer_keys if key != "diagnostic_checksum")
                or not 0 <= integers["diagnostic_checksum"] <= 2**64 - 1
                or any(not math.isfinite(value) or value <= 0 for value in times)
                or integers["diagnostic_bytes"] != 16 * 1024**2
                or integers["diagnostic_kernel_launches"] < 2
                or integers["diagnostic_h2d_bytes"] < integers["diagnostic_bytes"]
                or integers["diagnostic_device_allocation_bytes"] < integers["diagnostic_bytes"]
            ):
                raise ValueError("GPU diagnostic CLI lacks consistent positive HIP execution evidence")
        logs.append(log.name)
    return logs


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
    if time.monotonic() - started > 120:
        raise TimeoutError("Collector acquisition exceeded its deadline at EOF")
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
# Inputs: Existing Debug output, explicit GUI selection and fixed scenario paths. Outputs: Complete identity map.
def measurement_identity(include_gui: bool = False) -> dict[str, str]:
    programs = ("flitzzip_tests", "flitzzip_cli", *(["FlitzZip"] if include_gui else []))
    binaries = (f"build/Debug/{name}.{extension}" for name in programs for extension in ("exe", "pdb"))
    scripts = (*SCENARIO_INPUTS, *(GUI_SCENARIO_INPUTS if include_gui else ()))
    return {name: digest(owned_path(ROOT, ROOT / name)) for name in (*binaries, *scripts)}


# Purpose: Require the requested native build mode without treating a missing GPU as host qualification.
# Inputs: CPU-only or HIP host selection. Outputs: Current receipt; incompatible build mode fails explicitly.
def coverage_build(hip_host: bool) -> dict:
    current = validate_current(ROOT, "Debug", hip_host)
    required = "ON" if hip_host else "OFF"
    if current["recipe"]["FLITZZIP_ENABLE_HIP"] != required:
        raise ValueError(f"Native coverage requires HIP {required} for the selected measurement mode")
    return current


# Purpose: Reject unavailable or incomplete GPU admission before claiming a HIP-enabled host measurement.
# Inputs: Current Debug CLI and owned evidence directory. Outputs: Retained real capability result or failure.
def require_hip_device(output: Path) -> None:
    result = run_checked([str(ROOT / "build/Debug/flitzzip_cli.exe"), "gpu-info"], 60, output / "gpu-info-process.json")
    values = cli_key_values(result, "HIP host coverage")
    if any(
        values.get(name) != "true"
        for name in ("hip_compiled", "hip_runtime_loadable", "hip_kernel_loadable", "available")
    ):
        raise ValueError("HIP host coverage requires an available compiled HIP device")
    if not re.fullmatch(r"[1-9][0-9]*", values.get("device_count", "")):
        raise ValueError("HIP host coverage requires at least one real device")


# Purpose: Exercise the native registry and existing CLI failure/readback contracts under one collector tree.
# Inputs: Current Debug build, explicit host/GUI modes and owned directory. Outputs: Diagnostics; any failure stops.
def run_scenarios(output: Path, hip_host: bool = False, include_gui: bool = False) -> None:
    coverage_build(hip_host)
    if hip_host:
        require_hip_device(output)
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
    if include_gui:
        scenarios.append(
            (
                "gui",
                [
                    "powershell.exe",
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(ROOT / "tools/gui_smoke.ps1"),
                    "-Configuration",
                    "Debug",
                    "-ScenarioTimeoutSeconds",
                    str(GUI_SCENARIO_TIMEOUT_SECONDS),
                    "-ScreenshotPath",
                    str(output / "gui.png"),
                ],
            )
        )
    for name, arguments in scenarios:
        # Full suites launch many instrumented processes; retain the collector's
        # overall deadline and each matrix command's independent timeout.
        timeout = 900 if name in {"registry", "cli-formats"} else 300
        if name == "gui":
            timeout = GUI_SCENARIO_TIMEOUT_SECONDS
        run_checked(arguments, timeout, output / f"{name}-process.json")
    run_cli_admission(binary / "flitzzip_cli.exe", output, hip_host=hip_host)


# Purpose: Separate previous measurement evidence from a new attempt before any fallible admission.
# Inputs: Owned output directory; only the collector's fixed report names are moved, never deleted.
# Outputs: Prior reports remain recoverable locally; no old qualifying summary survives at the current path.
def retire_previous_measurement(output: Path) -> None:
    names = (
        "summary.json",
        "native.xml",
        "native-raw.xml",
        "native-process.json",
        "registry-process.json",
        "cli-help-process.json",
        "cli-version-process.json",
        "cli-plan-process.json",
        "cli-corpus-process.json",
        "cli-formats-process.json",
        "gui-process.json",
        "gpu-info-process.json",
    )
    paths = [owned_path(ROOT, output / name) for name in names]
    previous = [path for path in paths if path.exists()]
    if not previous:
        return
    archive = owned_path(ROOT, output / ("previous-measurement-" + uuid.uuid4().hex))
    archive.mkdir()
    # Retire the qualifying summary first: a later I/O failure cannot leave it beside a new report.
    for path in previous:
        path.rename(archive / path.name)


# Purpose: Measure native host instructions in an existing symbols-enabled build without inferring device hits.
# Inputs: Current successful build receipt, explicit host/GUI modes and verified collector.
# Outputs: Normalized Cobertura and source-bound metadata.
def collect(collector: Path, lock: dict, output: Path, hip_host: bool = False, include_gui: bool = False) -> None:
    retire_previous_measurement(output)
    before = coverage_build(hip_host)
    binary = ROOT / "build/Debug"
    identities = measurement_identity(include_gui)
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
            *(["--hip-host"] if hip_host else []),
            *(["--gui"] if include_gui else []),
        ],
        # GUI qualification adds a bounded stage to the existing native/CLI
        # transaction; it must not consume that transaction's entire deadline.
        BASE_COLLECTION_TIMEOUT_SECONDS + (GUI_SCENARIO_TIMEOUT_SECONDS if include_gui else 0),
        output / "native-process.json",
    )
    if coverage_build(hip_host) != before or measurement_identity(include_gui) != identities:
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
    if include_gui and not {"src/app/main.cpp", "src/app/main_window.cpp"}.issubset(summary["files"]):
        raise ValueError("Coverage missed required GUI consumers")
    record = {
        "scope": "native-hip-host-first-party-lines" if hip_host else "native-cpu-first-party-lines",
        "configuration": "Debug",
        "gpu_device_coverage": False,
        "gui_host_coverage": include_gui,
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
                "coverage": "native-hip-host" if hip_host else "native-cpu",
                "gui_host_coverage": include_gui,
                "files": len(summary["files"]),
                "lines_covered": summary["lines_covered"],
                "lines_valid": summary["lines_valid"],
            }
        )
    )


# Purpose: Provision or measure coverage without silently rebuilding the product.
# Inputs: Provisioning-only or explicit HIP host/GUI modes; GUI collection opens owned test windows.
# Outputs: Verified tool or measured report; unsupported hosts fail explicitly.
def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--provision-only", action="store_true")
    modes.add_argument("--run-scenarios", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument(
        "--hip-host", action="store_true", help="Measure host paths with real HIP; excludes device instructions"
    )
    parser.add_argument("--gui", action="store_true", help="Also open and measure the all-page GUI smoke test")
    args = parser.parse_args()
    if os.name != "nt":
        raise ValueError("Native coverage uses the Windows MSVC/PDB collector")
    if args.provision_only and (args.hip_host or args.gui):
        raise ValueError("--hip-host and --gui require coverage collection")
    directory = "coverage" + ("-hip-host" if args.hip_host else "") + ("-gui" if args.gui else "")
    output = owned_path(ROOT, ROOT / "out" / directory)
    output.mkdir(parents=True, exist_ok=True)
    if args.run_scenarios:
        run_scenarios(output, args.hip_host, args.gui)
        return
    lock = json.loads(LOCK.read_text(encoding="utf-8-sig"))
    collector = provision(lock, output)
    if args.provision_only:
        print(f"Verified OpenCppCoverage {lock['version']}")
    else:
        collect(collector, lock, output, args.hip_host, args.gui)


if __name__ == "__main__":
    main()
