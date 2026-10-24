"""Build and admit development-only instrumented objects from actual product HIP sources."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from tools import hip_coverage as coverage
from tools.hip_coverage_registration import admit_tool, compile_host, native_metadata, validate_completion
from tools.hip_coverage_toolchain import report_object

SOURCES = (
    "src/gpu/hip_codec.hip.cpp",
    "src/gpu/hip_codec_static_prefix.hip.cpp",
    "src/gpu/hip_codec_adaptive_prefix.hip.cpp",
    "src/gpu/hip_dictionary_matcher.hip.cpp",
    "src/gpu/hip_sparse_pattern.hip.cpp",
)
SCOPE = "development-hip-object-coverage-v1"
MAX_METADATA = 8 * 1024**2


# Purpose: Read bounded unambiguous evidence without following redirected paths.
# Inputs: Existing regular JSON file. Outputs: Strict object, or refusal before compilation/loading.
def read_record(path: Path) -> dict:
    coverage.existing_input(path)
    if not path.is_file() or path.stat().st_size > MAX_METADATA:
        raise ValueError("Product coverage metadata is not a bounded regular file")
    return report_object(path.read_text(encoding="utf-8"))


# Purpose: Require a complete exact-byte identity map rather than silently accepting a partial inventory.
# Inputs: Nonempty private path-to-size/hash map. Outputs: Unchanged admitted input paths, or refusal.
def require_identities(value: object) -> list[Path]:
    if not isinstance(value, dict) or not value or len(value) > 10000:
        raise ValueError("Product coverage lacks a bounded identity inventory")
    for name, identity in value.items():
        if (
            not isinstance(name, str)
            or not Path(name).is_absolute()
            or not isinstance(identity, dict)
            or set(identity) != {"bytes", "sha256"}
            or type(identity["bytes"]) is not int
            or identity["bytes"] < 0
            or not isinstance(identity["sha256"], str)
            or len(identity["sha256"]) != 64
        ):
            raise ValueError("Invalid product coverage input identity")
    paths = [Path(name) for name in value]
    if coverage.identities(paths) != value:
        raise ValueError("Product coverage inputs or artifacts changed")
    return paths


# Purpose: Select one explicitly qualified native dependency without arbitrary PATH discovery.
# Inputs: Frozen qualified paths and required basename. Outputs: Exactly one canonical existing path.
def unique_input(paths: list[Path], name: str) -> Path:
    selected = [path for path in paths if path.name == name]
    if len(selected) != 1:
        raise ValueError("Product coverage requires one qualified " + name)
    return selected[0]


# Purpose: Admit the actual GPU counter qualification before compiling product code.
# Inputs: Exact receipt and its retained source/tool/binary/profile artifacts. Outputs: Frozen dependencies and target.
def qualified_toolchain(path: Path) -> tuple[dict, dict]:
    record = read_record(path)
    if record.get("qualification") != "passed":
        raise ValueError("Product device coverage requires passing GPU counter qualification")
    target = coverage.architecture(record.get("architecture", ""))
    inputs = require_identities(record.get("inputs"))
    require_identities(record.get("artifacts"))
    if str(coverage.PROBE.resolve()) not in record["inputs"]:
        raise ValueError("Product coverage qualification lacks the actual helper-inclusive probe")
    toolchain = {
        "compiler": unique_input(inputs, "clang++.exe"),
        "sdk": unique_input(inputs, "amdhip64.lib").parent.parent,
        "runtime": unique_input(inputs, "libclang_rt.profile-amdgpu.a"),
        "host_runtime": unique_input(inputs, "clang_rt.profile_rocm-x86_64.lib"),
        "registration_tool": unique_input(inputs, "flitzzip_hip_profile_register.exe"),
        "architecture": target,
    }
    return toolchain, record["inputs"]


# Purpose: Compile one canonical product translation unit and repair only its validated profiling registrations.
# Inputs: Admitted tools, actual source and fresh owned directory. Outputs: Object with byte-identical GPU image.
def compile_object(tools: dict, source: Path, output: Path) -> tuple[Path, list[Path]]:
    output.mkdir()
    compiler, sdk, target = tools["compiler"], tools["sdk"], tools["architecture"]
    cuid = hashlib.sha256(
        source.relative_to(coverage.ROOT).as_posix().encode() + source.read_bytes() + target.encode()
    ).hexdigest()
    baseline = output / "baseline.obj"
    command = [
        str(compiler),
        "-x",
        "hip",
        f"--hip-path={sdk}",
        f"--rocm-path={sdk}",
        f"--rocm-device-lib-path={compiler.parent.parent / 'amdgcn/bitcode'}",
        f"--offload-arch={target}",
        "-cuid=" + cuid,
        "-std=c++20",
        "-O0",
        "-g",
        "-fms-runtime-lib=dll",
        "-DFLITZZIP_ENABLE_HIP=1",
        "-I" + str(coverage.ROOT / "src"),
        "-fprofile-instr-generate",
        "-fcoverage-mapping",
        "-mllvm",
        "-offload-pgo-sampling=0",
        "-Xoffload-linker",
        str(tools["runtime"]),
        "-c",
        str(source),
        "-o",
        str(baseline),
    ]
    coverage.checked(command, output / "compile.json", 180)
    image = coverage.device_image(compiler.parent, baseline, target, output, coff_object=True)
    host = compile_host(command, output / "image.hipfb", output, source=source)
    completed = output / "completed.bc"
    report = native_metadata(
        [
            str(tools["registration_tool"]),
            "--host",
            str(host),
            "--device",
            str(image),
            "--complete",
            "-o",
            str(completed),
        ],
        output / "complete.json",
    )
    validate_completion(report)
    obj = output / "completed.obj"
    coverage.checked(
        [
            str(compiler),
            "-x",
            "ir",
            "-c",
            str(completed),
            "-O0",
            "-g",
            "-fms-runtime-lib=dll",
            "-fprofile-instr-generate",
            "-fcoverage-mapping",
            "-o",
            str(obj),
        ],
        output / "lower-host.json",
        180,
    )
    comparison = output / "completed-image"
    comparison.mkdir()
    lowered = coverage.device_image(compiler.parent, obj, target, comparison, coff_object=True)
    if image.read_bytes() != lowered.read_bytes():
        raise ValueError("Product coverage changed a GPU executable image")
    return obj, [baseline, image, host, completed, obj, lowered]


# Purpose: Freeze the object producer's source/header inputs and qualified SDK environment.
# Inputs: Admitted toolchain. Outputs: Exact input list; CMake linking is a separately measured consumer.
def source_inputs(tools: dict) -> list[Path]:
    roots = [coverage.ROOT / "src", tools["sdk"] / "include"]
    paths = [path for root in roots for path in root.rglob("*") if path.is_file()]
    return [Path(__file__).resolve(), *sorted(paths)]


# Purpose: Prepare canonical HIP objects without executing product kernels or modifying the normal build.
# Inputs: Current real-GPU qualification and fresh checkout-owned output. Outputs: Complete development manifest.
def build(qualification: Path, output: Path) -> dict:
    tools, qualified = qualified_toolchain(qualification)
    output = coverage.owned_path(coverage.ROOT, output)
    output.mkdir(parents=True, exist_ok=False)
    frozen = {**qualified, **coverage.identities([qualification, *source_inputs(tools)])}
    admit_tool(tools["registration_tool"], output)
    objects, artifacts = {}, []
    for source in SOURCES:
        obj, generated = compile_object(tools, coverage.ROOT / source, output / Path(source).stem)
        objects[source] = str(obj)
        artifacts.extend(generated)
    require_identities(frozen)
    manifest = {
        "schema_version": 1,
        "scope": SCOPE,
        "root": str(coverage.ROOT),
        "architecture": tools["architecture"],
        "sdk": str(tools["sdk"]),
        "host_runtime": str(tools["host_runtime"]),
        "qualification": str(qualification.resolve()),
        "objects": objects,
        "inputs": frozen,
        "artifacts": coverage.identities(artifacts),
        "device_images_byte_identical": True,
    }
    (output / "product-objects.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


# Purpose: Admit exactly the current product objects before CMake may link the normal trusted kernel module.
# Inputs: Frozen development manifest plus canonical SDK/target. Outputs: Small validated CMake metadata.
def admit(manifest: Path, sdk: Path, target: str) -> dict:
    record = read_record(manifest)
    expected = {
        "schema_version",
        "scope",
        "root",
        "architecture",
        "sdk",
        "host_runtime",
        "qualification",
        "objects",
        "inputs",
        "artifacts",
        "device_images_byte_identical",
    }
    if (
        set(record) != expected
        or type(record.get("schema_version")) is not int
        or record["schema_version"] != 1
        or record["scope"] != SCOPE
        or record["root"] != str(coverage.ROOT)
        or record["architecture"] != coverage.architecture(target)
        or record["sdk"] != str(coverage.existing_input(sdk))
        or record["device_images_byte_identical"] is not True
        or not isinstance(record["objects"], dict)
        or set(record["objects"]) != set(SOURCES)
    ):
        raise ValueError("Product coverage manifest has a foreign or incomplete build scope")
    require_identities(record["inputs"])
    require_identities(record["artifacts"])
    if not isinstance(record["qualification"], str) or record["qualification"] not in record["inputs"]:
        raise ValueError("Product coverage lacks its source-bound GPU qualification")
    qualified_path = Path(record["qualification"])
    tools, qualified = qualified_toolchain(qualified_path)
    expected_inputs = {**qualified, **coverage.identities([qualified_path, *source_inputs(tools)])}
    if record["inputs"] != expected_inputs or record["host_runtime"] != str(tools["host_runtime"]):
        raise ValueError("Product coverage dependency inventory is incomplete")
    if record["sdk"] != str(tools["sdk"]) or record["architecture"] != tools["architecture"]:
        raise ValueError("Product coverage differs from its qualified toolchain")
    for source, obj in record["objects"].items():
        if (
            not isinstance(obj, str)
            or obj not in record["artifacts"]
            or str(coverage.ROOT / source) not in record["inputs"]
            or obj != str(manifest.parent.resolve() / Path(source).stem / "completed.obj")
        ):
            raise ValueError("Product coverage object lacks source/artifact identity")
    return {key: record[key] for key in ("objects", "host_runtime", "architecture")}


# Purpose: Expose separate object preparation and read-only build admission with explicit paths.
# Inputs: CLI arguments. Outputs: Small JSON metadata or failure; never GPU execution or normal-build mutation.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare = commands.add_parser("build")
    prepare.add_argument("--qualification", type=Path, required=True)
    prepare.add_argument("--output", type=Path, required=True)
    inspect = commands.add_parser("admit")
    inspect.add_argument("--manifest", type=Path, required=True)
    inspect.add_argument("--sdk", type=Path, required=True)
    inspect.add_argument("--arch", required=True)
    args = parser.parse_args()
    if args.command == "build":
        result = build(args.qualification, args.output)
        print(json.dumps({"scope": result["scope"], "objects": len(result["objects"])}))
    else:
        print(json.dumps(admit(args.manifest, args.sdk, args.arch)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
