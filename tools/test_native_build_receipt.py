"""Real-file build transaction regressions; fixture outputs are never benchmark evidence."""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import threading
import unittest
from pathlib import Path

from tools import native_build_receipt as receipt
from tools import test_native_build_provenance as input_fixtures
from tools.native_build_provenance import canonical


class NativeBuildReceiptTests(unittest.TestCase):
    # Purpose: Add isolated CMake/compiler/output bytes without executing a compiler or touching host software.
    # Inputs: Private Git fixture. Outputs: Actual production-reader metadata and bounded nonexecutable artifacts.
    def setUp(self):
        input_fixtures.NativeBuildInputsTests.setUp(self)
        self.configure_fixture()

    # Purpose: Populate a private fixture for both Python and PowerShell consumer regressions.
    # Inputs: Existing test checkout root.
    # Outputs: Nonexecutable output/compiler fixtures, never measurements.
    def configure_fixture(self):
        self.build = self.root / "build"
        self.build.mkdir(exist_ok=True)
        (self.build / receipt.LOCK).write_bytes(b"")
        self.compiler = self.root / "private compiler"
        self.compiler.mkdir()
        for name in ("cl.exe", "c1xx.dll", "c2.dll", "cmake.exe"):
            (self.compiler / name).write_bytes(name.encode("ascii"))
        self.cmake = self.compiler / "cmake.exe"
        self.abi_header = self.root / "share/cmake-4.4/Modules/CMakeCompilerABI.h"
        self.abi_header.parent.mkdir(parents=True)
        self.abi_header.write_bytes(b"bounded nonexecutable compiler ABI fixture")
        self.cache = {key: "" for key in receipt.RECIPE_KEYS}
        self.cache.update(
            CMAKE_GENERATOR="Visual Studio 18 2026",
            CMAKE_GENERATOR_PLATFORM="x64",
            SUPERZIP_ENABLE_HIP="OFF",
            SUPERZIP_HIP_ARCH="gfx1201",
            SUPERZIP_PACKAGE_VERSION="0.8.0",
            SUPERZIP_MSI_INSTALL_SCOPE="perMachine",
            SUPERZIP_BUILD_GUI="ON",
            SUPERZIP_BUILD_TESTS="ON",
            CMAKE_CACHE_MAJOR_VERSION="4",
            CMAKE_CACHE_MINOR_VERSION="4",
            CMAKE_CACHE_PATCH_VERSION="3",
        )
        self.write_cache()
        probe = self.build / "CMakeFiles/4.4.3/CMakeCXXCompiler.cmake"
        probe.parent.mkdir(parents=True)
        probe.write_text(
            f'set(CMAKE_CXX_COMPILER "{(self.compiler / "cl.exe").as_posix()}")\n'
            'set(CMAKE_CXX_COMPILER_ID "MSVC")\nset(CMAKE_CXX_COMPILER_VERSION "19.51.36260.0")\n',
            encoding="utf-8",
        )
        output = self.build / "Release"
        output.mkdir(exist_ok=True)
        for name in ("superzip_cli.exe", "SuperZip.exe", "fixture.dll"):
            (output / name).write_bytes(name.encode("ascii"))
        (self.build / "superzip-runtime-dependencies.json").write_text(
            json.dumps({"packaged_runtime_files": [{"name": "fixture.dll"}]}), encoding="utf-8"
        )

    # Purpose: Write the private configured cache. Inputs: Fixture values. Outputs: CMake-compatible regular file.
    def write_cache(self):
        (self.build / "CMakeCache.txt").write_text(
            "\n".join(f"{key}:STRING={value}" for key, value in self.cache.items()), encoding="utf-8"
        )

    # Purpose: Exercise the production start/prepare/finish path with explicit fresh-build acknowledgement.
    # Inputs: Private nonexecutable fixtures. Outputs: Completed successful receipt summary and transaction ID.
    def successful(self):
        token = receipt.begin(self.root, "Release")["transaction_id"]
        prepared = receipt.prepare(self.root, token, self.cmake)
        result = receipt.finish(self.root, token, self.cmake, prepared["clean_first_required"])
        return token, result

    # Purpose: Bind real source/output bytes without exposing compiler or checkout locations.
    # Inputs: Completed fixture invocation. Outputs: Requires independent hashes and current-source acceptance.
    def test_success_binds_content_and_rejects_hip_fallback(self):
        _, result = self.successful()
        self.assertEqual(
            result["receipt_sha256"], receipt.validate_current(self.root, "Release", False)["receipt_sha256"]
        )
        data = receipt.read_metadata(self.root, f"build/{receipt.RECEIPT}")
        output = self.build / "Release/superzip_cli.exe"
        self.assertEqual(
            data["outputs_sha256"]["build/Release/superzip_cli.exe"], hashlib.sha256(output.read_bytes()).hexdigest()
        )
        self.assertNotIn(str(self.root), canonical(data).decode("ascii"))
        self.assertNotIn(self.root.as_posix(), canonical(data).decode("ascii"))
        with self.assertRaisesRegex(ValueError, "CPU-only"):
            receipt.validate_current(self.root, "Release", True)
        with self.assertRaises(ValueError):
            receipt.validate_current(self.root, "Debug", False)

    # Purpose: Bind the tool source component and detect edits during a native build transaction.
    # Inputs: Private compiler ABI header mutated after source/recipe capture.
    # Outputs: Independent digest equality and rejection of changed tool inputs.
    def test_compiler_abi_source_is_bound_to_transaction(self):
        token = receipt.begin(self.root, "Release")["transaction_id"]
        prepared = receipt.prepare(self.root, token, self.cmake)
        state = receipt.read_metadata(self.root, f"build/{receipt.TRANSACTION}")
        self.assertEqual(
            state["toolchain"]["cmake_abi_header_sha256"], hashlib.sha256(self.abi_header.read_bytes()).hexdigest()
        )
        self.abi_header.write_bytes(self.abi_header.read_bytes() + b"changed ABI payload")
        with self.assertRaisesRegex(ValueError, "observed toolchain changed"):
            receipt.finish(self.root, token, self.cmake, prepared["clean_first_required"])

    # Purpose: Migrate exact narrower receipts without accepting them as current build evidence.
    # Inputs: Valid v1 toolchain metadata and malformed sibling controls.
    # Outputs: Requires clean-first, preserves old bytes, rejects current acceptance and unknown metadata.
    def test_historical_toolchain_requires_rebuild_and_retains_evidence(self):
        self.successful()
        old = receipt.read_metadata(self.root, f"build/{receipt.RECEIPT}")
        old["toolchain"].pop("cmake_abi_header_sha256")
        old["toolchain"]["scope"] = "cmake-msvc-probe-and-critical-compiler-files-v1"
        with self.assertRaises(ValueError):
            receipt.validate_receipt(old)
        receipt.validate_receipt(old, allow_historical_toolchain=True)
        bad = json.loads(json.dumps(old))
        bad["toolchain"]["unexpected"] = "unknown"
        with self.assertRaises(ValueError):
            receipt.validate_receipt(bad, allow_historical_toolchain=True)
        bad = json.loads(json.dumps(old))
        bad["toolchain"]["cmake_sha256"] = "malformed"
        with self.assertRaises(ValueError):
            receipt.validate_receipt(bad, allow_historical_toolchain=True)
        receipt.publish(self.root, receipt.RECEIPT, old)
        old_bytes = (self.build / receipt.RECEIPT).read_bytes()
        token = receipt.begin(self.root, "Release")["transaction_id"]
        prepared = receipt.prepare(self.root, token, self.cmake)
        self.assertTrue(prepared["clean_first_required"])
        with self.assertRaisesRegex(ValueError, "fresh build"):
            receipt.finish(self.root, token, self.cmake, False)
        receipt.finish(self.root, token, self.cmake, True)
        archive = self.build / "native-build-history" / (hashlib.sha256(old_bytes).hexdigest() + ".json")
        self.assertEqual(old_bytes, archive.read_bytes())

    # Purpose: Refuse absent and incomplete builds while preserving a previous successful receipt on failed attempts.
    # Inputs: New, unfinished, configure-only and failed transactions. Outputs: No stale successful acceptance.
    def test_terminal_states_preserve_history(self):
        with self.assertRaises(FileNotFoundError):
            receipt.validate_current(self.root, "Release", False)
        self.successful()
        original = (self.build / receipt.RECEIPT).read_bytes()
        for status in ("configure-only", "failed"):
            token = receipt.begin(self.root, "Release")["transaction_id"]
            with self.assertRaisesRegex(ValueError, "incomplete"):
                receipt.validate_current(self.root, "Release", False)
            receipt.end_incomplete(self.root, token, status)
            with self.assertRaisesRegex(ValueError, "incomplete"):
                receipt.validate_current(self.root, "Release", False)
            self.assertEqual(original, (self.build / receipt.RECEIPT).read_bytes())
        self.successful()
        archive = self.build / "native-build-history" / (hashlib.sha256(original).hexdigest() + ".json")
        self.assertEqual(original, archive.read_bytes())

    # Purpose: Reject edits after successful build, including a second edit within an already dirty checkout.
    # Inputs: Mutated source, addition, runtime, GUI, CLI and configured option.
    # Outputs: Current validation fails on each mutation.
    def test_stale_content_and_configuration(self):
        self.successful()
        for name in (
            "src/main.cpp",
            "build/Release/SuperZip.exe",
            "build/Release/superzip_cli.exe",
            "build/Release/fixture.dll",
        ):
            path = self.root / name
            old = path.read_bytes()
            path.write_bytes(old + b"mutation")
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "changed"):
                receipt.validate_current(self.root, "Release", False)
            path.write_bytes(old)
        (self.root / "src/new.cpp").write_text("new source", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "changed"):
            receipt.validate_current(self.root, "Release", False)
        self.cache["SUPERZIP_PACKAGE_VERSION"] = "0.9.0"
        self.write_cache()
        with self.assertRaisesRegex(ValueError, "configuration"):
            receipt.validate_current(self.root, "Release", False)

    # Purpose: Require fresh initial/toolchain builds while retaining content-sensitive incremental source builds.
    # Inputs: Initial invocation, source edit and changed compiler bytes. Outputs: Correct clean-first decisions.
    def test_fresh_build_and_incremental_policy(self):
        token = receipt.begin(self.root, "Release")["transaction_id"]
        self.assertTrue(receipt.prepare(self.root, token, self.cmake)["clean_first_required"])
        with self.assertRaisesRegex(ValueError, "fresh build"):
            receipt.finish(self.root, token, self.cmake, False)
        receipt.finish(self.root, token, self.cmake, True)
        (self.root / "src/main.cpp").write_text("updated source", encoding="utf-8")
        token = receipt.begin(self.root, "Release")["transaction_id"]
        self.assertFalse(receipt.prepare(self.root, token, self.cmake)["clean_first_required"])
        receipt.finish(self.root, token, self.cmake, False)
        (self.compiler / "cl.exe").write_bytes(b"whole-toolchain update fixture")
        token = receipt.begin(self.root, "Release")["transaction_id"]
        self.assertTrue(receipt.prepare(self.root, token, self.cmake)["clean_first_required"])

    # Purpose: Refuse source/compiler/configuration changes during an invocation without publishing success.
    # Inputs: Changed frozen source or observed tool bytes. Outputs: Failed completion and preserved old receipt.
    def test_mid_build_changes(self):
        self.successful()
        original = (self.build / receipt.RECEIPT).read_bytes()
        token = receipt.begin(self.root, "Release")["transaction_id"]
        receipt.prepare(self.root, token, self.cmake)
        (self.compiler / "c2.dll").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "toolchain changed"):
            receipt.finish(self.root, token, self.cmake, False)
        self.assertEqual(original, (self.build / receipt.RECEIPT).read_bytes())
        token = receipt.begin(self.root, "Release")["transaction_id"]
        (self.root / "src/main.cpp").write_text("changed during configure", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "during configure"):
            receipt.prepare(self.root, token, self.cmake)

    # Purpose: Keep immutable evidence on a history collision and reject malformed/path-bearing metadata.
    # Inputs: Colliding history, duplicate fields and unsafe configured paths.
    # Outputs: Fails closed without overwrites.
    def test_collisions_and_malformed_metadata(self):
        self.successful()
        original = (self.build / receipt.RECEIPT).read_bytes()
        archive = self.build / "native-build-history" / (hashlib.sha256(original).hexdigest() + ".json")
        archive.write_bytes(b"collision")
        with self.assertRaisesRegex(ValueError, "history collision"):
            receipt.publish(self.root, receipt.RECEIPT, {"replacement": True})
        self.assertEqual(original, (self.build / receipt.RECEIPT).read_bytes())
        (self.build / "bad.json").write_text('{"status":1,"status":2}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            receipt.read_metadata(self.root, "build/bad.json")
        self.cache["SUPERZIP_HIP_ARCH"] = "C:/private/location"
        self.write_cache()
        with self.assertRaises(ValueError):
            receipt.configured_recipe(self.root, "Release")

    # Purpose: Prove the actual Windows sharing lock excludes consumers without using PID or stale markers as liveness.
    # Inputs: A private PowerShell-owned exclusive file handle.
    # Outputs: Reader failure while held and acceptance after release.
    @unittest.skipUnless(os.name == "nt", "Windows sharing semantics")
    def test_active_tree_lock(self):
        self.successful()
        script = self.build / "lock-fixture.ps1"
        script.write_text(
            "param([string]$LockPath)\n$ErrorActionPreference='Stop'\n"
            "$f=[IO.FileStream]::new($LockPath,[IO.FileMode]::Open,"
            "[IO.FileAccess]::ReadWrite,[IO.FileShare]::None);\n"
            "try { 'locked'; [Console]::ReadLine() | Out-Null } finally { $f.Dispose() }",
            encoding="utf-8",
        )
        process = subprocess.Popen(
            [
                "powershell",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(script),
                str(self.build / receipt.LOCK),
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        timer = threading.Timer(15, process.kill)
        timer.start()
        try:
            self.assertEqual("locked", process.stdout.readline().strip())
            with self.assertRaises(PermissionError):
                receipt.validate_current(self.root, "Release", False)
            process.communicate("release\n", timeout=10)
            self.assertEqual(0, process.returncode)
        finally:
            timer.cancel()
            timer.join()
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=10)
        receipt.validate_current(self.root, "Release", False)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--create-fixture":
        case = NativeBuildReceiptTests()
        case.root = Path(sys.argv[2])
        case.configure_fixture()
        case.successful()
    else:
        unittest.main()
