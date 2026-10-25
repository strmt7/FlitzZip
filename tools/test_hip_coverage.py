"""Independent negative controls for real device counter qualification."""

import copy
import ctypes
import io
import itertools
import json
import os
import runpy
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import MagicMock, Mock, patch

from tools import hip_coverage as coverage
from tools import hip_coverage_export as capture
from tools.native_coverage import process_owner


class HipCoverageTests(unittest.TestCase):
    # Purpose: Require both successful Windows job inspection and confirmed containment before exporting.
    # Inputs: Explicit API failure, uncontained success and contained success through a controlled Win32 boundary.
    # Outputs: Only confirmed containment passes; every failure prevents launching the export descendant.
    def test_export_job_admission_requires_positive_native_evidence(self):
        for returned, contained, accepted in ((0, 0, False), (0, 1, False), (1, 0, False), (1, 1, True)):
            # Purpose: Model the OS result. Inputs: Native arguments and output pointer. Outputs: Status/value.
            def inspect(handle, job, pointer, returned=returned, contained=contained):
                self.assertEqual(handle, 123)
                self.assertIsNone(job)
                ctypes.cast(pointer, ctypes.POINTER(ctypes.c_int)).contents.value = contained
                return returned

            kernel = Mock()
            kernel.GetCurrentProcess.return_value = 123
            kernel.IsProcessInJob.side_effect = inspect
            with (
                self.subTest(returned=returned, contained=contained),
                patch.object(capture.ctypes, "WinDLL", return_value=kernel, create=True),
                patch.object(capture.subprocess, "Popen") as child,
            ):
                if accepted:
                    capture.require_job()
                else:
                    with self.assertRaisesRegex(RuntimeError, "inherited Windows job"):
                        capture.capture(self.source, self.source, self.source, self.root / "export.json")
                child.assert_not_called()

    # Purpose: Own small tool and source fixtures without invoking a compiler or GPU.
    # Inputs: None. Outputs: Cleanup-bound files and independently framed LLVM control reports.
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root / "control.hip.cpp"
        self.source.write_text("// control\nif (selector != 0) {\n}\nif (selector == 0) {\n}\n", encoding="utf-8")
        self.reports = []
        for counts, invocations in (((0, 192), 192), ((192, 0), 192), ((192, 192), 384)):
            self.reports.append(
                {
                    "type": "llvm.coverage.json.export",
                    "version": "3.1.0",
                    "data": [
                        {
                            "functions": [
                                {
                                    "name": "_Z22coverage_counter_probePii",
                                    "count": invocations,
                                    "filenames": [str(self.source)],
                                    "branches": [[2, 5, 2, 18, *counts, 0, 0, 4]],
                                },
                                {
                                    "name": "_Z21coverage_helper_probei",
                                    "count": invocations,
                                    "filenames": [str(self.source)],
                                    "branches": [[4, 5, 4, 18, *reversed(counts), 0, 0, 4]],
                                },
                            ]
                        }
                    ],
                }
            )

    # Purpose: Establish expected branch counts with an independent, deliberately missed branch.
    # Inputs: Three hand-framed reports. Outputs: False, true and merged branch counts exactly preserved.
    def test_opposite_controls_and_merge(self):
        self.assertEqual(
            coverage.validate_controls(self.reports, self.source),
            {
                "false_counts": [0, 192],
                "true_counts": [192, 0],
                "merged_counts": [192, 192],
                "helper_false_counts": [192, 0],
                "helper_true_counts": [0, 192],
                "helper_merged_counts": [192, 192],
            },
        )

    # Purpose: Reject inflated, swapped, absent and stale evidence before declaring device coverage qualified.
    # Inputs: Independent corruptions of valid counter records. Outputs: Every corruption fails explicitly.
    def test_false_positive_qualification_is_rejected(self):
        mutations = (
            lambda reports: reports.pop(),
            lambda reports: reports.reverse(),
            lambda reports: reports[0].update(type="host-dispatch-counts"),
            lambda reports: reports[0].update(version="9.0.0"),
            lambda reports: reports[0].update(data=[]),
            lambda reports: reports[0]["data"][0].update(functions=[]),
            lambda reports: reports[0]["data"][0]["functions"][0].update(name="main"),
            lambda reports: reports[0]["data"][0]["functions"][0].update(count=True),
            lambda reports: reports[0]["data"][0]["functions"][0].update(count=64),
            lambda reports: reports[2]["data"][0]["functions"][0].update(count=1),
            lambda reports: reports[0]["data"][0]["functions"][0].update(filenames=["foreign.cpp"]),
            lambda reports: reports[0]["data"][0]["functions"][0].update(branches=[]),
            lambda reports: reports[0]["data"][0]["functions"][0]["branches"][0].__setitem__(4, 1),
            lambda reports: reports[0]["data"][0]["functions"][0]["branches"][0].__setitem__(5, -1),
            lambda reports: reports[0]["data"][0]["functions"][0]["branches"][0].__setitem__(5, True),
            lambda reports: reports[0]["data"][0]["functions"][0]["branches"][0].__setitem__(0, 20),
            lambda reports: reports[0]["data"][0]["functions"][0]["branches"][0].__setitem__(0, 0),
            lambda reports: reports[0]["data"][0]["functions"][0]["branches"][0].__setitem__(0, 1),
        )
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                changed = copy.deepcopy(self.reports)
                mutate(changed)
                with self.assertRaises(ValueError):
                    coverage.validate_controls(changed, self.source)

    # Purpose: Prevent target option/path injection and accidentally measuring another architecture.
    # Inputs: Valid targets and malformed alternatives. Outputs: Exactly one literal gfx target is accepted.
    def test_architecture_contract(self):
        for value in ("gfx1201", "gfx1100", "gfx90a"):
            self.assertEqual(coverage.architecture(value), value)
        for value in ("", "release", "gfx1201,gfx1100", "gfx1201\n", "gfx1201 -O3", "../gfx1201", "gfx" * 20):
            with self.subTest(value=value), self.assertRaises(ValueError):
                coverage.architecture(value)

    # Purpose: Reject a profiler that sees kernels but loses executed device helper counters.
    # Inputs: Missing helper records, zero counts, copied kernel branches and mismatched helper mappings.
    # Outputs: Each independently invalid measurement fails qualification without inferred hits.
    def test_helper_measurement_must_be_independently_complete(self):
        for mutation in ("missing", "zero", "copied", "wrong_source_branch", "duplicate"):
            with self.subTest(mutation=mutation):
                reports = copy.deepcopy(self.reports)
                functions = reports[0]["data"][0]["functions"]
                helper = functions[1]
                if mutation == "missing":
                    functions.pop()
                elif mutation == "zero":
                    helper["count"] = 0
                    helper["branches"][0][4:6] = [0, 0]
                elif mutation == "copied":
                    helper["branches"][0][4:6] = functions[0]["branches"][0][4:6]
                elif mutation == "wrong_source_branch":
                    helper["branches"][0][0] = 2
                else:
                    helper["name"] = functions[0]["name"]
                with self.assertRaises(ValueError):
                    coverage.validate_controls(reports, self.source)

    # Purpose: Require independently known helper-and-kernel output before using a device profile.
    # Inputs: Simulated external process results with correct output or the preceding kernel-only output.
    # Outputs: Retains both profiles only for exact readback and rejects the incorrect result before admission.
    def test_control_readback_requires_executed_helper(self):
        for invalid in (False, True):
            with self.subTest(invalid=invalid):
                output = self.root / f"helper-readback-{invalid}"
                output.mkdir()

                # Purpose: Model only the GPU process transport and its raw profile artifact.
                # Inputs: The real command vector and log path. Outputs: Known bytes and independent JSON readback.
                def execute(arguments, log, timeout, *, invalid=invalid):
                    self.assertEqual(timeout, 30)
                    selector = int(len(arguments) == 2)
                    (log.parent / "gfx1201.1.profraw").write_bytes(b"raw-profile-fixture")
                    return {
                        "stdout": json.dumps(
                            {
                                "selector": selector,
                                "value": (23 if selector == 0 else 17) if invalid else (18 if selector == 0 else 22),
                                "lanes": 192,
                                "architecture": "gfx1201",
                            }
                        )
                    }

                with patch.object(coverage, "checked", side_effect=execute):
                    if invalid:
                        with self.assertRaisesRegex(ValueError, "readback"):
                            coverage.run_controls(self.root / "probe.exe", "gfx1201", output)
                    else:
                        profiles = coverage.run_controls(self.root / "probe.exe", "gfx1201", output)
                        self.assertEqual(len(profiles), 2)
                        self.assertTrue(all(p.read_bytes() == b"raw-profile-fixture" for p in profiles))

    # Purpose: Ensure failed profiling cannot contaminate later builds through inherited process settings.
    # Inputs: Both an existing variable and a missing one. Outputs: Exact restoration on an exception.
    def test_child_environment_restores_on_failure(self):
        with patch.dict(os.environ, {"FLITZZIP_COVERAGE_TEST_EXISTING": "previous"}):
            os.environ.pop("FLITZZIP_COVERAGE_TEST_MISSING", None)
            with (
                self.assertRaisesRegex(RuntimeError, "control"),
                coverage.child_environment(
                    {"FLITZZIP_COVERAGE_TEST_EXISTING": "new", "FLITZZIP_COVERAGE_TEST_MISSING": "set"}
                ),
            ):
                self.assertEqual(os.environ["FLITZZIP_COVERAGE_TEST_EXISTING"], "new")
                raise RuntimeError("control")
            self.assertEqual(os.environ["FLITZZIP_COVERAGE_TEST_EXISTING"], "previous")
            self.assertNotIn("FLITZZIP_COVERAGE_TEST_MISSING", os.environ)

    # Purpose: Keep failed device measurements attributable after the working source changes.
    # Inputs: Independent compiler/binary fixtures and failures at image, GPU, export or counter boundaries.
    # Outputs: Original input identities and source survive every failure; no qualification pass is published.
    def test_failed_qualification_retains_compiled_input_evidence(self):
        original = self.source.read_bytes()
        inputs = coverage.identities([self.source])
        executable = self.root / "probe.exe"
        executable.write_bytes(b"MZ-independent-compiler-fixture")
        image = self.root / "device.elf"
        image.write_bytes(b"independent-image-fixture")
        profile = self.root / "device.profraw"
        profile.write_bytes(b"independent-profile-fixture")
        for failure in ("image", "gpu", "export", "counters"):
            with self.subTest(failure=failure):
                output = self.root / failure
                with (
                    patch.object(coverage, "ROOT", self.root),
                    patch.object(coverage, "PROBE", self.source),
                    patch.object(coverage, "compile_probe", return_value=(executable, inputs)),
                    patch.object(coverage, "device_image", return_value=image) as extract,
                    patch.object(coverage, "run_controls", return_value=[profile]) as execute,
                    patch.object(coverage, "export_profile", return_value={}) as export,
                    patch.object(coverage, "validate_controls", return_value={}) as validate,
                ):
                    stage = {"image": extract, "gpu": execute, "export": export, "counters": validate}[failure]
                    stage.side_effect = RuntimeError("independent " + failure + " failure")
                    with self.assertRaisesRegex(RuntimeError, "independent " + failure + " failure"):
                        coverage.qualify(self.root, self.root, self.source, "gfx1201", output)
                record = json.loads((output / "measurement-inputs.json").read_text(encoding="utf-8"))
                self.assertEqual(record["qualification"], "not-qualified")
                self.assertEqual(record["inputs"], inputs)
                self.assertEqual(record["artifacts"], coverage.identities([executable]))
                self.assertEqual(record["architecture"], "gfx1201")
                self.assertFalse((output / "qualification.json").exists())
                self.source.write_bytes(b"working source changed after the failed measurement")
                self.assertEqual((output / "probe-source.hip.cpp").read_bytes(), original)
                self.source.write_bytes(original)

    # Purpose: Reject a probe changed between capture and the compiler's own input snapshot.
    # Inputs: A compiler transport fixture that changes source before returning its compiled identity.
    # Outputs: Frozen original bytes remain available, and no device execution or pass receipt is possible.
    def test_probe_capture_rejects_compilation_source_drift(self):
        original = self.source.read_bytes()
        executable = self.root / "probe.exe"
        executable.write_bytes(b"independent-binary-fixture")
        output = self.root / "source-drift"

        # Purpose: Model source replacement before the compiler records the bytes it consumes.
        # Inputs: Unused compiler command metadata. Outputs: Binary and a different actual source identity.
        def compile_changed(*_arguments):
            self.source.write_bytes(b"changed before compilation")
            return executable, coverage.identities([self.source])

        with (
            patch.object(coverage, "ROOT", self.root),
            patch.object(coverage, "PROBE", self.source),
            patch.object(coverage, "compile_probe", side_effect=compile_changed),
            patch.object(coverage, "device_image") as execute,
            self.assertRaisesRegex(ValueError, "between evidence capture and compilation"),
        ):
            coverage.qualify(self.root, self.root, self.source, "gfx1201", output)
        execute.assert_not_called()
        self.assertEqual((output / "probe-source.hip.cpp").read_bytes(), original)
        self.assertFalse((output / "qualification.json").exists())

    # Purpose: Bind accepted counters to the exact source and executable that produced them.
    # Inputs: Both compiler lanes, independent counter reports, and source/binary mutation after validation.
    # Outputs: Only the selected compiler runs; unchanged inputs pass and mutations retain unqualified evidence.
    def test_qualification_rechecks_source_and_binary_before_acceptance(self):
        original = self.source.read_bytes()
        executable = self.root / "probe.exe"
        image = self.root / "device.elf"
        profiles = [self.root / "false.profraw", self.root / "true.profraw"]
        image.write_bytes(b"independent-image-fixture")
        for profile in profiles:
            profile.write_bytes(b"independent-profile-fixture")
        registration_tool = self.root / "registration-reader.exe"
        registration_tool.write_bytes(b"nonexecutable registration transport fixture")
        validate_original = coverage.validate_controls
        for registered, mutation in itertools.product((False, True), ("source", "binary", "none")):
            with self.subTest(registered=registered, mutation=mutation):
                self.source.write_bytes(original)
                executable.write_bytes(b"independent-binary-fixture")
                inputs = coverage.identities([self.source])
                output = self.root / f"{mutation}-{registered}"
                options = {"registration_tool": registration_tool} if registered else {}

                # Purpose: Inject mutation only after real independent counter validation succeeds.
                # Inputs: Actual reports and probe path; a frozen mutation choice. Outputs: Counter evidence.
                def validate_and_mutate(reports, source, *, mutation=mutation):
                    result = validate_original(reports, source)
                    if mutation != "none":
                        (self.source if mutation == "source" else executable).write_bytes(b"changed after execution")
                    return result

                with (
                    patch.object(coverage, "ROOT", self.root),
                    patch.object(coverage, "PROBE", self.source),
                    patch.object(coverage, "compile_probe", return_value=(executable, inputs)) as direct,
                    patch(
                        "tools.hip_coverage_registration.compile_registered_probe", return_value=(executable, inputs)
                    ) as registration,
                    patch.object(coverage, "device_image", return_value=image),
                    patch.object(coverage, "run_controls", return_value=profiles),
                    patch.object(coverage, "export_profile", side_effect=copy.deepcopy(self.reports)),
                    patch.object(coverage, "validate_controls", side_effect=validate_and_mutate),
                ):
                    if mutation == "none":
                        result = coverage.qualify(self.root, self.root, self.source, "gfx1201", output, **options)
                        self.assertEqual(result["qualification"], "passed")
                        self.assertEqual(result["inputs"], inputs)
                        self.assertIn(str((output / "probe-source.hip.cpp").resolve()), result["artifacts"])
                    else:
                        with self.assertRaisesRegex(ValueError, "inputs or measured binary changed"):
                            coverage.qualify(self.root, self.root, self.source, "gfx1201", output, **options)
                        self.assertFalse((output / "qualification.json").exists())
                    pending = json.loads((output / "measurement-inputs.json").read_text(encoding="utf-8"))
                    self.assertEqual(pending["qualification"], "not-qualified")
                    self.assertEqual(pending["inputs"], inputs)
                    self.assertEqual((output / "probe-source.hip.cpp").read_bytes(), original)
                    if registered:
                        direct.assert_not_called()
                        registration.assert_called_once_with(
                            self.root, self.root, self.source, registration_tool, "gfx1201", output
                        )
                    else:
                        registration.assert_not_called()
                        direct.assert_called_once_with(self.root, self.root, self.source, "gfx1201", output)

    # Purpose: Detect changed binary/profile bytes and refuse missing or redirected evidence.
    # Inputs: Small owned file with successive values. Outputs: Distinct identities and explicit invalid-path refusal.
    def test_file_identity_and_missing_input(self):
        before = coverage.identities([self.source])
        self.source.write_text("changed\n", encoding="utf-8")
        self.assertNotEqual(before, coverage.identities([self.source]))
        for path in (self.root, self.root / "absent"):
            with self.assertRaises(ValueError):
                coverage.identities([path])
        with (
            patch.object(coverage, "reject_link_chain", side_effect=ValueError("redirected")),
            self.assertRaisesRegex(ValueError, "redirected"),
        ):
            coverage.identities([self.source])

    # Purpose: Preserve native diagnostics for timeout, error and truncated-output failures instead of admitting a pass.
    # Inputs: Independent bounded-process outcomes. Outputs: Failed commands retain exact evidence and throw.
    def test_native_failures_retain_diagnostics(self):
        success = {
            "exit_code": 0,
            "timed_out": False,
            "output_limit_exceeded": False,
            "output_truncated": False,
            "stdout": "output",
            "stderr": "diagnostic",
        }
        for field, value in (
            ("exit_code", 2),
            ("timed_out", True),
            ("output_limit_exceeded", True),
            ("output_truncated", True),
        ):
            with self.subTest(field=field):
                failed = {**success, field: value}
                owner = Mock()
                owner.run_bounded_command.return_value = failed
                with patch.object(coverage, "process_owner", return_value=owner):
                    log = self.root / f"{field}.json"
                    with self.assertRaises(RuntimeError):
                        coverage.checked(["control"], log)
                    self.assertEqual(json.loads(log.read_text())[field], value)
        owner.run_bounded_command.return_value = success
        with patch.object(coverage, "process_owner", return_value=owner):
            self.assertEqual(coverage.checked(["control"], self.root / "success.json"), success)

    # Purpose: Prevent zero-profile execution from yielding a misleading empty success report.
    # Inputs: Empty profile list. Outputs: Refusal before invoking any external tool.
    def test_empty_profiles_are_not_coverage(self):
        with patch.object(coverage, "checked") as native:
            with self.assertRaises(ValueError):
                coverage.export_profile(self.root, self.source, [], self.root)
            native.assert_not_called()

    # Purpose: Verify exact profile merge/export framing and parse the retained native LLVM JSON artifact.
    # Inputs: One nonempty owned raw profile, device image and a child double that writes the export destination.
    # Outputs: Both bounded commands receive owned paths and the exact native report is returned unchanged.
    def test_profile_export_uses_owned_native_artifact(self):
        profile = self.root / "device.profraw"
        image = self.root / "device.elf"
        profile.write_bytes(b"nonempty raw profile")
        image.write_bytes(b"device image fixture")
        expected = {"type": "llvm.coverage.json.export", "version": "3.1.0", "data": []}

        def invoke(command, _log, _timeout=120):
            if "tools.hip_coverage_export" in command:
                Path(command[command.index("--output") + 1]).write_text(json.dumps(expected), encoding="utf-8")
            return {"stdout": "", "exit_code": 0}

        with patch.object(coverage, "checked", side_effect=invoke) as native:
            self.assertEqual(coverage.export_profile(self.root, image, [profile], self.root), expected)
        self.assertEqual(native.call_count, 2)
        self.assertIn(str(profile), native.call_args_list[0].args[0])
        self.assertIn("tools.hip_coverage_export", native.call_args_list[1].args[0])

    # Purpose: Reject host-only or ambiguous image evidence before any device counter can be interpreted.
    # Inputs: Independently framed ELF headers and bundle listings. Outputs: Only the exact AMDGPU image is admitted.
    def test_device_image_admission(self):
        valid_header = bytearray(20)
        valid_header[:6] = b"\x7fELF\x02\x01"
        valid_header[18:20] = (224).to_bytes(2, "little")
        bundle = "hip-amdgcn-amd-amdhsa--gfx1201"
        fixtures = (
            (bundle, bytes(valid_header), True),
            (bundle, b"MZ" + bytes(18), False),
            (bundle, bytes(valid_header[:8]), False),
            (bundle + "\n" + bundle, bytes(valid_header), False),
            ("host-x86_64-pc-windows-msvc-", bytes(valid_header), False),
            ("hip-amdgcn-amd-amdhsa--gfx1100", bytes(valid_header), False),
        )
        for index, (listing, header, admitted) in enumerate(fixtures):
            with self.subTest(index=index):
                output = self.root / str(index)
                output.mkdir()
                (output / "image.hipfb").write_bytes(b"bounded native bundle fixture")
                (output / "device.elf").write_bytes(header)
                with patch.object(coverage, "checked", return_value={"stdout": listing}):
                    if admitted:
                        self.assertEqual(
                            coverage.device_image(self.root, self.source, "gfx1201", output), output / "device.elf"
                        )
                    else:
                        with self.assertRaises(ValueError):
                            coverage.device_image(self.root, self.source, "gfx1201", output)
        absent = self.root / "absent-image"
        absent.mkdir()
        with (
            patch.object(coverage, "checked", return_value={"stdout": bundle}),
            self.assertRaisesRegex(ValueError, "no HIP device image"),
        ):
            coverage.device_image(self.root, self.source, "gfx1201", absent)

    # Purpose: Require actual device identity and correct results independently from the instrumenter's exit code.
    # Inputs: Successful process records with incorrect output, target or missing device counters.
    # Outputs: Every invalid run is rejected before a second GPU operation can be selected.
    def test_runtime_readback_and_profiles_are_required(self):
        fixtures = (
            {"selector": 0, "value": 17, "lanes": 192, "architecture": "gfx1201"},
            {"selector": 0, "value": 23, "lanes": 192, "architecture": "gfx1100"},
            {"selector": 0, "value": 23, "lanes": 192, "architecture": "gfx1201"},
        )
        for index, observed in enumerate(fixtures):
            with self.subTest(index=index):
                output = self.root / str(index)
                output.mkdir()
                with patch.object(coverage, "checked", return_value={"stdout": json.dumps(observed)}) as native:
                    with self.assertRaises(ValueError):
                        coverage.run_controls(self.source, "gfx1201", output)
                    self.assertEqual(native.call_count, 1)
        output = self.root / "missing-profile"
        output.mkdir()
        expected = {"selector": 0, "value": 18, "lanes": 192, "architecture": "gfx1201"}
        with (
            patch.object(coverage, "checked", return_value={"stdout": json.dumps(expected)}),
            self.assertRaisesRegex(ValueError, "exactly one nonempty device profile"),
        ):
            coverage.run_controls(self.source, "gfx1201", output)

    # Purpose: Admit one exact compiler/runtime closure and reject missing backends or mid-compile input drift.
    # Inputs: Owned directory geometry with mocked bounded compiler commands and immutable/mutated identity maps.
    # Outputs: Missing runtimes/backends fail early; stable commands return the probe path; drift never qualifies.
    def test_compile_probe_toolchain_and_drift_contracts(self):
        compiler = self.root / "compiler"
        sdk = self.root / "sdk"
        runtime = self.root / "device-runtime.obj"
        output = self.root / "compile-output"
        (compiler / "bin").mkdir(parents=True)
        (compiler / "lib/clang/20/lib/windows").mkdir(parents=True)
        (compiler / "amdgcn/bitcode").mkdir(parents=True)
        (sdk / "bin").mkdir(parents=True)
        (sdk / "lib").mkdir()
        output.mkdir()
        runtime.write_bytes(b"device runtime")
        with self.assertRaisesRegex(ValueError, "profiling runtime"):
            coverage.compile_probe(compiler, sdk, runtime, "gfx1201", output)
        profile = compiler / "lib/clang/20/lib/windows/clang_rt.profile_rocm-x86_64.lib"
        profile.write_bytes(b"host profile")
        with self.assertRaisesRegex(ValueError, "HIP runtime library"):
            coverage.compile_probe(compiler, sdk, runtime, "gfx1201", output)
        (sdk / "bin/amdhip64.dll").write_bytes(b"HIP runtime")
        stable = {"input": {"bytes": 1, "sha256": "a" * 64}}
        with (
            patch.object(coverage, "identities", return_value=stable),
            patch.object(coverage, "checked", return_value={"stdout": "  x86 - host"}),
            self.assertRaisesRegex(ValueError, "no AMDGPU backend"),
        ):
            coverage.compile_probe(compiler, sdk, runtime, "gfx1201", output)
        commands = ({"stdout": "  amdgcn - AMD GPU"}, {"stdout": "clang fixture"}, {"stdout": ""})
        with (
            patch.object(coverage, "identities", side_effect=[stable, stable]),
            patch.object(coverage, "checked", side_effect=commands) as native,
        ):
            executable, observed = coverage.compile_probe(compiler, sdk, runtime, "gfx1201", output)
        self.assertEqual(executable, output / "probe.exe")
        self.assertEqual(observed, stable)
        self.assertEqual(native.call_count, 3)
        self.assertIn("--offload-arch=gfx1201", native.call_args_list[2].args[0])
        with (
            patch.object(coverage, "identities", side_effect=[stable, {"changed": {}}]),
            patch.object(coverage, "checked", side_effect=commands),
            self.assertRaisesRegex(ValueError, "changed during compilation"),
        ):
            coverage.compile_probe(compiler, sdk, runtime, "gfx1201", output)

    # Purpose: Route explicit CLI paths to qualification and exercise __main__ failure before any native command.
    # Inputs: Owned existing compiler/SDK/runtime paths plus one nonexistent module-entry compiler path.
    # Outputs: Successful routing emits bounded summary JSON; missing input raises before qualification.
    def test_main_routes_explicit_paths_and_module_failure(self):
        compiler, sdk = self.root / "main-compiler", self.root / "main-sdk"
        runtime, output = self.root / "main-runtime.obj", self.root / "main-output"
        compiler.mkdir()
        sdk.mkdir()
        runtime.write_bytes(b"runtime fixture")
        result = {"qualification": "passed", "scope": "device controls", "architecture": "gfx1201", "controls": {}}
        arguments = [
            "hip_coverage",
            "--compiler-root",
            str(compiler),
            "--sdk",
            str(sdk),
            "--device-runtime",
            str(runtime),
            "--arch",
            "gfx1201",
            "--output",
            str(output),
        ]
        with (
            patch("sys.argv", arguments),
            patch.object(coverage, "qualify", return_value=result) as qualify,
            redirect_stdout(io.StringIO()) as emitted,
        ):
            self.assertEqual(coverage.main(), 0)
        qualify.assert_called_once_with(compiler, sdk, runtime, "gfx1201", output)
        self.assertEqual(json.loads(emitted.getvalue()), result)
        arguments[2] = str(self.root / "absent-compiler")
        with patch("sys.argv", arguments), self.assertRaises(FileNotFoundError):
            runpy.run_path(str(Path(coverage.__file__).resolve()), run_name="__main__")

    # Purpose: Preserve COFF object image names rather than silently trying the linked-executable section.
    # Inputs: Explicit object-file selection. Outputs: One exact native extraction request, with no retry.
    def test_coff_object_uses_its_native_section(self):
        with patch.object(coverage, "checked", side_effect=RuntimeError("stop before extraction")) as native:
            with self.assertRaisesRegex(RuntimeError, "stop before extraction"):
                coverage.device_image(self.root, self.source, "gfx1201", self.root, coff_object=True)
            native.assert_called_once()
            self.assertIn("--dump-section=.hip_fatbin=", native.call_args.args[0][1])

    # Purpose: Preserve complete exports beyond the response limit while enforcing an independent disk-byte cap.
    # Inputs: Exact-cap, oversized, empty and failed streams. Outputs: Complete success or bounded rejection.
    def test_native_artifact_bounds_and_exit_status(self):
        for index, (data, status, passed) in enumerate(
            ((b"x" * 32, 0, True), (b"x" * 33, 0, False), (b"", 0, False), (b"x", 2, False))
        ):
            with self.subTest(index=index):
                child = MagicMock()
                child.__enter__.return_value = child
                child.stdout = io.BytesIO(data)
                child.wait.return_value = status
                output = self.root / f"export-{index}.json"
                with (
                    patch.object(capture, "ROOT", self.root),
                    patch.object(capture, "MAX_EXPORT_BYTES", 32),
                    patch.object(capture, "require_job"),
                    patch.object(capture.subprocess, "Popen", return_value=child),
                ):
                    if passed:
                        capture.capture(self.source, self.source, self.source, output)
                        self.assertEqual(output.read_bytes(), data)
                        child.kill.assert_not_called()
                    else:
                        with self.assertRaises((ValueError, RuntimeError)):
                            capture.capture(self.source, self.source, self.source, output)
                        self.assertLessEqual(output.stat().st_size, 32)
                        child.kill.assert_called_once()

    # Purpose: Refuse an export before launch when containment is absent or an artifact would be overwritten.
    # Inputs: Missing job contract and an existing destination. Outputs: No child process and unchanged original bytes.
    def test_export_requires_containment_and_fresh_output(self):
        with (
            patch.object(capture, "require_job", side_effect=RuntimeError("uncontained")),
            patch.object(capture.subprocess, "Popen") as native,
        ):
            with self.assertRaisesRegex(RuntimeError, "uncontained"):
                capture.capture(self.source, self.source, self.source, self.root / "absent")
            native.assert_not_called()
        original = self.source.read_bytes()
        with (
            patch.object(capture, "require_job"),
            patch.object(capture, "ROOT", self.root),
            patch.object(capture.subprocess, "Popen") as native,
        ):
            with self.assertRaises(FileExistsError):
                capture.capture(self.source, self.source, self.source, self.source)
            native.assert_not_called()
            self.assertEqual(self.source.read_bytes(), original)

    # Purpose: Exercise export CLI parsing and missing-input admission without launching a native descendant.
    # Inputs: Four owned path arguments, one absent input and the direct help entry point.
    # Outputs: Exact routing, early missing-input refusal and standard zero-status help.
    def test_export_cli_routes_paths_and_rejects_missing_input(self):
        output = self.root / "cli-export.json"
        arguments = [
            "hip_coverage_export",
            "--tool",
            str(self.source),
            "--image",
            str(self.source),
            "--profile",
            str(self.source),
            "--output",
            str(output),
        ]
        with patch("sys.argv", arguments), patch.object(capture, "capture") as routed:
            self.assertEqual(capture.main(), 0)
        routed.assert_called_once_with(self.source, self.source, self.source, output)

        with (
            patch.object(capture, "ROOT", self.root),
            patch.object(capture, "require_job"),
            self.assertRaisesRegex(ValueError, "Missing LLVM export input"),
        ):
            capture.capture(self.root / "absent", self.source, self.source, output)

        with (
            patch("sys.argv", ["hip_coverage_export", "--help"]),
            patch("sys.stdout", io.StringIO()),
            self.assertRaises(SystemExit) as status,
        ):
            runpy.run_path(str(Path(capture.__file__).resolve()), run_name="__main__")
        self.assertEqual(status.exception.code, 0)


