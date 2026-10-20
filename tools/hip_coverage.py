"""Qualify actual AMDGPU branch counters independently of host dispatch coverage."""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from contextlib import contextmanager
from pathlib import Path

from tools.bootstrap_rocm_sdk import reject_link_chain
from tools.native_coverage import ROOT, digest, owned_path, process_owner

PROBE = ROOT / "tools/fixtures/hip_coverage_probe.hip.cpp"
TOOL_NAMES = (
    "clang++.exe",
    "llvm-objcopy.exe",
    "clang-offload-bundler.exe",
    "llvm-profdata.exe",
    "llvm-cov.exe",
)


# Purpose: Reject malformed architecture strings before constructing compiler or bundle arguments.
# Inputs: One explicit device target. Outputs: Validated single target, without compiler options or path syntax.
def architecture(value: str) -> str:
    if not re.fullmatch(r"gfx[0-9a-z]{3,12}", value):
        raise ValueError("Device coverage requires one explicit gfx architecture")
    return value


# Purpose: Bind evidence to regular, unchanged files without accepting redirected inputs.
# Inputs: Existing file paths. Outputs: Sizes and SHA-256 values; missing or redirected files fail.
def identities(paths: list[Path]) -> dict:
    result = {}
    for path in paths:
        reject_link_chain(path)
        if not path.is_file():
            raise ValueError(f"Missing coverage input: {path.name}")
        result[str(path.resolve())] = {"bytes": path.stat().st_size, "sha256": digest(path)}
    return result


# Purpose: Scope profiling and loader settings to this owned child invocation.
# Inputs: Temporary environment values. Outputs: Restores every previous value even after an exception.
@contextmanager
def child_environment(values: dict[str, str]):
    before = {name: os.environ.get(name) for name in values}
    try:
        os.environ.update(values)
        yield
    finally:
        for name, value in before.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value


# Purpose: Execute one bounded tool and retain complete success or failure diagnostics.
# Inputs: Argument vector, owned fresh log path and timeout. Outputs: Successful result; failure retains its log.
def checked(command: list[str], log: Path, timeout: int = 120) -> dict:
    result = process_owner().run_bounded_command(
        command, timeout_seconds=timeout, max_output_bytes=8 * 1024**2, response_tail_bytes=8 * 1024**2
    )
    log.write_text(json.dumps({"command": command, **result}, indent=2) + "\n", encoding="utf-8")
    if result["exit_code"] != 0 or result["timed_out"] or result["output_limit_exceeded"] or result["output_truncated"]:
        raise RuntimeError(f"Device coverage command failed; diagnostics: {log}")
    return result


# Purpose: Extract an actual device ELF from one HIP image without modifying the measured binary.
# Inputs: Qualified LLVM tools, binary, exact gfx target and fresh output directory.
# Outputs: Device image; absent, ambiguous or host-only images fail before report interpretation.
def device_image(llvm_bin: Path, binary: Path, target: str, output: Path, *, coff_object: bool = False) -> Path:
    target = architecture(target)
    fat = output / "image.hipfb"
    section = ".hip_fatbin" if coff_object else ".hip_fat"
    checked(
        [
            str(llvm_bin / "llvm-objcopy.exe"),
            f"--dump-section={section}={fat}",
            str(binary),
            str(output / "image-copy.bin"),
        ],
        output / "extract.json",
    )
    if not fat.is_file() or not fat.stat().st_size:
        raise ValueError("Measured binary has no HIP device image")
    bundle = llvm_bin / "clang-offload-bundler.exe"
    listed = checked([str(bundle), "--type=o", f"--input={fat}", "--list"], output / "bundles.json")
    entries = listed["stdout"].splitlines()
    selected = [entry for entry in entries if re.fullmatch(r"hip-amd(?:gcn|gpu)-amd-amdhsa--" + target, entry)]
    if len(selected) != 1:
        raise ValueError("HIP image does not contain exactly one requested device bundle")
    device = output / "device.elf"
    checked(
        [str(bundle), "--type=o", f"--input={fat}", f"--output={device}", f"--targets={selected[0]}", "--unbundle"],
        output / "unbundle.json",
    )
    with device.open("rb") as stream:
        header = stream.read(20)
    if len(header) != 20 or header[:6] != b"\x7fELF\x02\x01" or int.from_bytes(header[18:20], "little") != 224:
        raise ValueError("Coverage requires a 64-bit AMDGPU ELF, not a host executable")
    return device


# Purpose: Merge native device counters and ask LLVM to interpret its own coverage mapping.
# Inputs: Tools, device ELF and nonempty raw profile paths from one qualified binary.
# Outputs: Retained native JSON and merged profile; malformed or mismatched evidence is rejected by LLVM.
def export_profile(llvm_bin: Path, image: Path, profiles: list[Path], output: Path) -> dict:
    if not profiles:
        raise ValueError("Device execution produced no raw profiles")
    identities(profiles)
    merged = output / "device.profdata"
    checked(
        [str(llvm_bin / "llvm-profdata.exe"), "merge", "-sparse", "-o", str(merged), *map(str, profiles)],
        output / "merge.json",
    )
    native = output / "device.json"
    checked(
        [
            sys.executable,
            "-B",
            "-m",
            "tools.hip_coverage_export",
            "--tool",
            str(llvm_bin / "llvm-cov.exe"),
            "--image",
            str(image),
            "--profile",
            str(merged),
            "--output",
            str(native),
        ],
        output / "export.json",
    )
    report = json.loads(native.read_text(encoding="utf-8"))
    return report


