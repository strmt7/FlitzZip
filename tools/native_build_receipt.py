"""Successful native invocation receipts; local consistency evidence, not authenticated attestation."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import tempfile
import uuid
from pathlib import Path, PurePosixPath

try:
    from tools.native_build_provenance import (
        INPUT_FILES,
        INPUT_ROOTS,
        ROOT,
        canonical,
        capture_inputs,
        file_digest,
        input_path,
    )
except ModuleNotFoundError:
    from native_build_provenance import (
        INPUT_FILES,
        INPUT_ROOTS,
        ROOT,
        canonical,
        capture_inputs,
        file_digest,
        input_path,
    )

MAX_METADATA = 5 * 1024**2
CONFIGURATIONS = ("Debug", "Release", "RelWithDebInfo")
RECEIPT = "native-build-receipt.json"
TRANSACTION = "native-build-transaction.json"
LOCK = "native-build.lock"
SHA256 = r"[0-9a-f]{64}"
RECIPE_KEYS = (
    "CMAKE_GENERATOR",
    "CMAKE_GENERATOR_PLATFORM",
    "SUPERZIP_ENABLE_HIP",
    "SUPERZIP_HIP_ARCH",
    "SUPERZIP_VCVARS_VERSION",
    "SUPERZIP_PACKAGE_VERSION",
    "SUPERZIP_MSI_INSTALL_SCOPE",
    "SUPERZIP_BUILD_GUI",
    "SUPERZIP_BUILD_TESTS",
)


# Purpose: Validate portable digest maps without consulting a filesystem or accepting private/unsafe names.
# Inputs: Untrusted mapping, maximum count and optional required prefix. Outputs: Valid map or exception.
def validate_hash_map(value: object, maximum: int, prefix: str = "") -> dict:
    if not isinstance(value, dict) or not value or len(value) > maximum:
        raise ValueError("invalid native receipt hash inventory")
    for name, digest in value.items():
        if (
            not isinstance(name, str)
            or not name
            or name == "."
            or PurePosixPath(name).is_absolute()
            or PurePosixPath(name).as_posix() != name
            or "\\" in name
            or ":" in name
            or any(part in (".", "..", ".git") for part in PurePosixPath(name).parts)
            or prefix
            and not name.startswith(prefix)
            or not isinstance(digest, str)
            or not re.fullmatch(SHA256, digest)
        ):
            raise ValueError("native receipt contains an unsafe filename or digest")
    return value


# Purpose: Share strict receipt schema validation between local consumers and offline graph publication.
# Inputs: Untrusted metadata; explicit historical mode reads existing evidence without qualifying current builds.
# Outputs: Canonical digest; current consumers reject narrower historical toolchain scopes.
def validate_receipt(value: dict, *, allow_historical_toolchain: bool = False) -> str:
    keys = {
        "schema_version",
        "kind",
        "transaction_id",
        "status",
        "inputs",
        "recipe",
        "toolchain",
        "clean_first",
        "outputs_sha256",
    }
    if not isinstance(value, dict) or set(value) != keys or type(value["schema_version"]) is not int:
        raise ValueError("invalid native receipt schema")
    if (
        value["schema_version"] != 1
        or value["kind"] != "successful-native-invocation-v1"
        or value["status"] != "successful"
    ):
        raise ValueError("unsupported native invocation receipt")
    if not isinstance(value["transaction_id"], str) or not re.fullmatch(r"[0-9a-f]{32}", value["transaction_id"]):
        raise ValueError("invalid native transaction identity")
    if type(value["clean_first"]) is not bool:
        raise ValueError("invalid native clean-first evidence")
    inputs = value["inputs"]
    if not isinstance(inputs, dict) or set(inputs) != {"schema_version", "projection", "files", "inputs_sha256"}:
        raise ValueError("invalid native input manifest schema")
    if (
        type(inputs["schema_version"]) is not int
        or inputs["schema_version"] != 1
        or inputs["projection"] != "native-invocation-inputs-v1"
    ):
        raise ValueError("unsupported native input manifest")
    files = validate_hash_map(inputs["files"], 20000)
    if any(
        name not in INPUT_FILES and not any(name.startswith(prefix + "/") for prefix in INPUT_ROOTS) for name in files
    ):
        raise ValueError("native input manifest exceeds its declared projection")
    if "CMakeLists.txt" not in files or hashlib.sha256(canonical(files)).hexdigest() != inputs["inputs_sha256"]:
        raise ValueError("native input manifest digest is inconsistent")
    recipe = value["recipe"]
    if not isinstance(recipe, dict) or set(recipe) != {*RECIPE_KEYS, "configuration", "configured_flags_sha256"}:
        raise ValueError("invalid native configured recipe schema")
    if any(not isinstance(item, str) or "/" in item or "\\" in item for item in recipe.values()):
        raise ValueError("native recipe contains invalid or private metadata")
    if (
        recipe["configuration"] not in CONFIGURATIONS
        or recipe["SUPERZIP_ENABLE_HIP"] not in ("ON", "OFF")
        or recipe["CMAKE_GENERATOR"] not in ("Visual Studio 17 2022", "Visual Studio 18 2026")
        or recipe["CMAKE_GENERATOR_PLATFORM"] != "x64"
        or recipe["SUPERZIP_BUILD_GUI"] != "ON"
        or recipe["SUPERZIP_BUILD_TESTS"] != "ON"
        or not re.fullmatch(r"gfx[0-9a-f]+(?:,gfx[0-9a-f]+)*", recipe["SUPERZIP_HIP_ARCH"])
        or not re.fullmatch(SHA256, recipe["configured_flags_sha256"])
    ):
        raise ValueError("invalid native recipe scope")
    validate_toolchain_schema(
        value["toolchain"], recipe["SUPERZIP_ENABLE_HIP"] == "ON", allow_historical=allow_historical_toolchain
    )
    outputs = validate_hash_map(value["outputs_sha256"], 256, "build/")
    required = {
        f"build/{recipe['configuration']}/superzip_cli.exe",
        f"build/{recipe['configuration']}/SuperZip.exe",
        "build/superzip-runtime-dependencies.json",
    }
    if not required <= outputs.keys():
        raise ValueError("native receipt lacks required outputs")
    prefix = f"build/{recipe['configuration']}/"
    if any(
        name not in required and not re.fullmatch(re.escape(prefix) + r"[A-Za-z0-9_.-]+\.dll", name) for name in outputs
    ):
        raise ValueError("native receipt output exceeds the app-local artifact scope")
    return hashlib.sha256(canonical(value)).hexdigest()


# Purpose: Reject unsupported or path-bearing observed toolchain fields in portable receipts.
# Inputs: Untrusted toolchain, required HIP flag and explicit historical-only mode.
# Outputs: Validates an exact current or admitted historical schema; rejects unknown fields and scopes.
def validate_toolchain_schema(value: object, hip: bool, *, allow_historical: bool = False) -> None:
    historical = (
        allow_historical
        and isinstance(value, dict)
        and value.get("scope") == "cmake-msvc-probe-and-critical-compiler-files-v1"
    )
    keys = {
        "scope",
        "cmake_version",
        "cmake_sha256",
        "cmake_abi_header_sha256",
        "host_compiler_id",
        "host_compiler_version",
        "host_compiler_files_sha256",
    }
    if hip:
        keys.add("hip_sdk")
    if historical:
        keys.remove("cmake_abi_header_sha256")
    if not isinstance(value, dict) or set(value) != keys:
        raise ValueError("invalid native toolchain schema")
    if (
        value["scope"]
        != (
            "cmake-msvc-probe-and-critical-compiler-files-v1"
            if historical
            else "cmake-msvc-abi-probe-and-critical-compiler-files-v2"
        )
        or value["host_compiler_id"] != "MSVC"
        or not isinstance(value["cmake_version"], str)
        or not re.fullmatch(r"\d+\.\d+\.\d+", value["cmake_version"])
        or not isinstance(value["host_compiler_version"], str)
        or not re.fullmatch(r"[0-9.]+", value["host_compiler_version"])
        or not isinstance(value["cmake_sha256"], str)
        or not re.fullmatch(SHA256, value["cmake_sha256"])
        or (
            not historical
            and (
                not isinstance(value["cmake_abi_header_sha256"], str)
                or not re.fullmatch(SHA256, value["cmake_abi_header_sha256"])
            )
        )
        or set(validate_hash_map(value["host_compiler_files_sha256"], 3)) != {"cl.exe", "c1xx.dll", "c2.dll"}
    ):
        raise ValueError("invalid observed native compiler scope")
    if hip:
        sdk = value["hip_sdk"]
        if not isinstance(sdk, dict) or set(sdk) != {"scope", "lock_sha256", "critical_files_sha256"}:
            raise ValueError("invalid HIP SDK scope")
        if (
            sdk["scope"] != "lock-plus-hipcc-clang-import-version-and-device-bitcode-v1"
            or not isinstance(sdk["lock_sha256"], str)
            or not re.fullmatch(SHA256, sdk["lock_sha256"])
        ):
            raise ValueError("invalid HIP SDK lock identity")
        required = {
            "bin/hipcc.exe",
            "lib/llvm/bin/clang.exe",
            "lib/amdhip64.lib",
            "include/hip/hip_version.h",
            "lib/llvm/amdgcn/bitcode/ocml.bc",
        }
        if not required <= validate_hash_map(sdk["critical_files_sha256"], 1000).keys():
            raise ValueError("native HIP receipt lacks critical SDK files")


# Purpose: Read bounded regular JSON without accepting duplicate properties or nonfinite numbers.
# Inputs: Trusted checkout root and relative metadata filename. Outputs: Dictionary or fail-closed exception.
def read_metadata(root: Path, name: str) -> dict:
    path = input_path(root, name)
    if path.stat().st_size > MAX_METADATA:
        raise ValueError("native build metadata exceeds its bound")

    # Purpose: Reject duplicate JSON fields. Inputs: Parsed pairs. Outputs: Unique dictionary or error.
    def unique(pairs: list) -> dict:
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate native build metadata property")
            result[key] = value
        return result

    value = json.loads(path.read_bytes(), object_pairs_hook=unique)
    if not isinstance(value, dict):
        raise ValueError("native build metadata must be an object")
    canonical(value)
    return value


# Purpose: Atomically publish owned metadata while rejecting linked destinations and retaining replaced evidence.
# Inputs: Checkout, owned build-relative name and bounded JSON value.
# Outputs: Complete file or exception; replaced evidence remains immutable.
def publish(root: Path, name: str, value: dict) -> None:
    data = canonical(value) + b"\n"
    if len(data) > MAX_METADATA:
        raise ValueError("native build metadata exceeds its bound")
    build = root / "build"
    build.mkdir(exist_ok=True)
    reject_link(build)
    path = build / name
    if path.exists() or path.is_symlink():
        previous = input_path(root, f"build/{name}")
        if previous.stat().st_size > MAX_METADATA:
            raise ValueError("previous native build metadata exceeds its bound")
        old = previous.read_bytes()
        history = build / "native-build-history"
        history.mkdir(exist_ok=True)
        reject_link(history)
        archive = history / (hashlib.sha256(old).hexdigest() + ".json")
        try:
            with archive.open("xb") as stream:
                stream.write(old)
                stream.flush()
                os.fsync(stream.fileno())
        except FileExistsError:
            archived = input_path(root, archive.relative_to(root).as_posix())
            if archived.stat().st_size != len(old) or file_digest(archived) != hashlib.sha256(old).hexdigest():
                raise ValueError("native build history collision; previous evidence preserved") from None
    descriptor, temporary = tempfile.mkstemp(prefix=".native-build-", suffix=".tmp", dir=build)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


# Purpose: Reject linked metadata directories. Inputs: Existing directory. Outputs: Exception on links/reparse points.
def reject_link(path: Path) -> None:
    info = path.lstat()
    if path.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400 or not path.is_dir():
        raise ValueError("native build directory must be an unlinked directory")


# Purpose: Invalidate acceptance before source capture or configure work can fail.
# Inputs: Checkout and requested configuration. Outputs: New incomplete transaction with frozen native inputs.
def begin(root: Path, configuration: str) -> dict:
    if configuration not in CONFIGURATIONS:
        raise ValueError("unsupported native build configuration")
    state = {
        "schema_version": 1,
        "transaction_id": uuid.uuid4().hex,
        "status": "incomplete",
        "configuration": configuration,
    }
    publish(root, TRANSACTION, state)
    state["inputs"] = capture_inputs(root)
    publish(root, TRANSACTION, state)
    return {"transaction_id": state["transaction_id"]}


# Purpose: Require the exact incomplete invocation before mutating its state.
# Inputs: Checkout and caller-owned transaction ID. Outputs: Matching transaction or exception.
def pending(root: Path, token: str) -> dict:
    state = read_metadata(root, f"build/{TRANSACTION}")
    if (
        not re.fullmatch(r"[0-9a-f]{32}", token)
        or state.get("transaction_id") != token
        or state.get("status") != "incomplete"
    ):
        raise ValueError("native build transaction is unavailable, replaced or no longer incomplete")
    return state


# Purpose: Observe the configured invocation without exporting installation paths.
# Inputs: Checkout CMake cache. Outputs: Portable recipe and private cache values for scoped tool inspection.
def configured_recipe(root: Path, configuration: str) -> tuple[dict, dict]:
    if configuration not in CONFIGURATIONS:
        raise ValueError("unsupported native build configuration")
    cache_path = input_path(root, "build/CMakeCache.txt")
    if cache_path.stat().st_size > MAX_METADATA:
        raise ValueError("CMake cache exceeds its metadata bound")
    cache = {}
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([^#/:][^:]*):[^=]+=(.*)", line)
        if match:
            cache[match[1]] = match[2]
    if any(key not in cache for key in RECIPE_KEYS):
        raise ValueError("CMake cache lacks required native build options")
    recipe = {key: cache[key] for key in RECIPE_KEYS}
    recipe["configuration"] = configuration
    if recipe["CMAKE_GENERATOR"] not in ("Visual Studio 17 2022", "Visual Studio 18 2026"):
        raise ValueError("native receipt requires a supported Windows generator")
    if recipe["CMAKE_GENERATOR_PLATFORM"] != "x64" or recipe["SUPERZIP_ENABLE_HIP"] not in ("ON", "OFF"):
        raise ValueError("invalid native platform or HIP scope")
    if recipe["SUPERZIP_BUILD_GUI"] != "ON" or recipe["SUPERZIP_BUILD_TESTS"] != "ON":
        raise ValueError("native receipt requires the complete app and test build")
    if not re.fullmatch(r"gfx[0-9a-f]+(?:,gfx[0-9a-f]+)*", recipe["SUPERZIP_HIP_ARCH"]):
        raise ValueError("native receipt requires concrete HIP architecture targets")
    if any("/" in value or "\\" in value for value in recipe.values()):
        raise ValueError("configured recipe must not contain installation paths")
    flags = {
        key: value
        for key, value in cache.items()
        if key.startswith(("CMAKE_CXX_FLAGS", "CMAKE_C_FLAGS", "CMAKE_EXE_LINKER_FLAGS", "CMAKE_SHARED_LINKER_FLAGS"))
        or key
        in (
            "CMAKE_MSVC_RUNTIME_LIBRARY",
            "CMAKE_GENERATOR_TOOLSET",
            "CMAKE_CONFIGURATION_TYPES",
            "CMAKE_DISABLE_PRECOMPILE_HEADERS",
        )
    }
    recipe["configured_flags_sha256"] = hashlib.sha256(canonical(flags)).hexdigest()
    return recipe, cache


# Purpose: Hash a narrow, explicit compiler scope without claiming whole installation attestation.
# Inputs: Configured checkout, private cache and verified CMake executable.
# Outputs: Relative component hashes and compiler-probe metadata.
def observe_toolchain(root: Path, cache: dict, cmake: Path) -> dict:
    version = ".".join(cache[f"CMAKE_CACHE_{part}_VERSION"] for part in ("MAJOR", "MINOR", "PATCH"))
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError("invalid CMake probe version")
    probe = input_path(root, f"build/CMakeFiles/{version}/CMakeCXXCompiler.cmake")
    if probe.stat().st_size > MAX_METADATA:
        raise ValueError("CMake compiler probe exceeds its bound")
    text = probe.read_text(encoding="utf-8")
    values = {}
    for field in ("CMAKE_CXX_COMPILER", "CMAKE_CXX_COMPILER_ID", "CMAKE_CXX_COMPILER_VERSION"):
        match = re.search(r"set\(" + field + r' "([^"\r\n]*)"\)', text)
        if not match:
            raise ValueError("CMake compiler probe lacks required metadata")
        values[field] = match[1]
    if values["CMAKE_CXX_COMPILER_ID"] != "MSVC" or not re.fullmatch(r"[0-9.]+", values["CMAKE_CXX_COMPILER_VERSION"]):
        raise ValueError("native receipt requires the observed MSVC compiler probe")
    compiler = Path(values["CMAKE_CXX_COMPILER"])
    host_files = {
        name: file_digest(input_path(compiler.parent, name)) for name in (compiler.name, "c1xx.dll", "c2.dll")
    }
    toolchain = {
        "scope": "cmake-msvc-abi-probe-and-critical-compiler-files-v2",
        "cmake_version": version,
        "cmake_sha256": file_digest(input_path(cmake.parent, cmake.name)),
        "cmake_abi_header_sha256": file_digest(
            input_path(
                cmake.parent.parent, f"share/cmake-{'.'.join(version.split('.')[:2])}/Modules/CMakeCompilerABI.h"
            )
        ),
        "host_compiler_id": values["CMAKE_CXX_COMPILER_ID"],
        "host_compiler_version": values["CMAKE_CXX_COMPILER_VERSION"],
        "host_compiler_files_sha256": host_files,
    }
    if cache["SUPERZIP_ENABLE_HIP"] == "ON":
        sdk = Path(cache["SUPERZIP_HIP_PATH"])
        names = ("bin/hipcc.exe", "lib/llvm/bin/clang.exe", "lib/amdhip64.lib", "include/hip/hip_version.h")
        bitcode = input_path(sdk, "lib/llvm/amdgcn/bitcode/ocml.bc").parent
        names = sorted(set(names) | {path.relative_to(sdk).as_posix() for path in bitcode.glob("*.bc")})
        if len(names) > 1000:
            raise ValueError("critical SDK inventory exceeds its bound")
        toolchain["hip_sdk"] = {
            "scope": "lock-plus-hipcc-clang-import-version-and-device-bitcode-v1",
            "lock_sha256": file_digest(input_path(root, "tools/rocm-sdk-lock.json")),
            "critical_files_sha256": {name: file_digest(input_path(sdk, name)) for name in names},
        }
    return toolchain


# Purpose: Freeze configured tool bytes and decide whether existing objects need a fresh build.
# Inputs: Owned pending transaction and selected CMake executable.
# Outputs: Required clean-first decision and stored observations.
def prepare(root: Path, token: str, cmake: Path) -> dict:
    state = pending(root, token)
    if state.get("inputs") != capture_inputs(root):
        raise ValueError("native inputs changed during configure")
    recipe, cache = configured_recipe(root, state["configuration"])
    toolchain = observe_toolchain(root, cache, cmake)
    fresh = True
    previous_path = root / "build" / RECEIPT
    if previous_path.exists():
        previous = read_metadata(root, f"build/{RECEIPT}")
        # Exact historical schemas may inform rebuild decisions, never current
        # acceptance. Their narrower toolchain always requires clean-first.
        validate_receipt(previous, allow_historical_toolchain=True)
        try:
            current_outputs = capture_outputs(root, state["configuration"])
        except FileNotFoundError:
            current_outputs = None
        fresh = (
            previous.get("status") != "successful"
            or previous.get("kind") != "successful-native-invocation-v1"
            or previous.get("recipe") != recipe
            or previous.get("toolchain") != toolchain
            or previous.get("outputs_sha256") != current_outputs
        )
    state.update(recipe=recipe, toolchain=toolchain, clean_first_required=fresh)
    publish(root, TRANSACTION, state)
    return {"clean_first_required": fresh}


# Purpose: Bind actual GUI, CLI, runtime manifest and all app-local DLL contents.
# Inputs: Checkout and configuration. Outputs: Portable output hashes; missing, linked or inconsistent outputs fail.
def capture_outputs(root: Path, configuration: str) -> dict:
    if configuration not in CONFIGURATIONS:
        raise ValueError("unsupported native output configuration")
    prefix = f"build/{configuration}"
    names = {f"{prefix}/superzip_cli.exe", f"{prefix}/SuperZip.exe", "build/superzip-runtime-dependencies.json"}
    runtime = read_metadata(root, "build/superzip-runtime-dependencies.json")
    dependencies = runtime.get("packaged_runtime_files")
    if not isinstance(dependencies, list) or not dependencies:
        raise ValueError("native runtime manifest lacks packaged dependencies")
    for item in dependencies:
        name = item.get("name") if isinstance(item, dict) else None
        if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+\.dll", name):
            raise ValueError("invalid native runtime dependency filename")
        names.add(f"{prefix}/{name}")
    names.update(path.relative_to(root).as_posix() for path in (root / prefix).glob("*.dll"))
    if len(names) > 256:
        raise ValueError("native output inventory exceeds its bound")
    return {name: file_digest(input_path(root, name)) for name in sorted(names)}


# Purpose: Publish success only after a successful caller build and unchanged configured inputs/toolchain.
# Inputs: Owned token, selected CMake and whether clean-first was requested.
# Outputs: Atomic receipt and completed state, or fail-closed exception.
def finish(root: Path, token: str, cmake: Path, clean_first: bool) -> dict:
    state = pending(root, token)
    if "recipe" not in state or state.get("clean_first_required") and not clean_first:
        raise ValueError("native build lacks preparation or its required fresh build")
    recipe, cache = configured_recipe(root, state["configuration"])
    if recipe != state["recipe"] or observe_toolchain(root, cache, cmake) != state["toolchain"]:
        raise ValueError("native build configuration or observed toolchain changed")
    outputs = capture_outputs(root, state["configuration"])
    if capture_inputs(root) != state["inputs"]:
        raise ValueError("native inputs changed during build; no successful receipt published")
    receipt = {
        "schema_version": 1,
        "kind": "successful-native-invocation-v1",
        "transaction_id": token,
        "status": "successful",
        "inputs": state["inputs"],
        "recipe": recipe,
        "toolchain": state["toolchain"],
        "clean_first": clean_first,
        "outputs_sha256": outputs,
    }
    validate_receipt(receipt)
    publish(root, RECEIPT, receipt)
    state.update(status="successful", receipt_sha256=hashlib.sha256(canonical(receipt)).hexdigest())
    publish(root, TRANSACTION, state)
    return {"receipt_sha256": state["receipt_sha256"], "inputs_sha256": receipt["inputs"]["inputs_sha256"]}


# Purpose: Record failure or configure-only status without accepting a previous successful build.
# Inputs: Owned token and explicit terminal status. Outputs: Atomic nonaccepted transaction; preserves previous receipt.
def end_incomplete(root: Path, token: str, status: str) -> None:
    if status not in ("failed", "configure-only"):
        raise ValueError("unsupported incomplete build terminal status")
    state = pending(root, token)
    state["status"] = status
    publish(root, TRANSACTION, state)


# Purpose: Verify current source/output/configuration against one completed native build.
# Inputs: Checkout, configuration and whether HIP is required.
# Outputs: Portable receipt summary; rejects active or stale evidence.
def validate_current(root: Path, configuration: str, require_hip: bool) -> dict:
    # Windows FileShare.None held by build.ps1 rejects this open throughout configure/build/publication.
    with input_path(root, f"build/{LOCK}").open("rb"):
        state = read_metadata(root, f"build/{TRANSACTION}")
        receipt = read_metadata(root, f"build/{RECEIPT}")
        digest = validate_receipt(receipt)
        if state.get("status") != "successful" or receipt.get("status") != "successful":
            raise ValueError("native build is active, failed or incomplete")
        if state.get("transaction_id") != receipt.get("transaction_id") or state.get("receipt_sha256") != digest:
            raise ValueError("native build receipt does not match the completed transaction")
        recipe, _ = configured_recipe(root, configuration)
        if recipe != receipt.get("recipe"):
            raise ValueError("native build receipt configuration is stale")
        if require_hip and recipe["SUPERZIP_ENABLE_HIP"] != "ON":
            raise ValueError("required-HIP measurement cannot use a CPU-only build receipt")
        if capture_inputs(root) != receipt.get("inputs") or capture_outputs(root, configuration) != receipt.get(
            "outputs_sha256"
        ):
            raise ValueError("native build inputs or outputs changed; rebuild before measurement")
        return {
            "receipt_sha256": digest,
            "inputs_sha256": receipt["inputs"]["inputs_sha256"],
            "recipe": recipe,
            "receipt": receipt,
        }


# Purpose: Bridge Windows build/benchmark tools to one receipt implementation.
# Inputs: Explicit operation/root/configuration/token/CMake options.
# Outputs: Bounded JSON summary or nonzero actionable failure.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("begin", "prepare", "finish", "failed", "configure-only", "validate"))
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--configuration", choices=CONFIGURATIONS, default="Release")
    parser.add_argument("--token", default="")
    parser.add_argument("--cmake", type=Path)
    parser.add_argument("--clean-first", action="store_true")
    parser.add_argument("--require-hip", action="store_true")
    args = parser.parse_args()
    if args.operation in ("prepare", "finish") and args.cmake is None:
        parser.error("configured receipt operations require --cmake")
    if args.operation == "begin":
        result = begin(args.root, args.configuration)
    elif args.operation == "prepare":
        result = prepare(args.root, args.token, args.cmake)
    elif args.operation == "finish":
        result = finish(args.root, args.token, args.cmake, args.clean_first)
    elif args.operation == "validate":
        result = validate_current(args.root, args.configuration, args.require_hip)
    else:
        end_incomplete(args.root, args.token, args.operation)
        result = {"status": args.operation}
    print(canonical(result).decode("ascii"))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, KeyError, TimeoutError) as error:
        cause = type(error).__name__ if isinstance(error, OSError) else str(error)
        print(f"native_build_receipt failed: {cause}", file=sys.stderr)
        raise SystemExit(1) from None
