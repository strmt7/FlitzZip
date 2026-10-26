"""Offline rejection, preservation and recovery controls for development HIP object admission."""

import copy
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

from tools import hip_coverage as coverage
from tools import hip_coverage_product as product


class ProductCoverageTests(unittest.TestCase):
    # Purpose: Own clearly nonexecutable compiler/source/profile fixtures without touching an installed SDK or GPU.
    # Inputs: None. Outputs: Cleanup-bound isolated source and evidence paths.
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        for source in product.SOURCES:
            self.write(source, b"// nonexecutable source fixture\n")
        self.write("CMakeLists.txt", b"# nonexecutable build fixture\n")
        self.write("cmake/fixture.cmake", b"# source dependency fixture\n")
        self.sdk = self.root / "private SDK"
        self.header = self.write("private SDK/include/hip/fixture.h", b"// SDK header fixture\n")
        self.probe = self.write("probe.hip.cpp", b"// nonexecutable helper control identity\n")
        paths = [
            self.probe,
            self.write("private compiler/bin/clang++.exe", b"nonexecutable compiler fixture"),
            self.write("private SDK/lib/amdhip64.lib", b"nonexecutable import fixture"),
            self.write("runtime/libclang_rt.profile-amdgpu.a", b"nonexecutable device runtime fixture"),
            self.write("runtime/clang_rt.profile_rocm-x86_64.lib", b"nonexecutable host runtime fixture"),
            self.write("tools/flitzzip_hip_profile_register.exe", b"nonexecutable registration fixture"),
        ]
        artifact = self.write("probe.profraw", b"nonexecutable qualification artifact fixture")
        self.qualification = self.root / "qualification.json"
        self.qualifier = {
            "qualification": "passed",
            "architecture": "gfx1201",
            "inputs": coverage.identities(paths),
            "artifacts": coverage.identities([artifact]),
        }
        self.qualification.write_text(json.dumps(self.qualifier), encoding="utf-8")
        for name, value in (("ROOT", self.root), ("PROBE", self.probe)):
            context = patch.object(coverage, name, value)
            context.start()
            self.addCleanup(context.stop)

    # Purpose: Create one private fixture. Inputs: Relative path and literal bytes. Outputs: Existing file.
    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    # Purpose: Replace external compilation only. Inputs: Selected source/output. Outputs: Labelled bytes.
    @staticmethod
    def unit_fixture(tools, source, output):
        output.mkdir()
        obj = output / "completed.obj"
        obj.write_bytes(b"nonexecutable object fixture: " + source.name.encode())
        return obj, [obj]

    # Purpose: Exercise the real manifest producer with no compiler or GPU execution.
    # Inputs: Fresh private output. Outputs: Current complete receipt and its path.
    def built_fixture(self):
        output = self.root / "out/objects"
        with (
            patch.object(product, "admit_tool"),
            patch.object(product, "compile_object", side_effect=self.unit_fixture),
        ):
            record = product.build(self.qualification, output)
        return output / "product-objects.json", record

    # Purpose: Prove successful preparation/admission and refusal to overwrite retained evidence.
    # Inputs: Exact private sources/tools. Outputs: All canonical object roles and preserved first result.
    def test_complete_manifest_and_fresh_output_ownership(self):
        manifest, record = self.built_fixture()
        result = product.admit(manifest, self.sdk, "gfx1201")
        self.assertEqual(set(result["objects"]), set(product.SOURCES))
        self.assertEqual(result["host_runtime"], record["host_runtime"])
        before = manifest.read_bytes()
        with self.assertRaises(FileExistsError):
            product.build(self.qualification, manifest.parent)
        self.assertEqual(manifest.read_bytes(), before)

    # Purpose: Refuse directory metadata, missing helper-inclusive qualification and cross-SDK substitution.
    # Inputs: Individually malformed owned evidence. Outputs: Refusal without compilation or manifest mutation.
    def test_metadata_probe_role_and_qualified_sdk_cannot_be_substituted(self):
        with self.assertRaisesRegex(ValueError, "bounded regular file"):
            product.read_record(self.root)
        qualifier = copy.deepcopy(self.qualifier)
        qualifier["inputs"].pop(str(self.probe))
        self.qualification.write_text(json.dumps(qualifier))
        with self.assertRaisesRegex(ValueError, "helper-inclusive"):
            product.qualified_toolchain(self.qualification)
        self.qualification.write_text(json.dumps(self.qualifier))
        manifest, record = self.built_fixture()
        other = self.root / "other SDK"
        other.mkdir()
        record["sdk"] = str(other)
        manifest.write_text(json.dumps(record))
        with self.assertRaisesRegex(ValueError, "qualified toolchain"):
            product.admit(manifest, other, "gfx1201")

    # Purpose: Keep manifest admission free of native compilation and require an explicit build command.
    # Inputs: Actual CLI arguments and controlled compiler transport. Outputs: Exact route and small JSON summaries.
    def test_cli_admission_never_implicitly_compiles(self):
        for command in ("build", "admit"):
            output = self.root / "out/explicit"
            args = ["objects", command]
            if command == "build":
                args.extend(["--qualification", str(self.qualification), "--output", str(output)])
            else:
                args.extend(["--manifest", str(self.qualification), "--sdk", str(self.sdk), "--arch", "gfx1201"])
            with (
                patch.object(sys, "argv", args),
                redirect_stdout(io.StringIO()) as captured,
                patch.object(
                    product, "build", return_value={"scope": "fixture", "objects": {"one": "fixture"}}
                ) as build,
                patch.object(product, "admit", return_value={"admitted": True}) as admit,
            ):
                self.assertEqual(product.main(), 0)
                report = json.loads(captured.getvalue())
                if command == "build":
                    build.assert_called_once_with(self.qualification, output)
                    admit.assert_not_called()
                    self.assertEqual(report["objects"], 1)
                else:
                    build.assert_not_called()
                    admit.assert_called_once_with(self.qualification, self.sdk, "gfx1201")
                    self.assertTrue(report["admitted"])

    # Purpose: Keep separately bound CMake consumers from invalidating unchanged standalone compiler outputs.
    # Inputs: Object manifest followed by a consumer recipe edit. Outputs: Original objects remain admitted.
    def test_consumer_recipe_is_not_a_standalone_object_compiler_input(self):
        manifest, record = self.built_fixture()
        self.assertNotIn(str(self.root / "CMakeLists.txt"), record["inputs"])
        (self.root / "CMakeLists.txt").write_text("# changed consumer; compiler source and argv unchanged\n")
        product.admit(manifest, self.sdk, "gfx1201")

    # Purpose: Detect stale, missing or newly introduced dependencies before linking.
    # Inputs: One independently changed header/source inventory. Outputs: No successful admission.
    def test_header_mutation_missing_artifact_and_inventory_expansion_are_rejected(self):
        manifest, record = self.built_fixture()
        original = self.header.read_bytes()
        self.header.write_bytes(b"changed SDK header")
        with self.assertRaisesRegex(ValueError, "changed"):
            product.admit(manifest, self.sdk, "gfx1201")
        self.header.write_bytes(original)
        extra = self.write("private SDK/include/new.h", b"new dependency")
        with self.assertRaisesRegex(ValueError, "inventory is incomplete"):
            product.admit(manifest, self.sdk, "gfx1201")
        extra.unlink()
        obj = Path(record["objects"][product.SOURCES[0]])
        obj.unlink()
        with self.assertRaises(ValueError):
            product.admit(manifest, self.sdk, "gfx1201")

    # Purpose: Reject forged scope, role, dependency and object associations without modifying any source.
    # Inputs: Independent single-field manifest corruptions. Outputs: Refusal for each and successful original reuse.
    def test_manifest_corruptions_are_not_admitted(self):
        manifest, original = self.built_fixture()
        (hip_import,) = [name for name in original["inputs"] if name.endswith("amdhip64.lib")]
        source = product.SOURCES[0]
        corruptions = [
            lambda r: r.update(schema_version=True),
            lambda r: r.update(scope="production"),
            lambda r: r.update(root=str(self.root.parent)),
            lambda r: r.update(architecture="gfx1100"),
            lambda r: r.update(sdk="foreign"),
            lambda r: r.update(device_images_byte_identical=1),
            lambda r: r.update(objects=[]),
            lambda r: r["objects"].pop(source),
            lambda r: r["objects"].update({source: []}),
            lambda r: r["objects"].update({source: r["objects"][product.SOURCES[1]]}),
            lambda r: r.update(host_runtime="foreign"),
            lambda r: r.update(host_runtime=True),
            lambda r: r.update(host_runtime=[]),
            lambda r: r.update(host_runtime=hip_import),
            lambda r: r.update(qualification="foreign"),
            lambda r: r["inputs"].pop(str(self.header)),
            lambda r: r.update(extra="unknown schema field"),
        ]
        for index, change in enumerate(corruptions):
            changed = copy.deepcopy(original)
            change(changed)
            manifest.write_text(json.dumps(changed), encoding="utf-8")
            with self.subTest(index=index), self.assertRaises(ValueError):
                product.admit(manifest, self.sdk, "gfx1201")
        manifest.write_text(json.dumps(original), encoding="utf-8")
        product.admit(manifest, self.sdk, "gfx1201")

    # Purpose: Refuse unqualified/stale compiler evidence and ambiguous dependency selection.
    # Inputs: Qualification with independently altered status, file bytes or compiler identities. Outputs: Refusal.
    def test_qualification_and_unique_dependency_controls(self):
        tools, inputs = product.qualified_toolchain(self.qualification)
        self.assertEqual(tools["sdk"], self.sdk)
        self.qualifier["qualification"] = "failed"
        self.qualification.write_text(json.dumps(self.qualifier), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "passing GPU"):
            product.qualified_toolchain(self.qualification)
        with self.assertRaisesRegex(ValueError, "one qualified"):
            product.unique_input([], "compiler")
        with self.assertRaisesRegex(ValueError, "one qualified"):
            product.unique_input([Path("a/compiler"), Path("b/compiler")], "compiler")
        self.probe.write_bytes(b"changed qualified source")
        with self.assertRaisesRegex(ValueError, "changed"):
            product.require_identities(inputs)

    # Purpose: Reject malformed identity data before native execution or filesystem writes.
    # Inputs: Wrong types, paths and incomplete hashes. Outputs: Typed refusal.
    def test_identity_metadata_is_complete_and_typed(self):
        for value in (None, {}, [], {"relative": {}}, {str(self.probe): {"bytes": True, "sha256": "a" * 64}}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                product.require_identities(value)
        invalid = self.root / "invalid.json"
        for data in ('{"x":1,"x":2}', '{"x":NaN}', "[]"):
            invalid.write_text(data, encoding="utf-8")
            with self.subTest(data=data), self.assertRaises(ValueError):
                product.read_record(invalid)

    # Purpose: Exercise actual command routing and the image-preservation boundary without native execution.
    # Inputs: Real source path and independent matching/mismatching device byte fixtures. Outputs: Pass or refusal.
    def test_object_compile_keeps_original_source_and_exact_device_image(self):
        tools, _ = product.qualified_toolchain(self.qualification)
        source = self.root / product.SOURCES[0]
        inventory = {
            "device_data_records": 1,
            "registered_data_records": 1,
            "host_profile_groups": 1,
            "absent_host_profile_groups": 0,
            "missing": [],
            "table": "__llvm_profile_sections_1",
            "profile_record_bytes": 72,
            "module_ir_sha256": "a" * 64,
        }
        report = {
            "before": dict(inventory),
            "after": dict(inventory),
            "completed_records": 0,
            "retired_profile_groups": 0,
            "original_program_values_preserved": True,
        }
        for mismatch in (False, True):
            commands = []

            # Purpose: Retain argv, replacing only compilation. Inputs: Command/log. Outputs: Fixture object.
            def compile_fixture(command, log, timeout=120, *, commands=commands):
                commands.append(command)
                Path(command[command.index("-o") + 1]).write_bytes(b"nonexecutable compiler output")
                return {"stdout": ""}

            # Purpose: Replace extraction only. Inputs: Object/output. Outputs: Independently controlled ELF bytes.
            def image_fixture(llvm, obj, target, output, *, coff_object, mismatch=mismatch):
                image = output / "device.elf"
                image.write_bytes(
                    b"different image" if mismatch and output.name == "completed-image" else b"same image"
                )
                return image

            # Purpose: Replace native repair only. Inputs: Actual tool argv/log. Outputs: Labelled bitcode and metadata.
            def repair_fixture(command, log):
                Path(command[-1]).write_bytes(b"nonexecutable repaired bitcode fixture")
                return copy.deepcopy(report)

            with (
                self.subTest(mismatch=mismatch),
                patch.object(coverage, "checked", side_effect=compile_fixture),
                patch.object(coverage, "device_image", side_effect=image_fixture),
                patch.object(product, "native_metadata", side_effect=repair_fixture),
            ):
                output = self.root / ("changed-device" if mismatch else "same-device")
                if mismatch:
                    with self.assertRaisesRegex(ValueError, "changed a GPU executable"):
                        product.compile_object(tools, source, output)
                else:
                    obj, artifacts = product.compile_object(tools, source, output)
                    self.assertTrue(obj.exists())
                    self.assertEqual(len(artifacts), 6)
                self.assertEqual(len(commands), 3)
                self.assertIn(str(source), commands[0])
                self.assertIn(str(source), commands[1])
                self.assertIn("--offload-host-only", commands[1])
                self.assertIn("-fms-runtime-lib=dll", commands[2])
                self.assertNotIn("-save-temps", commands[0])

    # Purpose: Retain failed build evidence and publish no pass manifest after partial compilation or source mutation.
    # Inputs: Controlled native failure then private source change. Outputs: Failure without a pass manifest.
    def test_partial_build_and_concurrent_source_change_do_not_publish(self):
        output = self.root / "out/failed"
        with (
            patch.object(product, "admit_tool"),
            patch.object(product, "compile_object", side_effect=RuntimeError("native failure")),
            self.assertRaisesRegex(RuntimeError, "native failure"),
        ):
            product.build(self.qualification, output)
        self.assertFalse((output / "product-objects.json").exists())

        # Purpose: Inject a real per-instance source mutation. Inputs: Fixture compilation. Outputs: Changed header.
        def changing(tools, source, directory):
            result = self.unit_fixture(tools, source, directory)
            self.header.write_bytes(b"changed during compilation")
            return result

        output = self.root / "out/mutated"
        with (
            patch.object(product, "admit_tool"),
            patch.object(product, "compile_object", side_effect=changing),
            self.assertRaisesRegex(ValueError, "changed"),
        ):
            product.build(self.qualification, output)
        self.assertFalse((output / "product-objects.json").exists())


if __name__ == "__main__":
    unittest.main()