# Purpose: Prove that instrumentation distinguishes real device hits from deliberate misses.
# Inputs: LLVM exports for false, true and merged runs, and their exact probe source.
# Outputs: Independent expected counter evidence; fabricated all-hit, swapped or host-only reports fail.
def validate_controls(reports: list[dict], source: Path) -> dict:
    if len(reports) != 3:
        raise ValueError("Qualification requires false, true and merged reports")
    expected = ((0, 192), (192, 0), (192, 192))
    contracts = {
        "_Z22coverage_counter_probePii": ("if (selector != 0)", expected),
        "_Z21coverage_helper_probei": ("if (selector == 0)", (expected[1], expected[0], expected[2])),
    }
    source_lines = source.read_text(encoding="utf-8").splitlines()
    for index, report in enumerate(reports):
        if report.get("type") != "llvm.coverage.json.export" or report.get("version") != "3.1.0":
            raise ValueError("Unsupported LLVM coverage export schema")
        data = report.get("data", [])
        if len(data) != 1 or len(data[0].get("functions", [])) != len(contracts):
            raise ValueError("Probe must measure its kernel and non-inlined device helper")
        functions = data[0]["functions"]
        if {function.get("name") for function in functions} != set(contracts):
            raise ValueError("Probe device functions differ from the controls")
        for function in functions:
            marker, counts = contracts[function["name"]]
            expected_count = 192 if index < 2 else 384
            if type(function.get("count")) is not int or function["count"] != expected_count:
                raise ValueError(
                    "Probe device function invocation count differs from the controls: "
                    f"{function['name']}, export={index}, observed={function.get('count')!r}, "
                    f"expected={expected_count}"
                )
            if function.get("filenames") != [str(source.resolve())]:
                raise ValueError("Device mapping does not identify the exact probe source")
            branches = function.get("branches", [])
            if len(branches) != 1 or len(branches[0]) != 9:
                raise ValueError("Probe function must contain exactly one measured device branch")
            branch = branches[0]
            if any(type(value) is not int or value < 0 for value in branch) or tuple(branch[4:6]) != counts[index]:
                raise ValueError("Device hit and miss controls did not match independently known branches")
            if branch[0] > len(source_lines) or branch[0] < 1 or marker not in source_lines[branch[0] - 1]:
                raise ValueError("Device branch mapping differs from the probe source")
    return {
        "false_counts": [0, 192],
        "true_counts": [192, 0],
        "merged_counts": [192, 192],
        "helper_false_counts": [192, 0],
        "helper_true_counts": [0, 192],
        "helper_merged_counts": [192, 192],
    }


# Purpose: Compile a bounded control using the selected SDK and matching host/device profiler.
# Inputs: Explicit LLVM installation, SDK, device runtime, architecture and fresh owned output.
# Outputs: A coverage-instrumented executable plus complete compiler diagnostics and immutable input identities.
def compile_probe(compiler: Path, sdk: Path, runtime: Path, target: str, output: Path) -> tuple[Path, dict]:
    llvm_bin = compiler / "bin"
    host_profiles = list((compiler / "lib/clang").glob("*/lib/windows/clang_rt.profile_rocm-x86_64.lib"))
    if len(host_profiles) != 1:
        raise ValueError("Compiler must supply exactly one matching Windows ROCm profiling runtime")
    hip_libraries = list((sdk / "bin").glob("amdhip64*.dll"))
    if len(hip_libraries) != 1:
        raise ValueError("SDK must contain exactly one HIP runtime library")
    inputs = [
        PROBE,
        Path(__file__).resolve(),
        ROOT / "tools/hip_coverage_export.py",
        ROOT / "tools/native_coverage.py",
        ROOT / "tools/bootstrap_rocm_sdk.py",
        ROOT / "mcp/flitzzip_mcp.py",
        runtime,
        host_profiles[0],
        sdk / "lib/amdhip64.lib",
        *hip_libraries,
        *[llvm_bin / name for name in TOOL_NAMES],
        *sorted(llvm_bin.glob("*.dll")),
        *sorted((compiler / "amdgcn/bitcode").glob("*.bc")),
    ]
    before = identities(inputs)
    targets = checked([str(llvm_bin / "clang++.exe"), "--print-targets"], output / "targets.json")
    if not re.search(r"(?m)^\s+amd(?:gcn|gpu)\s+-", targets["stdout"]):
        raise ValueError("Selected compiler has no AMDGPU backend")
    checked([str(llvm_bin / "clang++.exe"), "--version"], output / "compiler.json")
    executable = output / "probe.exe"
    checked(
        [
            str(llvm_bin / "clang++.exe"),
            "-x",
            "hip",
            f"--hip-path={sdk}",
            f"--rocm-path={sdk}",
            f"--rocm-device-lib-path={compiler / 'amdgcn/bitcode'}",
            f"--offload-arch={architecture(target)}",
            "-std=c++20",
            "-O0",
            "-g",
            "-fms-runtime-lib=dll",
            "-fprofile-instr-generate",
            "-fcoverage-mapping",
            "-mllvm",
            "-offload-pgo-sampling=0",
            "-Xoffload-linker",
            str(runtime),
            str(PROBE),
            "-o",
            str(executable),
            f"-L{sdk / 'lib'}",
            "-lamdhip64",
            "-Xlinker",
            "/nodefaultlib:clang_rt.profile-x86_64.lib",
            "-x",
            "none",
            str(host_profiles[0]),
        ],
        output / "compile.json",
    )
    if identities(inputs) != before:
        raise ValueError("Coverage inputs changed during compilation")
    return executable, before


