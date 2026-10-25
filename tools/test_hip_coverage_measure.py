"""Offline source, image, counter and failure-publication controls for product device measurement."""

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

from tools import hip_coverage as coverage
from tools import hip_coverage_images as images
from tools import hip_coverage_measure as measure


class MeasurementTests(unittest.TestCase):
    # Purpose: Own nonexecutable build/cache/profile fixtures. Inputs: None. Outputs: Isolated cleanup-bound paths.
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        context = patch.object(coverage, "ROOT", self.root)
        context.start()
        self.addCleanup(context.stop)
        self.source = self.write("src/gpu/fixture.hip.cpp", b"// source fixture\n")
        self.compiler = self.write("compiler/bin/clang++.exe", b"nonexecutable compiler fixture")
        self.reader = self.write("compiler/bin/flitzzip_hip_profile_register.exe", b"nonexecutable reader fixture")
        self.sdk = self.root / "sdk"
        self.sdk.mkdir()
        self.build = self.root / "out/build"
        self.build.mkdir(parents=True)
        self.record = {
            "sdk": str(self.sdk),
            "architecture": "gfx1201",
            "inputs": coverage.identities([self.source, self.compiler, self.reader]),
            "artifacts": {},
            "objects": {},
        }
        self.manifest = self.write("out/objects.json", json.dumps(self.record).encode())
        self.cache = {
            "CMAKE_HOME_DIRECTORY": str(self.root),
            "FLITZZIP_HIP_COVERAGE_MANIFEST": str(self.manifest),
            "FLITZZIP_HIP_PATH": str(self.sdk),
            "FLITZZIP_ENABLE_HIP": "ON",
            "FLITZZIP_HIP_ARCH": "gfx1201",
        }
        self.write_cache()
        for name in ("flitzzip_tests.exe", "flitzzip_cli.exe", "flitzzip_hip_kernels.dll"):
            self.write("out/build/Debug/" + name, b"nonexecutable binary fixture")
        context = patch.object(measure, "admit")
        context.start()
        self.addCleanup(context.stop)

    # Purpose: Create one private file. Inputs: Relative name and bytes. Outputs: Existing fixture path.
    def write(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    # Purpose: Write a native-shaped cache. Inputs: Private settings. Outputs: Regular cache file.
    def write_cache(self):
        (self.build / "CMakeCache.txt").write_text(
            "\n".join(f"{key}:STRING={value}" for key, value in self.cache.items()), encoding="utf-8"
        )

    # Purpose: Freeze link/lifecycle/test consumers while preserving separately admitted standalone object inputs.
    # Inputs: Owned recipe, lifecycle and test changes. Outputs: Every consumer edit invalidates its exact receipt.
    def test_consumer_recipe_and_tests_have_independent_source_binding(self):
        paths = [
            self.write("CMakeLists.txt", b"# recipe fixture\n"),
            self.write("cmake/fixture.cmake", b"# configuration fixture\n"),
            self.write("tools/hip_profile_registration/hip_profile_lifecycle.cpp", b"// lifecycle fixture\n"),
            self.write("tests/cpp/fixture.cpp", b"// consumer fixture\n"),
        ]
        frozen = measure.consumer_inputs(self.record, self.manifest)
        for path in paths:
            original = path.read_bytes()
            self.assertIn(str(path), frozen)
            path.write_bytes(original + b"changed\n")
            with self.subTest(path=path.name), self.assertRaisesRegex(ValueError, "changed"):
                measure.require_identities(frozen)
            self.assertEqual(
                coverage.identities([self.source]), {str(self.source): self.record["inputs"][str(self.source)]}
            )
            path.write_bytes(original)
        measure.require_identities(frozen)

    # Purpose: Keep admission read-only and make the costly build/measurement role explicit at the CLI boundary.
    # Inputs: Actual parsed selectors and labelled transport doubles. Outputs: Exact dispatch and bounded summaries.
    def test_cli_separates_admission_from_explicit_native_execution(self):
        for command in ("admit", "build-and-measure"):
            args = ["measurement", command, "--manifest", str(self.manifest), "--build-root", str(self.build)]
            output = self.root / "out/cli"
            if command == "build-and-measure":
                args.extend(["--filter", "=independent_oracle", "--output", str(output)])
            with (
                patch.object(sys, "argv", args),
                redirect_stdout(io.StringIO()) as captured,
                patch.object(measure, "admit_build", return_value=self.record) as admission,
                patch.object(
                    measure,
                    "build_and_measure",
                    return_value={
                        "qualification": "passed",
                        "scope": "fixture measurement",
                        "summary": {"lines": 2, "covered": 1},
                    },
                ) as execution,
            ):
                self.assertEqual(measure.main(), 0)
                report = json.loads(captured.getvalue())
                if command == "admit":
                    self.assertTrue(report["admitted"])
                    execution.assert_not_called()
                    admission.assert_called_once_with(self.manifest, self.build)
                else:
                    admission.assert_not_called()
                    execution.assert_called_once_with(self.manifest, self.build, ["=independent_oracle"], output)
                    self.assertEqual((report["lines"], report["covered"]), (2, 1))

    # Purpose: Parse native cache comments and reject foreign roots, sources, SDKs, manifests and duplicate keys.
    # Inputs: Ordinary CMake comments and independent corruptions. Outputs: Valid admission or typed refusal.
    def test_build_scope_is_exact_and_unambiguous(self):
        self.assertEqual(measure.admit_build(self.manifest, self.build), self.record)
        cache = self.build / "CMakeCache.txt"
        cache.write_text("# Generated by CMake\n// Native cache comment\n\n" + cache.read_text())
        self.assertEqual(measure.admit_build(self.manifest, self.build), self.record)
        original = dict(self.cache)
        foreign = self.root / "foreign"
        foreign.mkdir()
        for key, value in (
            ("CMAKE_HOME_DIRECTORY", str(foreign)),
            ("FLITZZIP_HIP_PATH", str(foreign)),
            ("FLITZZIP_HIP_COVERAGE_MANIFEST", str(self.source)),
            ("FLITZZIP_ENABLE_HIP", "OFF"),
            ("FLITZZIP_HIP_ARCH", "gfx1100"),
        ):
            self.cache = {**original, key: value}
            self.write_cache()
            with self.subTest(key=key), self.assertRaises(ValueError):
                measure.admit_build(self.manifest, self.build)
        self.cache = original
        self.write_cache()
        cache = self.build / "CMakeCache.txt"
        cache.write_text(cache.read_text() + "\nFLITZZIP_ENABLE_HIP:STRING=ON\n")
        with self.assertRaisesRegex(ValueError, "Ambiguous"):
            measure.admit_build(self.manifest, self.build)
        with self.assertRaisesRegex(ValueError, "isolated"):
            measure.admit_build(self.manifest, self.root)
        cache.write_bytes(b"#" * (5 * 1024**2 + 1))
        with self.assertRaisesRegex(ValueError, "metadata bound"):
            measure.admit_build(self.manifest, self.build)

    # Purpose: Prevent accidental whole-group workloads or repeated selectors before native execution.
    # Inputs: Empty, prefix, malformed and duplicate selectors. Outputs: Refusal without an output directory.
    def test_measurement_requires_reviewed_exact_selectors(self):
        for selectors in ([], ["^gpu_"], ["="], ["=a", "=a"], ["=a --unrelated"], ["=a"] * 65):
            output = self.root / "out/refused"
            with self.subTest(selectors=selectors), self.assertRaises(ValueError):
                measure.build_and_measure(self.manifest, self.build, selectors, output)
            self.assertFalse(output.exists())

    # Purpose: Replace process transport only while retaining actual profile counting and native result parsing.
    # Inputs: Controlled result/count. Outputs: Labelled raw-profile fixtures and metadata.
    def execution_fixture(self, command, timeout, log, *, count=5, result=None):
        for index in range(count):
            (log.parent / f"gfx1201.{index}.fixture.profraw").write_bytes(b"nonexecutable native profile fixture")
        return result or {"stdout": "1 tests, 0 failed\n", "stderr": "", "output_truncated": False}

    # Purpose: Reject missing/truncated/failed profiles and skipped HIP admission even when native exit was zero.
    # Inputs: One corrupted process boundary per fresh case. Outputs: Correct positive control and every refusal.
    def test_process_oracles_require_actual_admission_and_every_profile(self):
        cases = [(5, None, False, True), (4, None, False, False), (6, None, False, False)]
        cases += [
            (
                5,
                {"stdout": "1 tests, 0 failed\n", "stderr": "collection failed", "output_truncated": False},
                False,
                False,
            ),
            (5, {"stdout": "1 tests, 0 failed\n", "stderr": "", "output_truncated": True}, False, False),
            (5, {"stdout": "0 tests, 0 failed\n", "stderr": "", "output_truncated": False}, False, False),
            (5, {"stdout": "available=false\n", "stderr": "", "output_truncated": False}, True, False),
        ]
        fields = ("hip_compiled", "hip_runtime_loadable", "hip_kernel_loadable", "available")
        status = "".join(f"{field}=true\n" for field in fields)
        for changed in (None, *fields):
            for missing in (False,) if changed is None else (False, True):
                output_text = (
                    status
                    if changed is None
                    else status.replace(f"{changed}=true\n", "" if missing else f"{changed}=false\n")
                )
                cases.append(
                    (5, {"stdout": output_text, "stderr": "", "output_truncated": False}, True, changed is None)
                )
        for index, (count, result, capability, accepted) in enumerate(cases):
            output = self.root / f"out/case-{index}"

            # Purpose: Control one transport response. Inputs: Native argv/bounds/log. Outputs: Fixture result.
            def execute(command, timeout, log, *, count=count, result=result):
                return self.execution_fixture(command, timeout, log, count=count, result=result)

            with self.subTest(index=index), patch.object(measure, "run_checked", side_effect=execute):
                if accepted:
                    self.assertEqual(
                        measure.run_case(["fixture"], "gfx1201", output, capability=capability),
                        [output / f"gfx1201.{index}.fixture.profraw" for index in range(5)],
                    )
                else:
                    with self.assertRaises(ValueError):
                        measure.run_case(["fixture"], "gfx1201", output, capability=capability)

    # Purpose: Prevent a pass receipt after execution mutates admitted source and preserve the original failed evidence.
    # Inputs: External-work doubles and a real source mutation. Outputs: No false pass; immutable control passes.
    def test_publication_fails_on_source_mutation_and_recovers_in_fresh_output(self):
        inputs = coverage.identities([self.source, self.compiler])
        elf = self.write("device.elf", b"nonexecutable ELF fixture")
        profile = self.write("fixture.profraw", b"nonexecutable profile fixture")
        original = self.source.read_bytes()
        for changed in (True, False):
            self.source.write_bytes(original)
            output = self.root / ("out/mutated" if changed else "out/valid")

            # Purpose: Change an owned source boundary. Inputs: None. Outputs: Fixture profile and optional mutation.
            def case(*args, changed=changed, **kwargs):
                if changed:
                    self.source.write_bytes(b"changed during measurement")
                return [profile]

            with (
                patch.object(measure, "admit_build", return_value=self.record),
                patch.object(measure, "consumer_inputs", return_value=inputs),
                patch.object(measure, "run_checked"),
                patch.object(measure, "linked_images", return_value={"fixture": elf}),
                patch.object(measure, "run_case", side_effect=case),
                patch.object(measure, "export_images", return_value={"lines": 1, "covered": 1}),
            ):
                if changed:
                    with self.assertRaisesRegex(ValueError, "changed"):
                        measure.build_and_measure(self.manifest, self.build, ["=fixture"], output)
                    self.assertFalse((output / "qualification.json").exists())
                else:
                    self.assertEqual(
                        measure.build_and_measure(self.manifest, self.build, ["=fixture"], output)["qualification"],
                        "passed",
                    )


class ImageReportTests(unittest.TestCase):
    # Purpose: Own native-extraction and LLVM-report fixtures. Inputs: None. Outputs: Cleanup-bound data.
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        context = patch.object(coverage, "ROOT", self.root)
        context.start()
        self.addCleanup(context.stop)
        self.compiler = self.root / "clang++.exe"
        self.compiler.write_bytes(b"nonexecutable compiler fixture")

    # Purpose: Independently describe source counts. Inputs: Filename and integer counts. Outputs: Native-shaped item.
    def item(self, name, count=10, covered=3):
        return {"filename": str(self.root / name), "summary": {"lines": {"count": count, "covered": covered}}}

    # Purpose: Wrap native-shaped files. Inputs: File rows. Outputs: Complete combined-export fixture.
    def report(self, files):
        return {"type": "llvm.coverage.json.export", "version": "3.1.0", "data": [{"files": files}]}

    # Purpose: Keep cached SDK headers and lookalike paths out of product totals while retaining raw native data.
    # Inputs: Project, out/ SDK and src-other paths. Outputs: Only the exact source-root row.
    def test_project_scope_and_inconsistent_native_counts(self):
        report = self.report(
            [self.item("src/gpu/a.cpp"), self.item("out/sdk/include/vendor.hpp"), self.item("src-other/b.cpp")]
        )
        result = images.project_summary(report)
        self.assertEqual(result, [{"source": "src/gpu/a.cpp", "lines": 10, "covered": 3, "missed": 7}])
        for count, covered in ((True, 1), (1, True), (3, 4), (-1, 0), (None, 0)):
            with self.subTest(count=count, covered=covered), self.assertRaises(ValueError):
                images.project_summary(self.report([self.item("src/a.cpp", count, covered)]))
        for invalid in (
            {"type": "unknown"},
            self.report([]),
            self.report([None]),
            self.report([{"filename": str(self.root / "src/a.cpp")}]),
            self.report([self.item("src/a.cpp"), self.item("src/a.cpp")]),
            {"type": "llvm.coverage.json.export", "version": "3.1.0", "data": [None]},
        ):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                images.project_summary(invalid)

    # Purpose: Challenge every bundle-preservation boundary using actual frozen fixture files and native-call doubles.
    # Inputs: Missing, duplicated, changed ELF and unreviewed section bytes. Outputs: One exact success and refusals.
    def test_linked_module_contains_only_exact_admitted_bundles(self):
        record = {
            "inputs": coverage.identities([self.compiler]),
            "architecture": "gfx1201",
            "objects": {},
            "artifacts": {},
        }
        bundles = {"a.hip": b"independent bundle A fixture", "b.hip": b"independent bundle B fixture"}
        for name in bundles:
            unit = self.root / name
            unit.mkdir()
            obj, elf = unit / "completed.obj", unit / "device.elf"
            obj.write_bytes(b"nonexecutable object fixture")
            elf.write_bytes(("nonexecutable ELF " + name).encode())
            record["objects"]["src/" + name + ".cpp"] = str(obj)
            record["artifacts"].update(coverage.identities([elf]))
        for variant in ("valid", "missing", "duplicate", "gap", "tail", "changed-elf", "empty"):
            section = b"\0" * 8 + bundles["a.hip"] + b"\0" * 3 + bundles["b.hip"]
            if variant == "missing":
                section = bundles["a.hip"]
            if variant == "duplicate":
                section += bundles["a.hip"]
            if variant == "gap":
                section = b"unexpected" + section
            if variant == "tail":
                section += b"unexpected"
            if variant == "empty":
                section = b""

            # Purpose: Replace section extraction only. Inputs: Actual argv. Outputs: Controlled linked section bytes.
            def extract(command, timeout, log, *, section=section):
                selected = next(arg.split("=", 2)[2] for arg in command if arg.startswith("--dump-section="))
                Path(selected).write_bytes(section)

            # Purpose: Replace object extraction. Inputs: Object/output. Outputs: Controlled ELF/bundle pair.
            def device(llvm, obj, target, output, *, coff_object, variant=variant):
                elf = output / "device.elf"
                expected = (obj.parent / "device.elf").read_bytes()
                elf.write_bytes(b"changed" if variant == "changed-elf" else expected)
                (output / "image.hipfb").write_bytes(bundles[obj.parent.name])
                return elf

            with (
                self.subTest(variant=variant),
                patch.object(images, "run_checked", side_effect=extract),
                patch.object(coverage, "device_image", side_effect=device),
            ):
                if variant == "valid":
                    self.assertEqual(
                        len(images.linked_images(record, self.root / "fixture.dll", self.root / variant)), 2
                    )
                else:
                    with self.assertRaises(ValueError):
                        images.linked_images(record, self.root / "fixture.dll", self.root / variant)

    # Purpose: Reject cross-image association, incomplete collection and malformed native identity reports.
    # Inputs: Two independent images/profiles and transport mutations. Outputs: Exact per-image groups or refusal.
    def test_profile_association_is_unique_complete_and_typed(self):
        first, second = self.root / "first.elf", self.root / "second.elf"
        profiles = [self.root / "first.profraw", self.root / "second.profraw"]
        expected = {"first": [profiles[0]], "second": [profiles[1]]}
        valid = {
            "matched_device": str(first),
            "records": 2,
            "scope": "Native profile/ELF inventory association; no execution",
        }
        for variant in ("valid", "missing", "foreign", "boolean", "no-scope", "truncated", "warning"):
            output = self.root / ("association-" + variant)
            output.mkdir()

            # Purpose: Control only native transport. Inputs: Actual profile argv. Outputs: Image identity or failure.
            def execute(command, timeout, log, *, variant=variant):
                report = {**valid, "matched_device": str(first if str(profiles[0]) in command else second)}
                if variant == "missing":
                    report["matched_device"] = str(first)
                if variant == "foreign":
                    report["matched_device"] = str(self.root / "foreign.elf")
                if variant == "boolean":
                    report["records"] = True
                if variant == "no-scope":
                    report.pop("scope")
                return {
                    "stdout": json.dumps(report),
                    "stderr": "warning" if variant == "warning" else "",
                    "output_truncated": variant == "truncated",
                }

            with self.subTest(variant=variant), patch.object(images, "run_checked", side_effect=execute):
                if variant == "valid":
                    self.assertEqual(
                        images.associate_profiles(self.compiler, {"first": first, "second": second}, profiles, output),
                        expected,
                    )
                else:
                    with self.assertRaises(ValueError):
                        images.associate_profiles(self.compiler, {"first": first, "second": second}, profiles, output)
        for candidates, raw in (({"a": first, "b": first}, profiles), ({"a": first}, [profiles[0]] * 2)):
            with self.assertRaises(ValueError):
                images.associate_profiles(self.compiler, candidates, raw, output)

    # Purpose: Union active and unused shared-header mappings without counting a line twice or inventing hits.
    # Inputs: Independently specified per-image native line reports and malformed boundaries. Outputs: Exact union.
    def test_native_line_union_preserves_shared_source_and_rejects_invalid_hits(self):
        name = str(self.root / "src/shared.hpp")
        first = {
            "scope": "LLVM per-image mapped line hits; no execution",
            "files": [{"filename": name, "lines": [[10, False], [12, True]]}],
        }
        second = {"scope": first["scope"], "files": [{"filename": name, "lines": [[10, True], [14, False]]}]}
        self.assertEqual(
            images.project_line_summary([first, second]),
            [{"source": "src/shared.hpp", "lines": 3, "covered": 2, "missed": 1}],
        )
        external = {
            "scope": first["scope"],
            "files": [{"filename": str(self.root / "out/sdk/vendor.hpp"), "lines": [[10, True]]}],
        }
        self.assertEqual(
            images.project_line_summary([first, second, external]), images.project_line_summary([first, second])
        )
        with self.assertRaisesRegex(ValueError, "no repository"):
            images.project_line_summary([external])
        for pairs in ([[True, False]], [[0, True]], [[2**32, True]], [[1, 1]], [[1]], [[1, True], [1, False]]):
            invalid = {"scope": first["scope"], "files": [{"filename": name, "lines": pairs}]}
            with self.subTest(pairs=pairs), self.assertRaises(ValueError):
                images.project_line_summary([invalid])
        for invalid in (
            {},
            {"scope": first["scope"], "files": [None]},
            {"scope": first["scope"], "files": first["files"] * 2},
        ):
            with self.assertRaises(ValueError):
                images.project_line_summary([invalid])

    # Purpose: Preserve per-image exports and reject truncated output, merge diagnostics or mismatched CFGs.
    # Inputs: Strict native-shaped reports and independent failure flags. Outputs: Actual line union only on success.
    def test_export_preserves_native_data_and_refuses_incomplete_mapping(self):
        elf, profile = self.root / "device.elf", self.root / "device.profraw"
        elf.write_bytes(b"nonexecutable ELF fixture")
        profile.write_bytes(b"nonexecutable profile fixture")
        for name in ("llvm-profdata.exe", "llvm-cov.exe"):
            (self.root / name).write_bytes(b"nonexecutable LLVM fixture")
        for index, (truncated, diagnostic) in enumerate(
            ((False, ""), (True, ""), (False, "mapping mismatch"), (False, "merge warning"))
        ):
            native = json.dumps(self.report([self.item("src/gpu/a.cpp")]))

            # Purpose: Replace native transport. Inputs: Merge/export argv. Outputs: Native-shaped data and fixture.
            def execute(command, timeout, log, *, truncated=truncated, diagnostic=diagnostic, native=native):
                if "merge" in command:
                    Path(command[command.index("-o") + 1]).write_bytes(b"nonexecutable merged fixture")
                result = native
                if "--profile" in command:
                    result = json.dumps(
                        {
                            "matched_device": str(elf),
                            "records": 1,
                            "scope": "Native profile/ELF inventory association; no execution",
                        }
                    )
                if "--coverage-profile" in command:
                    result = json.dumps(
                        {
                            "scope": "LLVM per-image mapped line hits; no execution",
                            "files": [
                                {
                                    "filename": str(self.root / "src/gpu/a.cpp"),
                                    "lines": [[i, i <= 3] for i in range(1, 11)],
                                }
                            ],
                        }
                    )
                error = diagnostic if diagnostic != "merge warning" or "merge" in command else ""
                return {"stdout": result, "stderr": error, "output_truncated": truncated}

            with self.subTest(index=index), patch.object(images, "run_checked", side_effect=execute):
                output = self.root / f"export-{index}"
                if not truncated and not diagnostic:
                    result = images.export_images(self.root, self.compiler, {"fixture": elf}, [profile], output)
                    self.assertEqual((result["lines"], result["covered"]), (10, 3))
                    self.assertEqual((output / "image-00/export-process-native.json").read_text(), native)
                else:
                    with self.assertRaises(ValueError):
                        images.export_images(self.root, self.compiler, {"fixture": elf}, [profile], output)


if __name__ == "__main__":
    unittest.main()