class HipCoverageCommandTests(unittest.TestCase):
    # Purpose: Own real command diagnostics and would-be outputs without installing or selecting a GPU.
    # Inputs: None. Outputs: Cleanup-bound Unicode paths and the canonical bounded process owner.
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="flitzzip-coverage-cli-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name) / "command paths Ω"
        self.directory.mkdir()
        self.owner = process_owner()

    # Purpose: Execute the supported module interface with actual import, parser and exit-status handling.
    # Inputs: Module name and literal arguments. Outputs: Complete bounded diagnostics or a test failure.
    def command(self, module, *arguments):
        result = self.owner.run_bounded_command(
            [sys.executable, "-X", "utf8", "-B", "-m", module, *map(str, arguments)],
            timeout_seconds=15,
            max_output_bytes=65536,
            response_tail_bytes=65536,
            working_directory=coverage.ROOT,
        )
        for field in ("timed_out", "output_truncated", "output_limit_exceeded"):
            self.assertIs(result[field], False, result)
        self.assertNotIn("Traceback", result["stderr"], result)
        return result

    # Purpose: Keep help usable before compiler discovery, device admission or output publication.
    # Inputs: Every public module and subcommand help interface. Outputs: Successful readable help and no files.
    def test_real_module_help_needs_no_compiler_or_device(self):
        cases = (
            ("tools.hip_coverage", (), ("--compiler-root", "--device-runtime", "--arch")),
            ("tools.hip_coverage_toolchain", (), ("--lock", "--field")),
            ("tools.hip_coverage_product", (), ("build", "admit")),
            ("tools.hip_coverage_product", ("build",), ("--qualification", "--output")),
            ("tools.hip_coverage_product", ("admit",), ("--manifest", "--sdk", "--arch")),
            ("tools.hip_coverage_measure", (), ("admit", "build-and-measure")),
            ("tools.hip_coverage_measure", ("admit",), ("--manifest", "--build-root")),
            ("tools.hip_coverage_measure", ("build-and-measure",), ("--filter", "--output")),
        )
        for module, command, options in cases:
            with self.subTest(module=module, command=command):
                result = self.command(module, *command, "--help")
                self.assertEqual(result["exit_code"], 0, result)
                self.assertEqual(result["stderr"], "", result)
                self.assertTrue(result["stdout"].startswith("usage: "), result)
                for option in options:
                    self.assertIn(option, result["stdout"])
                self.assertEqual(list(self.directory.iterdir()), [])

    # Purpose: Reject incomplete automation requests through argparse before any qualification work.
    # Inputs: Missing required arguments for every public command. Outputs: Exit two, clear diagnostics, no files.
    def test_real_commands_reject_incomplete_requests(self):
        cases = (
            ("tools.hip_coverage", (), "--compiler-root"),
            ("tools.hip_coverage_toolchain", (), "--field"),
            ("tools.hip_coverage_product", (), "command"),
            ("tools.hip_coverage_product", ("build",), "--qualification"),
            ("tools.hip_coverage_product", ("admit",), "--manifest"),
            ("tools.hip_coverage_measure", (), "command"),
            ("tools.hip_coverage_measure", ("admit",), "--manifest"),
            ("tools.hip_coverage_measure", ("build-and-measure",), "--filter"),
        )
        for module, arguments, missing in cases:
            with self.subTest(module=module, arguments=arguments):
                result = self.command(module, *arguments)
                self.assertEqual(result["exit_code"], 2, result)
                self.assertEqual(result["stdout"], "", result)
                self.assertIn("error:", result["stderr"])
                self.assertIn(missing, result["stderr"])
                self.assertEqual(list(self.directory.iterdir()), [])

    # Purpose: Reject unknown options even after all mandatory arguments have been supplied.
    # Inputs: Syntactically complete commands with nonexistent inputs and one unknown switch.
    # Outputs: Parser rejection rather than input access, compilation, a traceback or output creation.
    def test_real_commands_reject_unknown_options_before_work(self):
        missing, output = self.directory / "missing input", self.directory / "unused output"
        cases = (
            (
                "tools.hip_coverage",
                (
                    "--compiler-root",
                    missing,
                    "--sdk",
                    missing,
                    "--device-runtime",
                    missing,
                    "--arch",
                    "gfx1100",
                    "--output",
                    output,
                ),
            ),
            ("tools.hip_coverage_toolchain", ("--lock", missing, "--field", "revision")),
            ("tools.hip_coverage_product", ("build", "--qualification", missing, "--output", output)),
            ("tools.hip_coverage_product", ("admit", "--manifest", missing, "--sdk", missing, "--arch", "gfx1100")),
            ("tools.hip_coverage_measure", ("admit", "--manifest", missing, "--build-root", missing)),
            (
                "tools.hip_coverage_measure",
                (
                    "build-and-measure",
                    "--manifest",
                    missing,
                    "--build-root",
                    missing,
                    "--filter",
                    "=unused",
                    "--output",
                    output,
                ),
            ),
        )
        for module, arguments in cases:
            with self.subTest(module=module, arguments=arguments):
                result = self.command(module, *arguments, "--unexpected-option")
                self.assertEqual(result["exit_code"], 2, result)
                self.assertEqual(result["stdout"], "", result)
                self.assertIn("unrecognized arguments: --unexpected-option", result["stderr"])
                self.assertEqual(list(self.directory.iterdir()), [])

    # Purpose: Verify the actual scalar interface consumed by CMake, including an explicit spaced Unicode path.
    # Inputs: The pinned record and an identical private copy. Outputs: Exactly one requested scalar per process.
    def test_real_toolchain_command_emits_only_requested_scalar(self):
        lock = coverage.ROOT / "tools/hip-coverage-toolchain.json"
        copied = self.directory / "explicit provenance.json"
        copied.write_bytes(lock.read_bytes())
        record = json.loads(copied.read_text(encoding="utf-8"))
        for field, options in (("revision", ()), ("llvm_major", ("--lock", copied))):
            with self.subTest(field=field):
                result = self.command("tools.hip_coverage_toolchain", *options, "--field", field)
                self.assertEqual(result["exit_code"], 0, result)
                self.assertEqual(result["stderr"], "", result)
                self.assertEqual(result["stdout"].splitlines(), [str(record[field])])
                self.assertEqual(copied.read_bytes(), lock.read_bytes())
                self.assertEqual(list(self.directory.iterdir()), [copied])


if __name__ == "__main__":
    unittest.main()
