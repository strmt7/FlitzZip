"""Real-file build transaction regressions; fixture outputs are never benchmark evidence."""

from __future__ import annotations

import copy
import hashlib
import json
import os
import subprocess
import sys
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

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
            FLITZZIP_ENABLE_HIP="OFF",
            FLITZZIP_HIP_ARCH="gfx1201",
            FLITZZIP_PACKAGE_VERSION="0.8.0",
            FLITZZIP_MSI_INSTALL_SCOPE="perMachine",
            FLITZZIP_BUILD_GUI="ON",
            FLITZZIP_BUILD_TESTS="ON",
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
        for configuration in receipt.CONFIGURATIONS:
            output = self.build / configuration
            output.mkdir(exist_ok=True)
            for name in ("flitzzip_cli.exe", "FlitzZip.exe", "fixture.dll"):
                (output / name).write_bytes(f"{configuration}:{name}".encode("ascii"))
            (output / "flitzzip-runtime-dependencies.json").write_text(
                json.dumps(
                    {
                        "packaged_runtime_files": [
                            {
                                "name": "fixture.dll",
                                "sha256": hashlib.sha256((output / "fixture.dll").read_bytes()).hexdigest(),
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )

    # Purpose: Write the private configured cache. Inputs: Fixture values. Outputs: CMake-compatible regular file.
    def write_cache(self):
        (self.build / "CMakeCache.txt").write_text(
            "\n".join(f"{key}:STRING={value}" for key, value in self.cache.items()), encoding="utf-8"
        )

    # Purpose: Prevent a receipt from qualifying a build with a different forced-header strategy.
    # Inputs: Successful nonexecutable fixture followed by a changed CMake PCH setting.
    # Outputs: Old qualification fails and the next build requires fresh compilation.
    def test_precompiled_header_strategy_invalidates_previous_qualification(self):
        self.cache["CMAKE_DISABLE_PRECOMPILE_HEADERS"] = "FALSE"
        self.write_cache()
        self.successful()
        self.cache["CMAKE_DISABLE_PRECOMPILE_HEADERS"] = "TRUE"
        self.write_cache()
        with self.assertRaisesRegex(ValueError, "configuration is stale"):
            receipt.validate_current(self.root, "Release", False)
        token = receipt.begin(self.root, "Release")["transaction_id"]
        self.assertTrue(receipt.prepare(self.root, token, self.cmake)["clean_first_required"])

    # Purpose: Exercise the production start/prepare/finish path with explicit fresh-build acknowledgement.
    # Inputs: Private nonexecutable fixtures and configuration. Outputs: Successful receipt and transaction ID.
    def successful(self, configuration="Release"):
        token = receipt.begin(self.root, configuration)["transaction_id"]
        prepared = receipt.prepare(self.root, token, self.cmake)
        result = receipt.finish(self.root, token, self.cmake, prepared["clean_first_required"])
        return token, result

    # Purpose: Avoid unnecessary clean builds when returning to an unchanged configuration.
    # Inputs: Completed Release and Debug builds with distinct output paths and the same frozen source.
    # Outputs: Reuses only rebuild evidence; current acceptance still requires the newly completed invocation.
    def test_configuration_switch_reuses_matching_outputs_without_accepting_stale_qualification(self):
        self.successful("Release")
        self.successful("Debug")
        with self.assertRaisesRegex(ValueError, "configuration is stale"):
            receipt.validate_current(self.root, "Release", False)
        for configuration in ("Release", "Debug", "Release"):
            token = receipt.begin(self.root, configuration)["transaction_id"]
            self.assertFalse(receipt.prepare(self.root, token, self.cmake)["clean_first_required"])
            with self.assertRaisesRegex(ValueError, "active, failed or incomplete"):
                receipt.validate_current(self.root, configuration, False)
            receipt.finish(self.root, token, self.cmake, False)
            self.assertEqual(
                configuration, receipt.validate_current(self.root, configuration, False)["recipe"]["configuration"]
            )

    # Purpose: Keep another configuration's metadata from qualifying the selected product.
    # Inputs: Completed configurations followed by a copied Debug manifest over the Release manifest.
    # Outputs: Release requires a clean build; the unchanged Debug qualification remains valid.
    def test_configuration_manifest_mismatch_is_not_reused(self):
        self.successful("Release")
        self.successful("Debug")
        release = self.build / "Release/flitzzip-runtime-dependencies.json"
        debug = self.build / "Debug/flitzzip-runtime-dependencies.json"
        self.assertNotEqual(release.read_bytes(), debug.read_bytes())
        release.write_bytes(debug.read_bytes())
        receipt.validate_current(self.root, "Debug", False)
        token = receipt.begin(self.root, "Release")["transaction_id"]
        self.assertTrue(receipt.prepare(self.root, token, self.cmake)["clean_first_required"])
        receipt.end_incomplete(self.root, token, "failed")

    # Purpose: Retain historical shared-manifest evidence without admitting it as current qualification.
    # Inputs: A real fixture receipt with only its output key moved to the old shared location.
    # Outputs: Exact historical digest survives explicit historical validation; strict admission rejects it.
    def test_shared_manifest_is_historical_evidence_only(self):
        self.successful("Release")
        value = receipt.read_metadata(self.root, f"build/{receipt.RECEIPT}")
        outputs = value["outputs_sha256"]
        outputs["build/flitzzip-runtime-dependencies.json"] = outputs.pop(
            "build/Release/flitzzip-runtime-dependencies.json"
        )
        original = canonical(value)
        self.assertEqual(
            hashlib.sha256(original).hexdigest(),
            receipt.validate_receipt(value, allow_historical_toolchain=True),
        )
        with self.assertRaisesRegex(ValueError, "required outputs"):
            receipt.validate_receipt(value)
        self.assertEqual(canonical(value), original)

    # Purpose: Keep altered target outputs and compiler bytes from receiving incremental reuse after a switch.
    # Inputs: Completed configurations followed by independent output and toolchain mutations.
    # Outputs: Each mutation requires a clean build in the original configuration.
    def test_configuration_switch_detects_output_and_toolchain_mutations(self):
        self.successful("Release")
        self.successful("Debug")
        for path in (self.build / "Release/flitzzip_cli.exe", self.compiler / "cl.exe"):
            original = path.read_bytes()
            with self.subTest(path=path.name):
                path.write_bytes(original + b"changed")
                token = receipt.begin(self.root, "Release")["transaction_id"]
                self.assertTrue(receipt.prepare(self.root, token, self.cmake)["clean_first_required"])
                receipt.end_incomplete(self.root, token, "failed")
            path.write_bytes(original)

    # Purpose: Compare changed-source timestamps to the target configuration, not a later unrelated build.
    # Inputs: A source changed between Release and Debug, then copied back with an older timestamp.
    # Outputs: Newer source allows incremental Release compilation; preserved older timestamps require clean-first.
    def test_configuration_switch_uses_its_own_completion_boundary(self):
        self.successful("Release")
        completed = self.build / receipt.rebuild_receipt_name("Release")
        boundary = completed.stat().st_mtime_ns + 2_000_000_000
        os.utime(completed, ns=(boundary, boundary))
        source = self.root / "src/main.cpp"
        source.write_text("source updated after Release and before Debug", encoding="utf-8")
        newer = boundary + 1_000_000_000
        os.utime(source, ns=(newer, newer))
        self.successful("Debug")
        later = newer + 1_000_000_000
        os.utime(self.build / receipt.RECEIPT, ns=(later, later))
        token = receipt.begin(self.root, "Release")["transaction_id"]
        self.assertFalse(receipt.prepare(self.root, token, self.cmake)["clean_first_required"])
        receipt.end_incomplete(self.root, token, "failed")
        os.utime(source, ns=(boundary - 1, boundary - 1))
        token = receipt.begin(self.root, "Release")["transaction_id"]
        self.assertTrue(receipt.prepare(self.root, token, self.cmake)["clean_first_required"])

    # Purpose: Preserve fail-closed behavior for absent or malformed per-configuration rebuild evidence.
    # Inputs: A switched build with missing metadata, then duplicate JSON fields and an unsupported configuration.
    # Outputs: Missing evidence requires clean-first; malformed metadata and unsafe names are rejected.
    def test_configuration_rebuild_metadata_is_bounded_and_never_inferred(self):
        self.successful("Release")
        self.successful("Debug")
        cached = self.build / receipt.rebuild_receipt_name("Release")
        cached.unlink()
        token = receipt.begin(self.root, "Release")["transaction_id"]
        self.assertTrue(receipt.prepare(self.root, token, self.cmake)["clean_first_required"])
        receipt.end_incomplete(self.root, token, "failed")
        cached.write_text('{"status":1,"status":2}', encoding="utf-8")
        token = receipt.begin(self.root, "Release")["transaction_id"]
        with self.assertRaisesRegex(ValueError, "duplicate"):
            receipt.prepare(self.root, token, self.cmake)
        with self.assertRaisesRegex(ValueError, "unsupported"):
            receipt.rebuild_receipt_name("../Release")

    # Purpose: Bind real source/output bytes without exposing compiler or checkout locations.
    # Inputs: Completed fixture invocation. Outputs: Requires independent hashes and current-source acceptance.
    def test_success_binds_content_and_rejects_hip_fallback(self):
        _, result = self.successful()
        self.assertEqual(
            result["receipt_sha256"], receipt.validate_current(self.root, "Release", False)["receipt_sha256"]
        )
        data = receipt.read_metadata(self.root, f"build/{receipt.RECEIPT}")
        output = self.build / "Release/flitzzip_cli.exe"
        self.assertEqual(
            data["outputs_sha256"]["build/Release/flitzzip_cli.exe"], hashlib.sha256(output.read_bytes()).hexdigest()
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
            "build/Release/FlitzZip.exe",
            "build/Release/flitzzip_cli.exe",
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
        self.cache["FLITZZIP_PACKAGE_VERSION"] = "0.9.0"
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

    # Purpose: Prevent copied/restored source from being blessed when the incremental compiler sees an old timestamp.
    # Inputs: A successful build, changed source bytes, and the original older modification time.
    # Outputs: Requires clean-first acknowledgement and rejects a nominally successful incremental invocation.
    def test_preserved_timestamp_source_change_requires_clean_build(self):
        source = self.root / "src/main.cpp"
        original_time = source.stat().st_mtime_ns
        self.successful()
        source.write_text("replacement source with preserved timestamp", encoding="utf-8")
        os.utime(source, ns=(original_time, original_time))
        token = receipt.begin(self.root, "Release")["transaction_id"]
        self.assertTrue(receipt.prepare(self.root, token, self.cmake)["clean_first_required"])
        with self.assertRaisesRegex(ValueError, "fresh build"):
            receipt.finish(self.root, token, self.cmake, False)

    # Purpose: Cover consumers compiled after the last relink of the application executables.
    # Inputs: Changed bytes newer than unchanged application outputs but older than build completion.
    # Outputs: Requires a clean build, preventing stale test/library objects from being accepted.
    def test_source_older_than_completed_invocation_requires_clean_build(self):
        self.successful()
        completed = self.build / receipt.RECEIPT
        output_time = max(path.stat().st_mtime_ns for path in (self.build / "Release").glob("*"))
        completed_time = output_time + 2_000_000_000
        os.utime(completed, ns=(completed_time, completed_time))
        source = self.root / "src/main.cpp"
        source.write_text("copied source predating the last compiled consumer", encoding="utf-8")
        copied_time = output_time + 1_000_000_000
        os.utime(source, ns=(copied_time, copied_time))
        token = receipt.begin(self.root, "Release")["transaction_id"]
        self.assertTrue(receipt.prepare(self.root, token, self.cmake)["clean_first_required"])

    # Purpose: Keep removed inputs and coarse filesystem timestamps from retaining stale compiled definitions.
    # Inputs: A removed source or changed bytes with exactly an output's timestamp.
    # Outputs: Both cases require a fresh build; ordinary newer edits retain the existing incremental test.
    def test_removed_or_equal_timestamp_inputs_require_clean_build(self):
        for deleted in (False, True):
            with self.subTest(deleted=deleted):
                self.successful()
                source = self.root / "src/main.cpp"
                if deleted:
                    source.unlink()
                    subprocess.run(
                        ["git", "-C", str(self.root), "update-index", "--force-remove", "--", "src/main.cpp"],
                        check=True,
                        capture_output=True,
                    )
                else:
                    output = self.root / "build/Release/flitzzip_cli.exe"
                    timestamp = output.stat().st_mtime_ns
                    source.write_text("same tick different bytes", encoding="utf-8")
                    os.utime(source, ns=(timestamp, timestamp))
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
        self.cache["FLITZZIP_HIP_ARCH"] = "C:/private/location"
        self.write_cache()
        with self.assertRaises(ValueError):
            receipt.configured_recipe(self.root, "Release")

    # Purpose: Prevent malformed portable evidence from qualifying missing, differently scoped or private artifacts.
    # Inputs: A real successful fixture receipt with independent schema, value and path substitutions.
    # Outputs: Every invalid receipt is rejected without changing the accepted receipt or transaction.
    def test_exported_receipt_rejects_schema_scope_and_identity_substitution(self):
        self.successful()
        path = self.build / receipt.RECEIPT
        original = path.read_bytes()
        valid = json.loads(original)
        changes = [
            (("schema_version",), True),
            (("schema_version",), 2),
            (("kind",), "configure-only"),
            (("status",), "failed"),
            (("transaction_id",), "F" * 32),
            (("clean_first",), 1),
            (("inputs",), []),
            (("inputs", "schema_version"), True),
            (("inputs", "projection"), "all-host-files"),
            (("inputs", "inputs_sha256"), "0" * 64),
            (("inputs", "files"), {}),
            (("inputs", "files"), {"../outside": "a" * 64}),
            (("inputs", "files"), {"C:/private/file": "a" * 64}),
            (("inputs", "files"), {"src/./file.cpp": "a" * 64}),
            (("inputs", "files"), {"src/file.cpp": "G" * 64}),
            (("inputs", "files"), {"unrelated/file.cpp": "a" * 64}),
            (("recipe",), []),
            (("recipe", "configuration"), "Other"),
            (("recipe", "FLITZZIP_BUILD_GUI"), "OFF"),
            (("recipe", "FLITZZIP_HIP_ARCH"), "native"),
            (("recipe", "FLITZZIP_VCVARS_VERSION"), "C:/private/toolchain"),
            (("recipe", "CMAKE_GENERATOR_PLATFORM"), "arm64"),
            (("outputs_sha256",), {"build/Release/FlitzZip.exe": "a" * 64}),
        ]
        for keys, replacement in changes:
            value = copy.deepcopy(valid)
            target = value
            for key in keys[:-1]:
                target = target[key]
            target[keys[-1]] = replacement
            with self.subTest(keys=keys, replacement=replacement), self.assertRaises(ValueError):
                receipt.validate_receipt(value)
        for keys in (("recipe",), ("inputs",), ("toolchain",), ("outputs_sha256",)):
            value = copy.deepcopy(valid)
            value[keys[0]]["unexpected"] = "unreviewed"
            with self.subTest(keys=keys), self.assertRaises(ValueError):
                receipt.validate_receipt(value)
        value = copy.deepcopy(valid)
        value["outputs_sha256"]["build/Release/outside.exe"] = "a" * 64
        with self.assertRaisesRegex(ValueError, "app-local"):
            receipt.validate_receipt(value)
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual(
            receipt.validate_current(self.root, "Release", False)["receipt_sha256"],
            hashlib.sha256(canonical(valid)).hexdigest(),
        )

    # Purpose: Bound metadata readers and publishers without sacrificing an existing accepted generation.
    # Inputs: Oversized regular files and scalar/nonfinite JSON; bounded fixture payloads never execute.
    # Outputs: Rejection preserves the destination and creates no temporary or misleading accepted metadata.
    def test_metadata_bounds_and_invalid_json_preserve_existing_generation(self):
        destination = self.build / "bound.json"
        destination.write_bytes(b'{"generation":"old"}\n')
        old = destination.read_bytes()
        with self.assertRaisesRegex(ValueError, "exceeds its bound"):
            receipt.publish(self.root, "bound.json", {"payload": "x" * receipt.MAX_METADATA})
        self.assertEqual(destination.read_bytes(), old)
        oversized = b" " * (receipt.MAX_METADATA + 1)
        destination.write_bytes(oversized)
        with self.assertRaisesRegex(ValueError, "exceeds its bound"):
            receipt.read_metadata(self.root, "build/bound.json")
        with self.assertRaisesRegex(ValueError, "previous native build metadata exceeds"):
            receipt.publish(self.root, "bound.json", {"generation": "new"})
        self.assertEqual(destination.read_bytes(), oversized)
        for payload in (b"[]", b"42", b'{"bad":NaN}', b'{"bad":Infinity}', b'{"nested":{"x":1,"x":2}}'):
            destination.write_bytes(payload)
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                receipt.read_metadata(self.root, "build/bound.json")
        self.assertEqual(list(self.build.glob(".native-build-*.tmp")), [])

    # Purpose: Require explicit complete native build settings before observing or exporting compiler identities.
    # Inputs: Private CMake cache and compiler probe mutations. Outputs: No unrecognized or private recipe qualifies.
    def test_configured_recipe_and_compiler_probe_fail_closed(self):
        original = dict(self.cache)
        for key, value, message in (
            ("CMAKE_GENERATOR", "Ninja", "Windows generator"),
            ("CMAKE_GENERATOR_PLATFORM", "Win32", "platform or HIP"),
            ("FLITZZIP_ENABLE_HIP", "unknown", "platform or HIP"),
            ("FLITZZIP_BUILD_GUI", "OFF", "complete app"),
            ("FLITZZIP_BUILD_TESTS", "OFF", "complete app"),
            ("FLITZZIP_VCVARS_VERSION", "private/tool/path", "installation paths"),
        ):
            self.cache = {**original, key: value}
            self.write_cache()
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, message):
                receipt.configured_recipe(self.root, "Release")
        self.cache = {key: value for key, value in original.items() if key != "FLITZZIP_HIP_ARCH"}
        self.write_cache()
        with self.assertRaisesRegex(ValueError, "lacks required"):
            receipt.configured_recipe(self.root, "Release")
        self.cache = original
        self.write_cache()
        probe = self.build / "CMakeFiles/4.4.3/CMakeCXXCompiler.cmake"
        text = probe.read_text()
        for value, message in (
            ("", "lacks required"),
            (text.replace('"MSVC"', '"Clang"'), "observed MSVC"),
            (text.replace('"19.51.36260.0"', '"invalid"'), "observed MSVC"),
        ):
            probe.write_text(value)
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                receipt.observe_toolchain(self.root, self.cache, self.cmake)
        probe.write_text(text)
        self.cache["CMAKE_CACHE_MAJOR_VERSION"] = "../../outside"
        with self.assertRaisesRegex(ValueError, "invalid CMake probe version"):
            receipt.observe_toolchain(self.root, self.cache, self.cmake)

    # Purpose: Detect interrupted, forged and reused transactions independently from their final binary content.
    # Inputs: Actual state transitions and source mutation after prepare. Outputs: No stale or partial success.
    def test_transaction_replay_and_completion_identity(self):
        token, _ = self.successful()
        for candidate in ("", "0" * 32, token):
            with self.subTest(token=candidate), self.assertRaisesRegex(ValueError, "no longer incomplete"):
                receipt.pending(self.root, candidate)
        original = (self.build / receipt.RECEIPT).read_bytes()
        state = receipt.read_metadata(self.root, f"build/{receipt.TRANSACTION}")
        for key, value in (("transaction_id", "0" * 32), ("receipt_sha256", "0" * 64)):
            receipt.publish(self.root, receipt.TRANSACTION, {**state, key: value})
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "completed transaction"):
                receipt.validate_current(self.root, "Release", False)
        token = receipt.begin(self.root, "Release")["transaction_id"]
        receipt.prepare(self.root, token, self.cmake)
        (self.root / "src/main.cpp").write_text("source replacement after compilation began")
        with self.assertRaisesRegex(ValueError, "during build"):
            receipt.finish(self.root, token, self.cmake, False)
        self.assertEqual((self.build / receipt.RECEIPT).read_bytes(), original)
        with self.assertRaisesRegex(ValueError, "unsupported incomplete"):
            receipt.end_incomplete(self.root, token, "successful")

    # Purpose: Refuse missing or foreign runtime outputs and force clean reconstruction after lost artifacts.
    # Inputs: Private manifest substitutions and one deleted declared dependency.
    # Outputs: Output capture fails closed and preparation requests a fresh build without reusing a stale receipt.
    def test_runtime_inventory_is_bounded_and_requires_real_files(self):
        self.successful()
        manifest = self.build / "Release/flitzzip-runtime-dependencies.json"
        original = manifest.read_bytes()
        for value in (None, [], [None], [{"name": "../outside.dll"}], [{"name": "FlitzZip.exe"}]):
            manifest.write_text(json.dumps({"packaged_runtime_files": value}))
            with self.subTest(value=value), self.assertRaises(ValueError):
                receipt.capture_outputs(self.root, "Release")
        manifest.write_bytes(original)
        (self.build / "Release/fixture.dll").unlink()
        token = receipt.begin(self.root, "Release")["transaction_id"]
        self.assertTrue(receipt.prepare(self.root, token, self.cmake)["clean_first_required"])
        with self.assertRaises(FileNotFoundError):
            receipt.finish(self.root, token, self.cmake, True)

    # Purpose: Bind the full documented HIP compiler scope without touching an installed SDK or executing kernels.
    # Inputs: Small private tool, header and bitcode bytes plus independent scope/digest/inventory substitutions.
    # Outputs: Exact observed hashes, a valid required-HIP receipt and rejection of every incomplete SDK identity.
    def test_hip_toolchain_receipt_binds_critical_files_and_device_bitcode(self):
        sdk = self.root / "private sdk"
        names = (
            "bin/hipcc.exe",
            "lib/llvm/bin/clang.exe",
            "lib/amdhip64.lib",
            "include/hip/hip_version.h",
            "lib/llvm/amdgcn/bitcode/ocml.bc",
            "lib/llvm/amdgcn/bitcode/ockl.bc",
        )
        for name in names:
            path = sdk / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(("Nonexecutable SDK fixture: " + name).encode())
        self.cache.update(FLITZZIP_ENABLE_HIP="ON", FLITZZIP_HIP_PATH=str(sdk))
        lock = self.root / "tools/rocm-sdk-lock.json"
        lock.parent.mkdir(exist_ok=True)
        lock.write_bytes(b'{"fixture":"nonexecutable SDK identity"}\n')
        self.write_cache()
        self.successful()
        data = receipt.read_metadata(self.root, f"build/{receipt.RECEIPT}")
        actual = data["toolchain"]["hip_sdk"]
        self.assertEqual(
            actual["critical_files_sha256"],
            {name: hashlib.sha256((sdk / name).read_bytes()).hexdigest() for name in names},
        )
        self.assertEqual(actual["lock_sha256"], hashlib.sha256(lock.read_bytes()).hexdigest())
        receipt.validate_current(self.root, "Release", True)
        for replacement in (
            None,
            {},
            {**actual, "scope": "unreviewed"},
            {**actual, "lock_sha256": "invalid"},
            {**actual, "critical_files_sha256": {"bin/hipcc.exe": "a" * 64}},
        ):
            changed = copy.deepcopy(data)
            changed["toolchain"]["hip_sdk"] = replacement
            with self.subTest(replacement=replacement), self.assertRaises(ValueError):
                receipt.validate_receipt(changed)
        token = receipt.begin(self.root, "Release")["transaction_id"]
        self.assertFalse(receipt.prepare(self.root, token, self.cmake)["clean_first_required"])
        (sdk / "lib/llvm/amdgcn/bitcode/ockl.bc").write_bytes(b"changed device runtime")
        with self.assertRaisesRegex(ValueError, "toolchain changed"):
            receipt.finish(self.root, token, self.cmake, False)

    # Purpose: Reject unsupported configurations before touching metadata and non-directory build destinations.
    # Inputs: Invalid configuration names and an existing regular file standing in for the build directory.
    # Outputs: Clear early refusal without acceptance or replacement of caller data.
    def test_invalid_configuration_and_non_directory_refusal(self):
        for operation in (receipt.begin, receipt.configured_recipe, receipt.capture_outputs):
            for configuration in ("", "../Release", "release", "MinSizeRel"):
                with (
                    self.subTest(operation=operation.__name__, configuration=configuration),
                    self.assertRaisesRegex(ValueError, "unsupported"),
                ):
                    operation(self.root, configuration)
        file = self.root / "regular-file"
        file.write_bytes(b"preserve")
        with self.assertRaisesRegex(ValueError, "unlinked directory"):
            receipt.reject_link(file)
        self.assertEqual(file.read_bytes(), b"preserve")

    # Purpose: Reject conflicting historical aliases rather than silently selecting one compiler policy.
    # Inputs: An exported receipt containing both tested predecessor and current HIP keys.
    # Outputs: Historical reading refuses ambiguity and leaves live qualification unchanged.
    def test_historical_recipe_alias_collision_is_rejected(self):
        self.successful()
        data = receipt.read_metadata(self.root, f"build/{receipt.RECEIPT}")
        data["recipe"]["SUPERZIP_ENABLE_HIP"] = "ON"
        with self.assertRaisesRegex(ValueError, "ambiguous native configured recipe"):
            receipt.validate_receipt(data, allow_historical_toolchain=True)
        receipt.validate_current(self.root, "Release", False)

    # Purpose: Keep corrupted tool metadata and runaway runtime inventories from allocating or hashing unbounded input.
    # Inputs: Oversized real metadata, a bounded declared DLL list and a simulated SDK directory enumeration.
    # Outputs: Every resource limit rejects before payload hashes or compiler execution; prior outputs survive.
    def test_cache_probe_and_component_inventory_resource_bounds(self):
        cache = self.build / "CMakeCache.txt"
        probe = self.build / "CMakeFiles/4.4.3/CMakeCXXCompiler.cmake"
        for path, operation, message in (
            (cache, lambda: receipt.configured_recipe(self.root, "Release"), "cache exceeds"),
            (probe, lambda: receipt.observe_toolchain(self.root, self.cache, self.cmake), "probe exceeds"),
        ):
            original = path.read_bytes()
            path.write_bytes(b" " * 1025)
            try:
                with (
                    self.subTest(path=path.name),
                    patch.object(receipt, "MAX_METADATA", 1024),
                    self.assertRaisesRegex(ValueError, message),
                ):
                    operation()
            finally:
                path.write_bytes(original)
        manifest = self.build / "Release/flitzzip-runtime-dependencies.json"
        original = manifest.read_bytes()
        manifest.write_text(json.dumps({"packaged_runtime_files": [{"name": f"library-{i}.dll"} for i in range(254)]}))
        with self.assertRaisesRegex(ValueError, "output inventory exceeds"):
            receipt.capture_outputs(self.root, "Release")
        manifest.write_bytes(original)
        sdk = self.root / "private SDK"
        bitcode = sdk / "lib/llvm/amdgcn/bitcode"
        bitcode.mkdir(parents=True)
        (bitcode / "ocml.bc").write_bytes(b"nonexecutable")
        cache_values = {**self.cache, "FLITZZIP_ENABLE_HIP": "ON", "FLITZZIP_HIP_PATH": str(sdk)}
        entries = [bitcode / f"runtime-{i}.bc" for i in range(1001)]
        with (
            patch.object(Path, "glob", return_value=iter(entries)),
            self.assertRaisesRegex(ValueError, "critical SDK inventory exceeds"),
        ):
            receipt.observe_toolchain(self.root, cache_values, self.cmake)

    # Purpose: Exercise the real command entry points and direct-script import boundary without running a compiler.
    # Inputs: Private fixture checkout and explicit transaction arguments.
    # Outputs: Exact JSON lifecycle or stable failure.
    def test_command_line_lifecycle_and_diagnostics(self):
        script = Path(receipt.__file__).resolve()

        # Purpose: Run only the receipt CLI within a bounded private fixture.
        # Inputs: Explicit operation/options and invocation spelling. Outputs: Captured, checked process result.
        def invoke(*arguments, direct=False, expected=0):
            prefix = (
                [sys.executable, "-B", str(script)]
                if direct
                else [sys.executable, "-B", "-m", "tools.native_build_receipt"]
            )
            process = subprocess.run(
                [*prefix, "--root", str(self.root), *arguments],
                cwd=script.parents[1],
                capture_output=True,
                text=True,
                timeout=15,
            )
            self.assertEqual(process.returncode, expected, process.stderr)
            return process

        for direct in (False, True):
            state = json.loads(invoke("begin", direct=direct).stdout)
            options = ["--token", state["transaction_id"], "--cmake", str(self.cmake)]
            prepared = json.loads(invoke("prepare", *options, direct=direct).stdout)
            fresh = ["--clean-first"] if prepared["clean_first_required"] else []
            result = json.loads(invoke("finish", *options, *fresh, direct=direct).stdout)
            validated = json.loads(invoke("validate", direct=direct).stdout)
            self.assertEqual(result["receipt_sha256"], validated["receipt_sha256"])
        for terminal in ("failed", "configure-only"):
            token = json.loads(invoke("begin").stdout)["transaction_id"]
            self.assertEqual(json.loads(invoke(terminal, "--token", token).stdout), {"status": terminal})
            self.assertIn("active, failed or incomplete", invoke("validate", expected=1).stderr)
        self.assertIn("require --cmake", invoke("prepare", expected=2).stderr)
        self.assertIn("FileNotFoundError", invoke("validate", "--root", str(self.root / "missing"), expected=1).stderr)

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
