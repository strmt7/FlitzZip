"""Complete compiler-emitted HIP profiling registrations without changing device code."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

from tools import hip_coverage as coverage
from tools.hip_coverage_toolchain import LOCK, read_toolchain, report_object
from tools.native_coverage import run_checked

SOURCE_DIRECTORY = coverage.ROOT / "tools/hip_profile_registration"


# Purpose: Retain complete bounded native metadata beyond the interactive response tail without losing failures.
# Inputs: Validated native argv and fresh owned log path. Outputs: Strict JSON object, raw result and exact command.
def native_metadata(command: list[str], log: Path, timeout: int = 60) -> dict:
    with log.with_name(log.stem + "-command.json").open("x", encoding="utf-8") as stream:
        json.dump(command, stream, indent=2)
    result = run_checked(command, timeout, log)
    if result["output_truncated"]:
        raise RuntimeError("Native profiling metadata was truncated; qualification is refused")
    return report_object(result["stdout"])


# Purpose: Admit the reviewed native registration ABI before compiling any control with it.
# Inputs: Explicit existing tool and owned evidence directory. Outputs: Exact identify report, or refusal.
def admit_tool(tool: Path, output: Path) -> dict:
    provenance = read_toolchain()
    observed = native_metadata([str(tool), "--identify"], output / "registration-tool.json", 30)
    expected = {
        "schema_version": 1,
        "llvm_revision": provenance["revision"],
        "toolchain_sha256": coverage.digest(LOCK),
        "toolchain_parser_sha256": coverage.digest(LOCK.with_name("hip_coverage_toolchain.py")),
        "source_sha256": coverage.digest(SOURCE_DIRECTORY / "hip_profile_registration.cpp"),
        "cmake_sha256": coverage.digest(SOURCE_DIRECTORY / "CMakeLists.txt"),
        "profile_record_bytes": 72,
        "host": "windows-x64",
        "device": "amdgpu-elf64le",
    }
    if observed != expected or type(observed.get("schema_version")) is not int:
        raise ValueError("Registration tool differs from the reviewed profiling ABI")
    return observed


# Purpose: Compile original-source host IR using the device image's explicitly shared translation-unit identity.
# Inputs: Captured baseline argv, same-TU bundle and owned work directory. Outputs: Bitcode and its native diagnostics.
def compile_host(command: list[str], bundle: Path, work: Path, *, source: Path | None = None) -> Path:
    if source is None:
        source = coverage.PROBE
    source_index = command.index(str(source))
    frontend = command[:source_index]
    runtime_index = frontend.index("-Xoffload-linker")
    del frontend[runtime_index : runtime_index + 2]
    host = work / "original.bc"
    coverage.checked(
        [
            *frontend,
            "--offload-host-only",
            "-emit-llvm",
            "-c",
            "-Xclang",
            "-fcuda-include-gpubinary",
            "-Xclang",
            str(bundle),
            str(source),
            "-o",
            str(host),
        ],
        work / "compile-host.json",
    )
    return host


# Purpose: Require complete inventory closure and preservation of original program values before relinking.
# Inputs: Native transform report. Outputs: Success only for consistent, fully registered actual ELF records.
def validate_completion(report: dict) -> None:
    before, after = report.get("before"), report.get("after")
    completed = report.get("completed_records")
    if not isinstance(before, dict) or not isinstance(after, dict):
        raise ValueError("Registration completion lacks before/after inventories")
    fields = (
        before.get("device_data_records"),
        before.get("registered_data_records"),
        after.get("device_data_records"),
        after.get("registered_data_records"),
        completed,
        before.get("host_profile_groups"),
        before.get("absent_host_profile_groups"),
        after.get("host_profile_groups"),
        after.get("absent_host_profile_groups"),
        report.get("retired_profile_groups"),
    )
    if any(type(value) is not int or value < 0 for value in fields):
        raise ValueError("Registration completion has invalid inventory counts")
    (
        device,
        registered,
        after_device,
        after_registered,
        added,
        host_groups,
        absent,
        after_groups,
        after_absent,
        retired,
    ) = fields
    table = before.get("table")
    missing = before.get("missing")
    if (
        device < 1
        or device > 100000
        or host_groups < 1
        or host_groups > 100000
        or host_groups != registered + absent
        or registered > device
        or device != after_device
        or after_registered != device
        or registered + added != device
        or retired != absent
        or after_groups != device
        or after_absent != 0
        or report.get("original_program_values_preserved") is not True
        or not isinstance(missing, list)
        or len(missing) != added
        or any(not isinstance(name, str) or not name.startswith("__profd_") or len(name) <= 8 for name in missing)
        or len(set(missing)) != len(missing)
        or after.get("missing") != []
        or not isinstance(table, str)
        or not re.fullmatch(r"__llvm_profile_sections_[a-f0-9]{1,16}", table)
        or table != after.get("table")
        or before.get("profile_record_bytes") != 72
        or after.get("profile_record_bytes") != 72
        or any(
            not isinstance(value.get("module_ir_sha256"), str)
            or not re.fullmatch(r"[a-f0-9]{64}", value["module_ir_sha256"])
            for value in (before, after)
        )
    ):
        raise ValueError("Registration completion did not preserve and close the actual device inventory")


# Purpose: Repair missing normal registrations at the LLVM host-emission boundary, preserving the complete GPU image.
# Inputs: Explicit pinned compiler/SDK/runtime/tool, target and unused owned evidence directory.
# Outputs: Linked control and frozen inputs; no generated executable or GPU operation runs here.
def compile_registered_probe(
    compiler: Path, sdk: Path, runtime: Path, tool: Path, target: str, output: Path
) -> tuple[Path, dict]:
    extra_inputs = [
        tool,
        LOCK,
        LOCK.with_name("hip_coverage_toolchain.py"),
        Path(__file__).resolve(),
        coverage.ROOT / "tools/hip_registration_native_checks.py",
        SOURCE_DIRECTORY / "CMakeLists.txt",
        SOURCE_DIRECTORY / "hip_profile_registration.cpp",
    ]
    extra_before = coverage.identities(extra_inputs)
    tool_identity = admit_tool(tool, output)
    baseline = output / "registration-baseline"
    work = output / "registration-work"
    baseline.mkdir()
    work.mkdir()
    cuid = hashlib.sha256(coverage.PROBE.read_bytes() + target.encode("ascii")).hexdigest()
    executable, inputs = coverage.compile_probe(compiler, sdk, runtime, target, baseline, cuid=cuid)
    version = report_object((baseline / "compiler.json").read_text(encoding="utf-8"))
    if tool_identity["llvm_revision"] not in version.get("stdout", ""):
        raise ValueError("Coverage compiler differs from the registration tool's reviewed source revision")
    command = report_object((baseline / "compile.json").read_text(encoding="utf-8"))["command"]
    original = coverage.device_image(compiler / "bin", executable, target, baseline)
    host = compile_host(command, baseline / "image.hipfb", work)
    from tools.hip_registration_native_checks import NativeChecks

    NativeChecks(tool, compiler / "bin/clang++.exe", host, original, output / "registration-native-checks").run()
    completed = work / "completed.bc"
    transformed = native_metadata(
        [str(tool), "--host", str(host), "--device", str(original), "--complete", "-o", str(completed)],
        work / "complete.json",
    )
    validate_completion(transformed)
    source_index = command.index(str(coverage.PROBE))
    link = command[source_index + 1 :]
    result = output / "probe.exe"
    link[link.index("-o") + 1] = str(result)
    coverage.checked(
        [
            str(compiler / "bin/clang++.exe"),
            "-x",
            "ir",
            str(completed),
            "-O0",
            "-g",
            "-fms-runtime-lib=dll",
            "-fprofile-instr-generate",
            "-fcoverage-mapping",
            *link,
        ],
        work / "link.json",
    )
    image = coverage.device_image(compiler / "bin", result, target, work)
    if image.read_bytes() != original.read_bytes():
        raise ValueError("Registration completion changed the device image; generated code must not execute")
    if (
        coverage.identities(extra_inputs) != extra_before
        or coverage.identities([Path(name) for name in inputs]) != inputs
    ):
        raise ValueError("Registration source, tool or compiler inputs changed during compilation")
    artifacts = coverage.identities([executable, original, host, completed, image])
    return result, {**inputs, **extra_before, **artifacts}