# Purpose: Measure both controls on the actual device and verify byte-exact output before admitting counters.
# Inputs: Control executable, architecture and owned output. Outputs: One device profile per run and diagnostics.
def run_controls(executable: Path, target: str, output: Path) -> list[Path]:
    profiles = []
    for selector, expected in ((0, 18), (1, 22)):
        directory = output / f"selector-{selector}"
        directory.mkdir()
        with child_environment({"LLVM_PROFILE_FILE": str(directory / "probe.%p.profraw"), "LLVM_PROFILE_VERBOSE": "1"}):
            result = checked([str(executable), *(["true"] if selector else [])], directory / "execution.json", 30)
        observed = json.loads(result["stdout"])
        if observed != {"selector": selector, "value": expected, "lanes": 192, "architecture": target}:
            raise ValueError("GPU control readback or actual device architecture differs from its target")
        device_profiles = list(directory.glob(target + ".*.profraw"))
        if len(device_profiles) != 1 or not device_profiles[0].stat().st_size:
            raise ValueError("GPU control did not emit exactly one nonempty device profile")
        profiles.extend(device_profiles)
    return profiles


# Purpose: Qualify a selected device profiling toolchain without modifying the product build or its settings.
# Inputs: Explicit read-only tools and SDK, matching built device runtime, target and unused checkout output path.
# Outputs: Source/tool/binary/profile-bound qualification receipt; any failure retains diagnostics and no pass receipt.
def qualify(compiler: Path, sdk: Path, runtime: Path, target: str, output: Path) -> dict:
    target = architecture(target)
    for path in (compiler, sdk, runtime):
        reject_link_chain(path)
    output = owned_path(ROOT, output)
    output.mkdir(parents=True, exist_ok=False)
    probe_snapshot = output / "probe-source.hip.cpp"
    probe_snapshot.write_bytes(PROBE.read_bytes())
    llvm_bin = compiler / "bin"
    with child_environment({"PATH": os.pathsep.join((str(sdk / "bin"), str(llvm_bin), os.environ["PATH"]))}):
        executable, inputs = compile_probe(compiler, sdk, runtime, target, output)
        snapshot_identity = identities([probe_snapshot])[str(probe_snapshot.resolve())]
        if snapshot_identity != inputs.get(str(PROBE.resolve())):
            raise ValueError("Probe source changed between evidence capture and compilation")
        binary_identity = identities([executable])
        measurement = {
            "schema_version": 1,
            "qualification": "not-qualified",
            "architecture": target,
            "inputs": inputs,
            "artifacts": binary_identity,
        }
        (output / "measurement-inputs.json").write_text(json.dumps(measurement, indent=2) + "\n", encoding="utf-8")
        image = device_image(llvm_bin, executable, target, output)
        profiles = run_controls(executable, target, output)
        reports = [export_profile(llvm_bin, image, [profile], profile.parent) for profile in profiles]
        reports.append(export_profile(llvm_bin, image, profiles, output))
    controls = validate_controls(reports, PROBE)
    if identities([Path(name) for name in inputs]) != inputs or identities([executable]) != binary_identity:
        raise ValueError("Coverage inputs or measured binary changed during qualification")
    receipt = {
        "schema_version": 1,
        "qualification": "passed",
        "scope": "three-workgroup device kernel and non-inlined helper counter controls",
        "architecture": target,
        "inputs": inputs,
        "artifacts": identities([executable, image, probe_snapshot, *profiles]),
        "controls": controls,
    }
    (output / "qualification.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return receipt


# Purpose: Provide portable explicit-path qualification from an existing compatible x64 developer environment.
# Inputs: CLI arguments. Outputs: Small result and retained evidence; errors propagate without a fallback.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler-root", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--device-runtime", type=Path, required=True)
    parser.add_argument("--arch", required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    result = qualify(
        arguments.compiler_root.resolve(strict=True),
        arguments.sdk.resolve(strict=True),
        arguments.device_runtime.resolve(strict=True),
        arguments.arch,
        arguments.output.absolute(),
    )
    print(json.dumps({key: result[key] for key in ("qualification", "scope", "architecture", "controls")}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
