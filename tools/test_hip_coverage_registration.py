"""Offline failure-boundary contracts for the native HIP profiling registration repair."""

import copy
import csv
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

from tools import hip_coverage as coverage
from tools import hip_coverage_registration as registration
from tools import hip_coverage_toolchain as toolchain
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
            def native(command, log, timeout=120, fault=fault, tool_identity=tool_identity):
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
            def metadata(command, log, timeout=60):
                return json.loads(native(command, log, timeout)["stdout"])

            with (
                self.subTest(fault=fault),
                patch.object(registration, "SOURCE_DIRECTORY", self.native_source),
                patch.object(coverage, "PROBE", self.source),
                patch.object(coverage, "compile_probe", side_effect=compile_baseline) as compiler,
                patch.object(coverage, "checked", side_effect=native),
                patch.object(registration, "native_metadata", side_effect=metadata),
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
