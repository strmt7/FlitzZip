"""Build and measure bounded product GPU regressions with exact device-image and source admission."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from tools import hip_coverage as coverage
from tools.hip_coverage_images import export_images, linked_images
from tools.hip_coverage_product import admit, read_record, require_identities, unique_input
from tools.native_coverage import cli_key_values, run_checked

BUILDER = Path(__file__).resolve().parent / "hip_profile_registration/build_product.ps1"


# Purpose: Admit only a configured, isolated development build belonging to this checkout and qualified manifest.
# Inputs: Exact manifest and existing out/ build directory. Outputs: Validated producer record; no build or GPU work.
def admit_build(manifest: Path, build: Path) -> dict:
    manifest = coverage.existing_input(manifest)
    build = coverage.existing_input(build)
    if not build.is_relative_to(coverage.ROOT / "out"):
        raise ValueError("Device measurement requires an isolated checkout out/ build")
    record = read_record(manifest)
    admit(manifest, Path(record["sdk"]), record["architecture"])
    cache = build / "CMakeCache.txt"
    coverage.existing_input(cache)
    if cache.stat().st_size > 5 * 1024**2:
        raise ValueError("Device measurement CMake cache exceeds its metadata bound")
    values = {}
    for line in cache.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([^#/:][^:]*):[^=]+=(.*)", line)
        if match:
            if match[1] in values:
                raise ValueError("Ambiguous development CMake cache")
            values[match[1]] = match[2]
    paths = {
        "CMAKE_HOME_DIRECTORY": coverage.ROOT,
        "FLITZZIP_HIP_COVERAGE_MANIFEST": manifest,
        "FLITZZIP_HIP_PATH": Path(record["sdk"]),
    }
    if any(not values.get(key) or coverage.existing_input(Path(values[key])) != path for key, path in paths.items()):
        raise ValueError("Device measurement build has a foreign source, manifest or SDK")
    if values.get("FLITZZIP_ENABLE_HIP") != "ON" or values.get("FLITZZIP_HIP_ARCH") != record["architecture"]:
        raise ValueError("Device measurement build differs from its qualified HIP target")
    return record


# Purpose: Freeze the actual native consumers separately from standalone object compiler inputs.
# Inputs: Admitted object record and current manifest. Outputs: Source, test, CMake, lifecycle and tool identities.
def consumer_inputs(record: dict, manifest: Path) -> dict:
    root = coverage.ROOT
    extra = [
        Path(__file__).resolve(),
        Path(__file__).with_name("hip_coverage_images.py"),
        BUILDER,
        manifest,
        root / "CMakeLists.txt",
        root / "tools/hip_profile_registration/hip_profile_lifecycle.cpp",
        *sorted((root / "cmake").rglob("*.cmake")),
        *sorted(path for path in (root / "tests/cpp").rglob("*") if path.is_file()),
    ]
    return {**record["inputs"], **record["artifacts"], **coverage.identities(extra)}


# Purpose: Require actual HIP admission and passing native oracles with complete device profiles on clean shutdown.
# Inputs: Current executable argv, role, qualified target and fresh directory. Outputs: Five retained actual profiles.
def run_case(command: list[str], target: str, output: Path, *, capability: bool = False) -> list[Path]:
    output.mkdir()
    with coverage.child_environment(
        {"LLVM_PROFILE_FILE": str(output / "host.%p.profraw"), "LLVM_PROFILE_VERBOSE": "1"}
    ):
        result = run_checked(command, 90, output / "process.json")
    if result["output_truncated"] or "failed" in result["stderr"].lower():
        raise ValueError("Incomplete product device execution or profiler collection failure")
    if capability:
        values = cli_key_values(result, "product device coverage admission")
        if any(
            values.get(key) != "true"
            for key in ("hip_compiled", "hip_runtime_loadable", "hip_kernel_loadable", "available")
        ):
            raise ValueError("Device coverage requires actual compatible HIP admission")
    elif not re.search(r"\b[1-9][0-9]* tests, 0 failed\b", result["stdout"]):
        raise ValueError("Device measurement lacks passing selected native regressions")
    profiles = sorted(output.glob(target + ".*.profraw"))
    if len(profiles) != 5 or any(not path.stat().st_size for path in profiles):
        raise ValueError("Product device measurement requires every native translation-unit profile")
    return profiles


# Purpose: Explicitly refresh the configured test target and measure selected bounded production regressions.
# Inputs: Qualified objects, isolated CMake build, explicit native test filters and fresh owned output.
# Outputs: Source/binary/profile-bound device coverage; no release qualification, benchmarks or CPU substitution.
def build_and_measure(manifest: Path, build: Path, selectors: list[str], output: Path) -> dict:
    if not selectors or len(selectors) > 64 or any(not re.fullmatch(r"=[A-Za-z0-9_]{1,180}", s) for s in selectors):
        raise ValueError("Choose explicit exact native tests after reviewing their instrumented workloads")
    if len(set(selectors)) != len(selectors):
        raise ValueError("Device measurement selectors must not repeat")
    manifest = coverage.existing_input(manifest)
    build = coverage.existing_input(build)
    record = admit_build(manifest, build)
    output = coverage.owned_path(coverage.ROOT, output)
    output.mkdir(parents=True, exist_ok=False)
    inputs = consumer_inputs(record, manifest)
    run_checked(
        [
            "powershell.exe",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(BUILDER),
            "-BuildRoot",
            str(build),
            "-Manifest",
            str(manifest),
            "-Python",
            sys.executable,
        ],
        600,
        output / "build.json",
    )
    require_identities(inputs)
    binary = build / "Debug"
    for name in ("flitzzip_tests.exe", "flitzzip_cli.exe", "flitzzip_hip_kernels.dll"):
        coverage.existing_input(binary / name)
    binaries = coverage.identities(
        sorted(path for path in binary.iterdir() if path.suffix.lower() in (".exe", ".dll", ".pdb"))
    )
    images = linked_images(record, binary / "flitzzip_hip_kernels.dll", output / "images")
    profiles = run_case(
        [str(binary / "flitzzip_cli.exe"), "gpu-info"], record["architecture"], output / "gpu-info", capability=True
    )
    for index, selector in enumerate(selectors):
        profiles.extend(
            run_case(
                [str(binary / "flitzzip_tests.exe"), selector], record["architecture"], output / f"case-{index:02}"
            )
        )
    llvm = unique_input([Path(name) for name in record["inputs"]], "clang++.exe").parent
    reader = unique_input([Path(name) for name in record["inputs"]], "flitzzip_hip_profile_register.exe")
    summary = export_images(llvm, reader, images, profiles, output / "export")
    require_identities(inputs)
    require_identities(binaries)
    result = {
        "qualification": "passed",
        "scope": "Selected product device regressions; not whole-repository coverage or release qualification",
        "selectors": selectors,
        "inputs": inputs,
        "binaries": binaries,
        "summary": summary,
        "artifacts": coverage.identities([*images.values(), *profiles, build / "CMakeCache.txt"]),
    }
    (output / "qualification.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


# Purpose: Expose read-only build admission and explicitly named build/measure work for configured development builds.
# Inputs: CLI paths and filters. Outputs: Small result, complete retained evidence or failure without a pass receipt.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for command in ("admit", "build-and-measure"):
        sub = commands.add_parser(command)
        sub.add_argument("--manifest", type=Path, required=True)
        sub.add_argument("--build-root", type=Path, required=True)
        if command == "build-and-measure":
            sub.add_argument("--filter", action="append", required=True)
            sub.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "admit":
        admit_build(args.manifest, args.build_root)
        print(json.dumps({"admitted": True, "scope": "development HIP measurement only"}))
    else:
        result = build_and_measure(args.manifest, args.build_root, args.filter, args.output)
        print(
            json.dumps(
                {
                    "qualification": result["qualification"],
                    "scope": result["scope"],
                    "lines": result["summary"]["lines"],
                    "covered": result["summary"]["covered"],
                }
            )
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
