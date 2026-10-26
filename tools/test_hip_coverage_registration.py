"""Offline failure-boundary contracts for the native HIP profiling registration repair."""

import copy
import csv
import hashlib
import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from contextlib import ExitStack, redirect_stdout
from functools import partial
from pathlib import Path
from unittest.mock import patch

from tools import hip_coverage as coverage
from tools import hip_coverage_registration as registration
from tools import hip_coverage_toolchain as toolchain
from tools import hip_registration_native_checks as native_checks
from tools import scanner_metadata_review as metadata_review


class HipRegistrationTests(unittest.TestCase):
    # Purpose: Own tiny independent native metadata and tool fixtures. Inputs: None. Outputs: Cleanup-bound files.
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root / "probe.hip.cpp"
        self.source.write_text("independent source identity", encoding="utf-8")
        self.tool = self.root / "register.exe"
        self.tool.write_bytes(b"native tool identity")
        self.native_source = self.root / "native-source"
        self.native_source.mkdir()
        for name in ("CMakeLists.txt", "hip_profile_registration.cpp"):
            (self.native_source / name).write_text(name, encoding="utf-8")
        self.revision = registration.read_toolchain()["revision"]
        self.identity = {
            "schema_version": 1,
            "llvm_revision": self.revision,
            "toolchain_sha256": coverage.digest(registration.LOCK),
            "toolchain_parser_sha256": coverage.digest(registration.LOCK.with_name("hip_coverage_toolchain.py")),
            "source_sha256": coverage.digest(self.native_source / "hip_profile_registration.cpp"),
            "cmake_sha256": coverage.digest(self.native_source / "CMakeLists.txt"),
            "profile_record_bytes": 72,
            "host": "windows-x64",
            "device": "amdgpu-elf64le",
        }
        table = "__llvm_profile_sections_0123456789abcdef"
        self.completion = {
            "completed_records": 1,
            "retired_profile_groups": 0,
            "original_program_values_preserved": True,
            "before": {
                "module_ir_sha256": "a" * 64,
                "device_data_records": 2,
                "registered_data_records": 1,
                "host_profile_groups": 1,
                "absent_host_profile_groups": 0,
                "missing": ["__profd_independent_helper"],
                "table": table,
                "profile_record_bytes": 72,
            },
            "after": {
                "module_ir_sha256": "b" * 64,
                "device_data_records": 2,
                "registered_data_records": 2,
                "host_profile_groups": 2,
                "absent_host_profile_groups": 0,
                "missing": [],
                "table": table,
                "profile_record_bytes": 72,
            },
        }

    # Purpose: Reject ambiguous native evidence. Inputs: Duplicate/nested/nonfinite/nonobject JSON. Outputs: Refusal.
    def test_metadata_is_unambiguous_and_finite(self):
        self.assertEqual(registration.report_object('{"ok":1}'), {"ok": 1})
        self.assertEqual(registration.report_object('{"finite":1.25}'), {"finite": 1.25})
        for value in (
            '{"x":1,"x":2}',
            '{"child":{"x":1,"x":2}}',
            '{"x":NaN}',
            '{"x":Infinity}',
            '{"x":-Infinity}',
            '{"nested":{"x":1e400}}',
            '{"x":-1e400}',
            "[]",
            "null",
            "bad JSON",
        ):
            with self.subTest(value=value), self.assertRaises(ValueError):
                registration.report_object(value)

    # Purpose: Prevent admitting foreign/stale source or ABI identity. Inputs: One-field corruptions. Outputs: Refusal.
    def test_tool_requires_exact_source_recipe_revision_and_abi(self):
        with patch.object(registration, "SOURCE_DIRECTORY", self.native_source):
            for key, value in (
                (None, None),
                ("schema_version", True),
                ("schema_version", 2),
                ("llvm_revision", "a" * 40),
                ("source_sha256", "b" * 64),
                ("cmake_sha256", "c" * 64),
                ("toolchain_sha256", "d" * 64),
                ("toolchain_parser_sha256", "e" * 64),
                ("profile_record_bytes", 64),
                ("host", "linux-x64"),
                ("device", "coff"),
            ):
                observed = copy.deepcopy(self.identity)
                if key is not None:
                    observed[key] = value
                with (
                    self.subTest(key=key),
                    patch.object(registration, "native_metadata", return_value=observed) as native,
                ):
                    if key is None:
                        self.assertEqual(registration.admit_tool(self.tool, self.root), observed)
                    else:
                        with self.assertRaisesRegex(ValueError, "reviewed profiling ABI"):
                            registration.admit_tool(self.tool, self.root)
                    self.assertEqual(native.call_args.args[0], [str(self.tool), "--identify"])

    # Purpose: Require complete inventory closure. Inputs: Independent corruptions. Outputs: Refusal.
    def test_completion_requires_exact_counts_preservation_and_mapping(self):
        registration.validate_completion(self.completion)
        retired = copy.deepcopy(self.completion)
        retired.update(completed_records=2, retired_profile_groups=1)
        retired["before"].update(
            registered_data_records=0,
            absent_host_profile_groups=1,
            missing=["__profd_original_kernel", "__profd_independent_helper"],
        )
        registration.validate_completion(retired)
        # LLVM uses unpadded utohexstr(MD5Hash(CUID)); valid identities can omit leading zeroes.
        for suffix in ("0", "eab7f1a6c922eb9", "0123456789abcdef"):
            value = copy.deepcopy(self.completion)
            value["before"]["table"] = value["after"]["table"] = "__llvm_profile_sections_" + suffix
            registration.validate_completion(value)
        corruptions = [
            lambda v: v["before"].pop("module_ir_sha256"),
            lambda v: v["after"].update(module_ir_sha256="not a digest"),
            lambda v: v["before"].update(module_ir_sha256=True),
            lambda v: v.update(before=[]),
            lambda v: v.update(after=None),
            lambda v: v.update(completed_records=True),
            lambda v: v.update(completed_records=-1),
            lambda v: v.update(completed_records=2),
            lambda v: v.update(retired_profile_groups=True),
            lambda v: v.update(retired_profile_groups=1),
            lambda v: v["before"].update(host_profile_groups=0),
            lambda v: v["before"].update(absent_host_profile_groups=1),
            lambda v: v["after"].update(absent_host_profile_groups=1),
            lambda v: v["after"].update(host_profile_groups=3),
            lambda v: v.update(original_program_values_preserved=1),
            lambda v: v["before"].update(device_data_records=0),
            lambda v: v["before"].update(device_data_records=100001),
            lambda v: v["before"].update(registered_data_records=0),
            lambda v: v["after"].update(device_data_records=3),
            lambda v: v["after"].update(registered_data_records=1),
            lambda v: v["before"].update(missing=None),
            lambda v: v["before"].update(missing=[]),
            lambda v: v["before"].update(missing=[{}]),
            lambda v: v["before"].update(missing=["__profd_"]),
            lambda v: v["before"].update(missing=["foreign"]),
            lambda v: v["after"].update(missing=["__profd_unregistered"]),
            lambda v: v["before"].pop("table"),
            lambda v: v["after"].update(table="foreign"),
            lambda v: v["before"].update(table="__llvm_profile_sections_" + "a" * 17),
            lambda v: v["before"].update(profile_record_bytes=64),
        ]
        for index, corrupt in enumerate(corruptions):
            value = copy.deepcopy(self.completion)
            corrupt(value)
            with self.subTest(index=index), self.assertRaises(ValueError):
                registration.validate_completion(value)
        duplicate = copy.deepcopy(self.completion)
        duplicate.update(completed_records=2)
        duplicate["before"].update(device_data_records=3, missing=["__profd_duplicate", "__profd_duplicate"])
        duplicate["after"].update(device_data_records=3, registered_data_records=3)
        with self.assertRaises(ValueError):
            registration.validate_completion(duplicate)

    # Purpose: Preserve complete native reports beyond the interactive tail and reject truncated evidence.
    # Inputs: A large bounded JSON artifact and a deliberately truncated result. Outputs: Exact metadata or refusal.
    def test_native_metadata_is_complete_and_retains_argv(self):
        value = {"missing": ["__profd_" + "x" * 20000]}
        result = {"stdout": json.dumps(value), "output_truncated": False}
        log = self.root / "native.json"
        command = [str(self.tool), "--identify"]
        with patch.object(registration, "run_checked", return_value=result) as native:
            self.assertEqual(registration.native_metadata(command, log), value)
        self.assertEqual(json.loads((self.root / "native-command.json").read_text()), command)
        native.assert_called_once_with(command, 60, log)
        with (
            patch.object(registration, "run_checked", return_value={**result, "output_truncated": True}),
            self.assertRaisesRegex(RuntimeError, "metadata was truncated"),
        ):
            registration.native_metadata(command, self.root / "truncated.json")

    # Purpose: Preserve source mapping and the same CUID. Inputs: Captured argv. Outputs: Typed argv.
    def test_host_compile_retains_source_and_cuid_without_preprocessing(self):
        command = [
            "clang",
            "-x",
            "hip",
            "-cuid=" + "a" * 64,
            "-Xoffload-linker",
            "runtime.a",
            str(self.source),
            "-o",
            "old.exe",
        ]
        with patch.object(coverage, "PROBE", self.source), patch.object(coverage, "checked") as native:
            host = registration.compile_host(command, self.root / "image.hipfb", self.root)
        self.assertEqual(host, self.root / "original.bc")
        actual = native.call_args.args[0]
        self.assertIn("-cuid=" + "a" * 64, actual)
        self.assertIn("--offload-host-only", actual)
        self.assertIn("-emit-llvm", actual)
        self.assertIn(str(self.source), actual)
        self.assertNotIn("-Xoffload-linker", actual)
        self.assertNotIn("runtime.a", actual)
        self.assertFalse(any(value.startswith("-save-temps") for value in actual))

    # Purpose: Reject malformed TU identifiers before touching toolchains. Inputs: Invalid selectors. Outputs: Refusal.
    def test_cuid_is_a_single_explicit_digest(self):
        for value in ("", "a" * 63, "A" * 64, "../input", "a" * 64 + " -flag"):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "translation-unit identity"):
                coverage.compile_probe(self.root, self.root, self.source, "gfx1201", self.root, cuid=value)

    # Purpose: Reject original redirected CLI inputs before canonicalization can conceal their link components.
    # Inputs: A real Windows junction and all four compiler/runtime/tool selectors. Outputs: No qualification starts.
    @unittest.skipUnless(os.name == "nt", "Actual Windows junction boundary")
    def test_cli_cannot_resolve_away_redirected_inputs(self):
        real = self.root / "real-layout"
        real.mkdir()
        (real / "compiler").mkdir()
        (real / "sdk").mkdir()
        for name in ("runtime.a", "register.exe"):
            (real / name).write_bytes(b"owned fixture")
        junction = self.root / "junction"
        created = subprocess.run(
            ["cmd.exe", "/d", "/c", "mklink", "/J", str(junction), str(real)],
            capture_output=True,
            timeout=10,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        self.assertEqual(created.returncode, 0, "Owned junction fixture creation failed")
        self.addCleanup(junction.rmdir)
        self.assertTrue(junction.is_junction())
        (real / "provenance.json").write_bytes(toolchain.LOCK.read_bytes())
        self.assertEqual(toolchain.read_toolchain(real / "provenance.json"), toolchain.read_toolchain())
        with self.assertRaisesRegex(ValueError, "linked path"):
            toolchain.read_toolchain(junction / "provenance.json")
        arguments = [
            "hip_coverage",
            "--compiler-root",
            str(real / "compiler"),
            "--sdk",
            str(real / "sdk"),
            "--device-runtime",
            str(real / "runtime.a"),
            "--registration-tool",
            str(real / "register.exe"),
            "--arch",
            "gfx1201",
            "--output",
            str(self.root / "never-created"),
        ]
        result = {"qualification": "passed", "scope": "fixture", "architecture": "gfx1201", "controls": {}}
        for index, name in ((2, "compiler"), (4, "sdk"), (6, "runtime.a"), (8, "register.exe")):
            redirected = arguments.copy()
            redirected[index] = str(junction / name)
            with (
                self.subTest(selector=name),
                patch.object(sys, "argv", redirected),
                patch.object(coverage, "qualify", return_value=result) as qualify,
                redirect_stdout(io.StringIO()),
            ):
                with self.assertRaisesRegex(ValueError, "linked path"):
                    coverage.main()
                qualify.assert_not_called()

    # Purpose: Exercise compilation admission and its actual byte/source boundaries without a compiler or GPU.
    # Inputs: Controlled native file producers, foreign revision, image drift or changed source.
    # Outputs: Only exact unchanged inputs/device bytes return a linked control; failure never executes one.
    def test_registered_compile_preserves_images_and_rejects_drift(self):
        for fault in (None, "compiler", "image", "source", "inventory", "tool", "native-controls"):
            output = self.root / (fault or "valid")
            output.mkdir()
            immutable_source = (self.native_source / "hip_profile_registration.cpp").read_bytes()
            tool_identity = copy.deepcopy(self.identity)
            if fault == "tool":
                tool_identity["llvm_revision"] = "0" * 40

            # Purpose: Produce a known baseline. Inputs: Explicit paths/CUID. Outputs: Files and real hash identities.
            def compile_baseline(compiler, sdk, runtime, target, baseline, *, cuid, fault=fault):
                self.assertRegex(cuid, r"^[a-f0-9]{64}$")
                executable = baseline / "probe.exe"
                executable.write_bytes(b"baseline binary")
                (baseline / "compiler.json").write_text(
                    json.dumps({"stdout": "foreign" if fault == "compiler" else self.revision})
                )
                command = [
                    str(compiler / "bin/clang++.exe"),
                    "-x",
                    "hip",
                    "-cuid=" + cuid,
                    "-Xoffload-linker",
                    str(runtime),
                    str(self.source),
                    "-o",
                    str(executable),
                    "-x",
                    "none",
                    "host-profile.lib",
                ]
                (baseline / "compile.json").write_text(json.dumps({"command": command}))
                return executable, coverage.identities([self.source])

            # Purpose: Model bounded native compiler outputs. Inputs: Typed argv. Outputs: Owned tiny artifacts.
            def native(command, log, timeout=120, *, fault, tool_identity):
                if command[-1] == "--identify":
                    return {"stdout": json.dumps(tool_identity)}
                destination = Path(command[command.index("-o") + 1])
                destination.write_bytes(b"compiled fixture")
                if "--complete" in command:
                    report = copy.deepcopy(self.completion)
                    if fault == "inventory":
                        report["after"]["missing"] = ["__profd_not_registered"]
                    if fault == "source":
                        (self.native_source / "hip_profile_registration.cpp").write_bytes(
                            b"changed during native command"
                        )
                    return {"stdout": json.dumps(report)}
                return {"stdout": ""}

            # Purpose: Supply distinct actual image files. Inputs: Baseline/relinked owner. Outputs: Independent bytes.
            def extract(llvm_bin, binary, target, directory, fault=fault):
                image = directory / "device.elf"
                image.write_bytes(
                    b"changed device code"
                    if fault == "image" and directory.name == "registration-work"
                    else b"original device code"
                )
                (directory / "image.hipfb").write_bytes(b"bundle")
                return image

            # Purpose: Model metadata capture. Inputs: Typed argv. Outputs: Strict report from the native fixture.
            def metadata(command, log, timeout=60, *, producer):
                return json.loads(producer(command, log, timeout)["stdout"])

            with (
                self.subTest(fault=fault),
                patch.object(registration, "SOURCE_DIRECTORY", self.native_source),
                patch.object(coverage, "PROBE", self.source),
                patch.object(coverage, "compile_probe", side_effect=compile_baseline) as compiler,
                patch.object(
                    coverage, "checked", side_effect=partial(native, fault=fault, tool_identity=tool_identity)
                ),
                patch.object(
                    registration,
                    "native_metadata",
                    side_effect=partial(metadata, producer=partial(native, fault=fault, tool_identity=tool_identity)),
                ),
                patch.object(coverage, "device_image", side_effect=extract),
                patch.object(coverage, "run_controls") as execute,
                patch("tools.hip_registration_native_checks.NativeChecks") as native_checks,
            ):
                if fault == "native-controls":
                    native_checks.return_value.run.side_effect = ValueError("native control refusal")
                if fault is None:
                    executable, inputs = registration.compile_registered_probe(
                        self.root, self.root, self.source, self.tool, "gfx1201", output
                    )
                    self.assertEqual(executable, output / "probe.exe")
                    self.assertEqual(coverage.identities([Path(name) for name in inputs]), inputs)
                else:
                    with self.assertRaises(ValueError):
                        registration.compile_registered_probe(
                            self.root, self.root, self.source, self.tool, "gfx1201", output
                        )
                if fault == "tool":
                    compiler.assert_not_called()
                execute.assert_not_called()
            (self.native_source / "hip_profile_registration.cpp").write_bytes(immutable_source)

    # Purpose: Own minimal native-tool inputs without compiling or executing device code.
    # Inputs: Fresh scenario name. Outputs: A real qualification owner with independent file contents.
    def native_checker(self, name):
        with patch.object(coverage, "ROOT", self.root):
            checker = native_checks.NativeChecks(self.tool, self.tool, self.source, self.tool, self.root / name)
        checker.text = self.source.read_text(encoding="utf-8")
        return checker

    # Purpose: Model the native producer at its command boundary while preserving actual artifact reads/writes.
    # Inputs: One independent report fault and command/log paths. Outputs: Metadata and exact owned output bytes.
    def completion_producer(self, fault, command, log, timeout):
        self.assertEqual(timeout, 30)
        host = Path(command[command.index("--host") + 1])
        content = host.read_bytes()
        completed_bytes = b"independently completed host module"
        before = copy.deepcopy(self.completion["after"] if content == completed_bytes else self.completion["before"])
        before["module_ir_sha256"] = hashlib.sha256(content).hexdigest()
        if "--complete" not in command:
            if fault == "wrong-probe" and log.stem == "original":
                before["device_data_records"] = 3
            if fault == "path-dependent-digest" and log.stem == "digest-relocated":
                before["module_ir_sha256"] = "c" * 64
            if fault == "constant-digest" and log.stem == "digest-text-changed":
                before["module_ir_sha256"] = hashlib.sha256(self.source.read_bytes()).hexdigest()
            if fault == "persisted-inventory" and log.stem == "idempotence-persisted":
                before["registered_data_records"] = 1
            return before
        destination = Path(command[command.index("-o") + 1])
        self.assertFalse(destination.exists())
        destination.write_bytes(completed_bytes)
        after = copy.deepcopy(self.completion["after"])
        after["module_ir_sha256"] = hashlib.sha256(completed_bytes).hexdigest()
        report = {
            "before": before,
            "after": after,
            "completed_records": 0 if content == completed_bytes else 1,
            "retired_profile_groups": 0,
            "original_program_values_preserved": True,
        }
        if log.stem == "complete" and fault in ("reported-original", "reported-completed"):
            report["before" if fault == "reported-original" else "after"]["module_ir_sha256"] = "c" * 64
        if log.stem == "idempotence" and fault == "idempotence-program":
            report["after"]["module_ir_sha256"] = "c" * 64
        return report

    # Purpose: Reject internally plausible reports that do not describe the artifacts actually consumed next.
    # Inputs: Real owned files and one wrong inventory/hash per native-producer scenario.
    # Outputs: All disconnected evidence chains fail; valid completion preserves the existing output.
    def test_native_completion_requires_continuous_persisted_evidence(self):
        for fault in (
            None,
            "reported-original",
            "reported-completed",
            "persisted-inventory",
            "idempotence-program",
            "wrong-probe",
            "path-dependent-digest",
            "constant-digest",
        ):
            checker = self.native_checker(fault or "complete-valid")
            result = {
                "exit_code": 1,
                "timed_out": False,
                "output_limit_exceeded": False,
                "output_truncated": False,
                "stderr": "output already exists",
            }

            # Purpose: Inject only a producer-report fault. Inputs: Native argv/log. Outputs: Independent file evidence.
            def producer(command, log, timeout, fault=fault):
                return self.completion_producer(fault, command, log, timeout)

            with (
                self.subTest(fault=fault),
                patch.object(native_checks, "native_metadata", side_effect=producer),
                patch.object(coverage, "process_owner") as owner,
            ):
                owner.return_value.run_bounded_command.return_value = result
                if fault is None:
                    checker.valid_controls()
                    self.assertEqual(
                        (checker.output / "completed.bc").read_bytes(), b"independently completed host module"
                    )
                    self.assertEqual(
                        (checker.output / "completed-again.bc").read_bytes(), b"independently completed host module"
                    )
                    owner.return_value.run_bounded_command.assert_called_once()
                else:
                    with self.assertRaisesRegex(ValueError, "inventory|evidence|digest|helper-inclusive"):
                        checker.valid_controls()
                    owner.return_value.run_bounded_command.assert_not_called()
            self.assertEqual(self.source.read_text(encoding="utf-8"), "independent source identity")

    # Purpose: Distinguish normal existing-output refusal from failed diagnostic capture.
    # Inputs: Actual preserved destination bytes and independent bounded-process failure flags.
    # Outputs: A truncated/overflowed/timed-out attempt never qualifies as a successful preservation control.
    def test_native_existing_output_requires_complete_process_evidence(self):
        for flag in ("output_limit_exceeded", "output_truncated", "timed_out", "exit_code", "changed-output"):
            checker = self.native_checker(flag)
            result = {
                "exit_code": 1,
                "timed_out": False,
                "output_limit_exceeded": False,
                "output_truncated": False,
                "stderr": "output already exists",
            }
            if flag in result:
                result[flag] = 0 if flag == "exit_code" else True

            # Purpose: Preserve real file operations in the producer. Inputs: Native argv. Outputs: Owned files.
            def producer(command, log, timeout):
                return self.completion_producer(None, command, log, timeout)

            # Purpose: Model a refusal that changes a file. Inputs: Native argv/bounds. Outputs: Real bytes.
            def refusal(command, *, timeout_seconds, flag=flag, result=result):
                self.assertEqual(timeout_seconds, 30)
                if flag == "changed-output":
                    Path(command[command.index("-o") + 1]).write_bytes(b"unexpected replacement")
                return result

            with (
                self.subTest(flag=flag),
                patch.object(native_checks, "native_metadata", side_effect=producer),
                patch.object(coverage, "process_owner") as owner,
            ):
                owner.return_value.run_bounded_command.side_effect = refusal
                with self.assertRaisesRegex(ValueError, "evidence|output"):
                    checker.valid_controls()
            self.assertEqual(json.loads((checker.output / "existing-output.json").read_text()), result)
            expected = b"unexpected replacement" if flag == "changed-output" else b"independently completed host module"
            self.assertEqual((checker.output / "completed.bc").read_bytes(), expected)

    # Purpose: Require the absent-global repair to retain the same inventory chain as ordinary completion.
    # Inputs: A metadata tuple with a neighboring program constant and independently computed repair inventories.
    # Outputs: Unchanged program text and matching persisted evidence; disconnection stops the next control.
    def test_absent_metadata_repair_retains_inventory_and_program_text(self):
        stem = "_Z5probev"
        lines = [
            f'@{index} = private unnamed_addr constant [{len(prefix + stem) + 1} x i8] c"{prefix + stem}\\00"'
            for index, prefix in enumerate(("__profd_", "__profc_", "__llvm_prf_unifcnt_"), 1)
        ]
        program = '@program = private constant [18 x i8] c"__profd__Z5probev\\00"'
        call = (
            "  call void @__hipRegisterVar(ptr %0, ptr @__llvm_profile_shadow_data_probe, "
            "ptr @1, ptr @1, i32 0, i64 8, i32 0, i32 0)"
        )
        for fault in (None, "before", "after", "persisted", "wrong-repair", "missing-call"):
            checker = self.native_checker("absent-" + (fault or "valid"))
            checker.text = "\n".join([*lines, program, call]) + "\n"
            if fault == "missing-call":
                checker.text = checker.text.replace(call + "\n", "")
            initial = copy.deepcopy(self.completion["before"])
            initial.update(registered_data_records=0, absent_host_profile_groups=1)
            initial["missing"] = ["__profd_independent_helper", "__profd_independent_kernel"]
            final = copy.deepcopy(self.completion["after"])

            # Purpose: Describe independently specified native repair stages. Inputs: Real argv. Outputs: Owned bytes.
            def producer(command, log, timeout, *, fault, initial, final):
                self.assertEqual(timeout, 30)
                if log.stem == "absent-original":
                    return copy.deepcopy(initial)
                if log.stem == "absent-idempotence-persisted":
                    result = copy.deepcopy(final)
                    if fault == "persisted":
                        result["host_profile_groups"] = 1
                    return result
                complete = log.stem == "absent-completion"
                result = {
                    "before": copy.deepcopy(initial if complete else final),
                    "after": copy.deepcopy(final),
                    "completed_records": 2 if complete else 0,
                    "retired_profile_groups": 1 if complete else 0,
                    "original_program_values_preserved": True,
                }
                Path(command[command.index("-o") + 1]).write_bytes(b"completed absent-group fixture")
                if complete and fault == "wrong-repair":
                    return copy.deepcopy(self.completion)
                if complete and fault in ("before", "after"):
                    result[fault]["module_ir_sha256"] = "c" * 64
                return result

            with (
                self.subTest(fault=fault),
                patch.object(
                    native_checks,
                    "native_metadata",
                    side_effect=partial(producer, fault=fault, initial=initial, final=final),
                ),
                patch.object(checker, "rejected") as negative,
            ):
                if fault is None:
                    checker.absent_metadata_controls()
                    negative.assert_called_once()
                    self.assertIn("i64 16,", negative.call_args.kwargs["ir"])
                else:
                    with self.assertRaisesRegex(
                        ValueError, "inventory evidence|did not reconcile|original data registration"
                    ):
                        checker.absent_metadata_controls()
                    negative.assert_not_called()
                    if fault != "missing-call":
                        self.assertEqual(checker.positive_outcomes, [])
                    self.assertFalse((checker.output / "qualification.json").exists())
            mutated = (checker.output / "absent-profile-group.ll").read_text()
            self.assertIn(program, mutated)
            if fault != "missing-call":
                self.assertIn(call, mutated)
            for prefix in ("__profd_", "__profc_", "__llvm_prf_unifcnt_"):
                self.assertIn(prefix + "_Z5probej", mutated)

    # Purpose: Keep negative controls sensitive to execution failure, the expected diagnostic and publication.
    # Inputs: Independent status changes plus a real unexpected output file. Outputs: Raw reports retained on refusal.
    def test_native_rejection_requires_complete_refusal_without_publication(self):
        for fault in (
            None,
            "exit_code",
            "timed_out",
            "output_limit_exceeded",
            "output_truncated",
            "stderr",
            "publication",
        ):
            checker = self.native_checker("rejection-" + (fault or "valid"))
            result = {
                "exit_code": 1,
                "timed_out": False,
                "output_limit_exceeded": False,
                "output_truncated": False,
                "stderr": "independent expected refusal",
            }
            if fault in result:
                result[fault] = {"exit_code": 0, "stderr": "unrelated failure"}.get(fault, True)

            # Purpose: Control only external execution and its artifact. Inputs: Actual argv/bounds. Outputs: Result.
            def execute(command, *, timeout_seconds, max_output_bytes, fault=fault, result=result):
                self.assertEqual((timeout_seconds, max_output_bytes), (30, 65536))
                self.assertEqual(Path(command[command.index("--host") + 1]).read_text(), "malformed fixture")
                if fault == "publication":
                    Path(command[command.index("-o") + 1]).write_bytes(b"unexpected publication")
                return result

            with self.subTest(fault=fault), patch.object(coverage, "process_owner") as owner:
                owner.return_value.run_bounded_command.side_effect = execute
                if fault is None:
                    checker.rejected("case", "expected refusal", ir="malformed fixture")
                    self.assertEqual(checker.outcomes, ["case"])
                else:
                    with self.assertRaisesRegex(ValueError, "rejection contract failed"):
                        checker.rejected("case", "expected refusal", ir="malformed fixture")
                    self.assertEqual(checker.outcomes, [])
            retained = json.loads((checker.output / "case/process.json").read_text())
            self.assertEqual({key: retained[key] for key in result}, result)

    # Purpose: Refuse incompatible compiler fixtures before a control can claim that it altered its intended boundary.
    # Inputs: Missing, partial and ambiguous registration declarations. Outputs: Explicit failure and no final receipt.
    def test_native_control_fixtures_require_unambiguous_compiler_structure(self):
        arguments = ", ptr @name, ptr @name, i32 0, i64 8, i32 0, i32 0)"
        data = "  call void @__hipRegisterVar(ptr %0, ptr @__llvm_profile_shadow_data_probe" + arguments
        table = "  call void @__hipRegisterVar(ptr %0, ptr @__llvm_profile_sections_0123" + arguments
        groups = "\n".join(
            "  call void @__llvm_profile_offload_register_section_shadow_variable(i32 " + str(index) + ")"
            for index in range(4)
        )
        cases = (
            ("namespace_controls", "", "translation-unit identity"),
            ("absent_metadata_controls", "", "profile data identity"),
            ("absent_metadata_controls", '@1 = private constant [10 x i8] c"__profd_x\\00"', "profile name tuple"),
            ("host_group_controls", groups.splitlines()[0], "four-section registration group"),
            ("host_group_controls", groups, "table identity"),
            ("host_group_controls", groups + "\n" + table, "immutable profile data name"),
            ("host_abi_controls", data + "\n" + table, "expected pointer shadow"),
        )
        for index, (method, fixture, diagnostic) in enumerate(cases):
            checker = self.native_checker("shape-" + str(index))
            checker.text = fixture
            with (
                self.subTest(method=method, diagnostic=diagnostic),
                patch.object(checker, "rejected"),
                patch.object(native_checks, "native_metadata") as metadata,
                self.assertRaisesRegex(ValueError, diagnostic),
            ):
                getattr(checker, method)()
            metadata.assert_not_called()
            self.assertFalse((checker.output / "qualification.json").exists())
        for index, fixture in enumerate(("", data + "\n" + data)):
            checker = self.native_checker("ambiguous-call-" + str(index))
            checker.text = fixture
            with self.assertRaisesRegex(ValueError, "single-kernel"):
                checker.change_call(lambda line: line.replace("i64 8", "i64 16"))
            self.assertEqual(checker.text, fixture)

    # Purpose: Detect coverage shadow collisions or omissions even when the producer reports valid inventory counts.
    # Inputs: Independent symbol sets from two emitted translation units. Outputs: No positive namespace verdict.
    def test_native_namespace_control_rejects_overlapping_or_missing_shadows(self):
        for fault in ("collision", "missing"):
            checker = self.native_checker("namespace-" + fault)
            checker.text = "@__llvm_profile_sections_0123 = global ptr null\n"
            (checker.output / "completed.bc").write_bytes(b"original completed fixture")

            # Purpose: Supply actual compiler output text. Inputs: Typed argv and stage. Outputs: Owned symbol fixture.
            def emit(command, log, fault=fault):
                second = log.stem == "namespace-1"
                count = 2 if second and fault == "missing" else 3
                start = 4 if second and fault == "missing" else 1
                text = "\n".join(
                    "@__flitzzip_hip_profile_shadow_" + format(index, "064x") + " = global ptr null"
                    for index in range(start, start + count)
                )
                Path(command[command.index("-o") + 1]).write_text(text)

            with (
                self.subTest(fault=fault),
                patch.object(native_checks, "native_metadata", return_value=copy.deepcopy(self.completion)),
                patch.object(coverage, "checked", side_effect=emit),
                self.assertRaisesRegex(ValueError, "shadows collide"),
            ):
                checker.namespace_controls()
            self.assertEqual(checker.positive_outcomes, [])

    # Purpose: Frame a minimal ELF64 section table independently of LLVM and the production fixture mutator.
    # Inputs: None. Outputs: Mutable bytes with null, text and string-table sections; no executable device code.
    def section_table_fixture(self):
        names = b"\0.text\0.shstrtab\0"
        data = bytearray(256) + names
        data[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<Q", data, 40, 64)
        struct.pack_into("<HHH", data, 58, 64, 3, 2)
        struct.pack_into("<I", data, 128, 1)
        struct.pack_into("<I", data, 192, 7)
        struct.pack_into("<QQ", data, 216, 256, len(names))
        return data

    # Purpose: Prevent fixture generation from inventing a valid metadata-budget control from incompatible image bytes.
    # Inputs: Independently framed table geometry and a wrong native inventory response.
    # Outputs: Rejection before excessive-section generation, with original bytes preserved and no positive verdict.
    def test_native_metadata_budget_control_requires_compatible_image_and_inventory(self):
        for fault in ("signature", "geometry", "no-text", "strings-extent", "inventory"):
            checker = self.native_checker("elf-" + fault)
            data = self.section_table_fixture()
            if fault == "signature":
                data[0] = 0
            elif fault == "geometry":
                struct.pack_into("<H", data, 58, 63)
            elif fault == "no-text":
                data[257:262] = b".data"
            elif fault == "strings-extent":
                struct.pack_into("<Q", data, 224, len(data) + 1)
            checker.device = checker.output / "fixture.elf"
            checker.device.write_bytes(data)
            with (
                self.subTest(fault=fault),
                patch.object(
                    coverage,
                    "checked",
                    return_value={
                        "stdout": json.dumps(
                            {
                                "device_data_records": 3,
                                "registered_data_records": 1,
                            }
                        )
                    },
                ) as native,
                self.assertRaisesRegex(ValueError, "ELF64|geometry|text section|profile admission"),
            ):
                checker.metadata_budget_controls()
            self.assertEqual(native.call_count, 1 if fault == "inventory" else 0)
            self.assertEqual(checker.device.read_bytes(), data)
            self.assertFalse((checker.output / "excessive-sections.elf").exists())
            self.assertEqual(checker.positive_outcomes, [])

    # Purpose: Reject a stale overall verdict even when every individual native control has returned successfully.
    # Inputs: Real source mutation after the last controlled native stage. Outputs: Failure with raw artifacts retained.
    def test_native_control_run_does_not_publish_after_input_drift(self):
        checker = self.native_checker("input-drift")
        (self.root / "llvm-objcopy.exe").write_bytes(b"independent objcopy identity")
        stages = (
            "valid_controls",
            "namespace_controls",
            "absent_metadata_controls",
            "host_abi_controls",
            "host_group_controls",
            "device_controls",
            "metadata_budget_controls",
        )

        # Purpose: Model the compiler's owned output. Inputs: Real command/log. Outputs: Readable probe bytes.
        def render(command, log):
            Path(command[command.index("-o") + 1]).write_text("independent rendered module")

        with ExitStack() as stack:
            stack.enter_context(patch.object(native_checks, "SOURCE_DIRECTORY", self.native_source))
            stack.enter_context(patch.object(native_checks, "admit_tool"))
            stack.enter_context(patch.object(coverage, "checked", side_effect=render))
            mocks = [stack.enter_context(patch.object(checker, name)) for name in stages]
            mocks[-1].side_effect = lambda: self.source.write_bytes(b"changed during final native control")
            with self.assertRaisesRegex(ValueError, "inputs changed during static checks"):
                checker.run()
            for control in mocks:
                control.assert_called_once_with()
        self.assertFalse((checker.output / "qualification.json").exists())
        self.assertEqual(self.source.read_bytes(), b"changed during final native control")
        self.assertEqual(self.tool.read_bytes(), b"native tool identity")
        self.assertEqual((checker.output / "host.ll").read_text(), "independent rendered module")


class HipToolchainTests(unittest.TestCase):
    # Purpose: Own independent manifest mutations. Inputs: Current public pin. Outputs: A copied stable value record.
    def setUp(self):
        self.lock = json.loads(toolchain.LOCK.read_bytes())

    # Purpose: Reject silent toolchain/role drift. Inputs: Foreign origin, schema, ABI, types and malformed bytes.
    # Outputs: Only exact supported typed provenance is admitted; no compiler or download is invoked.
    def test_strict_provenance_types_roles_and_extent(self):
        self.assertEqual(toolchain.parse_toolchain(json.dumps(self.lock).encode()), self.lock)
        for key, value in (
            ("schema_version", True),
            ("llvm_major", 25),
            ("llvm_major", 24.0),
            ("profile_record_bytes", 64),
            ("host", "linux-x64"),
            ("device", "host-only"),
            ("revision", "short"),
            ("repository", "https://example.invalid/llvm"),
            ("source_url", "https://example.invalid/source"),
            ("unknown", 1),
        ):
            altered = {**self.lock, key: value}
            with self.subTest(key=key), self.assertRaises(ValueError):
                toolchain.parse_toolchain(json.dumps(altered).encode())
        for payload in (b"", b"x" * 4097, b"\xff", b"[]", b'{"x":1,"x":2}'):
            with self.subTest(payload=payload[:20]), self.assertRaises(ValueError):
                toolchain.parse_toolchain(payload)

    # Purpose: Reject unpinned, foreign or malformed compressed-profile dependencies before native configuration.
    # Inputs: Independent official-source metadata mutations. Outputs: Typed refusal; no package is downloaded.
    def test_compressed_profile_dependency_is_pinned_and_bounded(self):
        for key, value in (
            ("version", "next"),
            ("url", "https://example.invalid/archive.tar.gz"),
            ("bytes", True),
            ("bytes", 0),
            ("bytes", 8 * 1024**2 + 1),
            ("sha256", "unverified"),
            ("unknown", 1),
        ):
            altered = copy.deepcopy(self.lock)
            altered["zlib"][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                toolchain.parse_toolchain(json.dumps(altered).encode())
        for dependency in (None, [], {}, "system"):
            with self.subTest(dependency=dependency), self.assertRaises(ValueError):
                toolchain.parse_toolchain(json.dumps({**self.lock, "zlib": dependency}).encode())

    # Purpose: Keep CMake input scalar, validated and independent of installation paths.
    # Inputs: Owned public manifest and each supported CLI field. Outputs: Exact revision or major only.
    def test_cmake_metadata_cli_and_missing_input(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "provenance.json"
            path.write_text(json.dumps(self.lock), encoding="utf-8")
            for field in ("revision", "llvm_major"):
                with (
                    patch.object(sys, "argv", ["toolchain", "--lock", str(path), "--field", field]),
                    redirect_stdout(io.StringIO()) as output,
                ):
                    self.assertEqual(toolchain.main(), 0)
                self.assertEqual(output.getvalue().strip(), str(self.lock[field]))
            with self.assertRaises(FileNotFoundError):
                toolchain.read_toolchain(path.with_name("absent.json"))

    # Purpose: Prevent public dependency metadata admission from accepting source code, stale bytes or foreign roles.
    # Inputs: One exact reviewed public manifest/value/region and independently corrupted provenance.
    # Outputs: Metadata-only match; source paths and every changed or malformed role stay unresolved.
    def test_public_commit_admission_is_metadata_only_and_role_bound(self):
        payload = json.dumps(self.lock, indent=2).encode()
        row = {
            "path": metadata_review.TOOLCHAIN_COMMIT_PATH,
            "rule": "DS173237",
            "input_sha256": hashlib.sha256(payload).hexdigest(),
            "public_value": self.lock["revision"],
            "value_kind": "git_commit",
            "evidence": "docs/security-code-scanning.md#finding-triage",
        }
        quoted = '"' + row["public_value"] + '"'
        region = {"charOffset": payload.decode().index(quoted), "charLength": len(quoted)}
        output = io.StringIO()
        writer = csv.DictWriter(output, fieldnames=metadata_review.TYPED_FIELDS)
        writer.writeheader()
        writer.writerow(row)
        self.assertEqual(metadata_review.read_reviews(output.getvalue().encode()), [row])
        self.assertTrue(metadata_review.public_value_matches(row, payload, region))
        self.assertFalse(metadata_review.public_value_matches(row, payload + b"\n", region))
        self.assertFalse(metadata_review.public_value_matches(row, payload, {**region, "charOffset": 0}))
        for field, value in (
            ("repository", "https://example.invalid/llvm"),
            ("profile_record_bytes", 64),
            ("source_url", "https://example.invalid/commit"),
            ("schema_version", True),
        ):
            altered = json.dumps({**self.lock, field: value}, indent=2).encode()
            changed = {**row, "input_sha256": hashlib.sha256(altered).hexdigest()}
            changed_region = {**region, "charOffset": altered.decode().index(quoted)}
            self.assertFalse(metadata_review.public_value_matches(changed, altered, changed_region))
        output = io.StringIO()
        writer = csv.DictWriter(output, fieldnames=metadata_review.TYPED_FIELDS)
        writer.writeheader()
        writer.writerow({**row, "path": "tools/hip_coverage_registration.py"})
        with self.assertRaisesRegex(ValueError, "code and tests remain blocking"):
            metadata_review.read_reviews(output.getvalue().encode())


if __name__ == "__main__":
    unittest.main()
