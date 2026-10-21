"""Regression tests for evidence validation and deterministic benchmark graphs."""

from __future__ import annotations

import builtins
import copy
import hashlib
import io
import json
import os
import runpy
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import render_benchmark_graph as graph
from tools.native_build_provenance import canonical
from tools.native_build_receipt import RECIPE_KEYS, validate_receipt
from tools.render_svg_graph import parse_bounded_document


class GraphPublicationCommandTests(unittest.TestCase):
    # Purpose: Test real graph entry points against ambiguous JSON and verify failed admission preserves output.
    # Inputs: Small schema-correct fixtures for all four renderers; duplicated properties, nonfinite numbers and roots.
    # Outputs: Valid commands emit accessible SVG; each malformed capture fails explicitly without replacing it.
    def test_entry_points_reject_ambiguous_records(self) -> None:
        from tools.test_comparison_graph import fixture
        from tools.test_native_tradeoff_graph import sweep_fixture as native_sweep
        from tools.test_tradeoff_graph import sweep_fixture as comparison_sweep

        commands = (
            ("tools.render_comparison_graph", [fixture()]),
            ("tools.render_tradeoff_graph", comparison_sweep()),
            ("tools.render_native_tradeoff_graph", native_sweep()),
            ("tools.render_benchmark_graph", [fixture_record()]),
        )
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for module, records in commands:
                output = directory / "graph.svg"
                arguments = []
                for index, record in enumerate(records):
                    source = directory / f"record-{index}.json"
                    source.write_text(json.dumps(record), encoding="utf-8")
                    arguments.extend(("--input", str(source)))
                command = [sys.executable, "-m", module, *arguments, "--output", str(output)]
                with self.subTest(module=module):
                    completed = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=30)
                    self.assertEqual(completed.returncode, 0, completed.stderr)
                    original = output.read_bytes()
                    self.assertEqual(parse_bounded_document(original).attrib["role"], "img")
                    first = Path(arguments[1])
                    payload = first.read_text(encoding="utf-8")
                    invalid = (
                        '{"schema_version":0,' + payload[1:],
                        '{"extra":{"repeated":0,"repeated":1},' + payload[1:],
                        '{"extra":NaN,' + payload[1:],
                        '{"extra":1e999,' + payload[1:],
                        "[]",
                    )
                    for malformed in invalid:
                        first.write_text(malformed, encoding="utf-8")
                        completed = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=30)
                        self.assertEqual(completed.returncode, 1, completed.stderr)
                        self.assertIn("graph", completed.stderr)
                        self.assertNotIn("Traceback", completed.stderr)
                        self.assertEqual(output.read_bytes(), original)

    # Purpose: Exercise the direct-script import path and byte-check failure modes used by automated publishing.
    # Inputs: A valid native record, an absent chart, an unrelated sentinel and the generated exact chart.
    # Outputs: Check refuses missing or stale output without modifying it; generation and exact check succeed.
    def test_native_direct_script_generation_and_check(self) -> None:
        root = Path(__file__).resolve().parents[1]
        environment = {key: value for key, value in os.environ.items() if key.upper() != "PYTHONPATH"}
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source, output = directory / "record.json", directory / "graph.svg"
            source.write_text(json.dumps(fixture_record()), encoding="utf-8")
            command = [
                sys.executable,
                str(root / "tools/render_benchmark_graph.py"),
                "--input",
                str(source),
                "--output",
                str(output),
            ]
            for sentinel in (None, b"existing unrelated bytes"):
                if sentinel is not None:
                    output.write_bytes(sentinel)
                checked = subprocess.run(
                    [*command, "--check"], cwd=directory, env=environment, capture_output=True, timeout=30
                )
                self.assertEqual(checked.returncode, 1)
                self.assertEqual(output.read_bytes() if output.exists() else None, sentinel)
            generated = subprocess.run(command, cwd=directory, env=environment, capture_output=True, timeout=30)
            self.assertEqual(generated.returncode, 0, generated.stderr)
            checked = subprocess.run(
                [*command, "--check"], cwd=directory, env=environment, capture_output=True, timeout=30
            )
            self.assertEqual(checked.returncode, 0, checked.stderr)

    # Purpose: Exercise imported check failure and the supported direct-script import fallback without subprocess gaps.
    # Inputs: One valid record, stale output and a controlled package-import miss with the tools directory available.
    # Outputs: Stale bytes are preserved and the fallback exposes the same renderer contracts.
    def test_imported_check_and_direct_script_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source, output = directory / "record.json", directory / "graph.svg"
            source.write_text(json.dumps(fixture_record()), encoding="utf-8")
            sentinel = b"unrelated existing chart"
            output.write_bytes(sentinel)
            with (
                patch.object(
                    sys,
                    "argv",
                    ["render_benchmark_graph.py", "--input", str(source), "--output", str(output), "--check"],
                ),
                patch.object(graph.sys, "stderr", io.StringIO()),
            ):
                self.assertEqual(graph.main(), 1)
            self.assertEqual(output.read_bytes(), sentinel)

        original_import = builtins.__import__

        def direct_script_import(name, globals=None, locals=None, fromlist=(), level=0):
            """Purpose: Model direct-script resolution.
            Inputs: Import request. Outputs: One package miss, then normal imports.
            """
            if name == "tools.benchmark_comparators":
                raise ModuleNotFoundError(name)
            return original_import(name, globals, locals, fromlist, level)

        tool_directory = str(Path(graph.__file__).resolve().parent)
        sys.path.insert(0, tool_directory)
        try:
            with patch.object(builtins, "__import__", side_effect=direct_script_import):
                namespace = runpy.run_path(
                    str(Path(graph.__file__).resolve()), run_name="render_benchmark_graph_direct"
                )
        finally:
            sys.path.remove(tool_directory)
        self.assertTrue(callable(namespace["parse_metadata"]))
        self.assertTrue(callable(namespace["validate_receipt"]))


# Purpose: Build one complete paired-run record without depending on a real GPU or filesystem benchmark.
# Inputs: None.
# Outputs: Returns three exact-size CPU runs and three exact-size HIP runs in schema one.
def fixture_record() -> dict:
    runs = []
    for iteration in range(1, 4):
        for lane, output_bytes, seconds in (("CPU", 257129691, 5.8), ("GPU", 171079680, 4.6)):
            runs.append(
                {
                    "lane": lane,
                    "iteration": iteration,
                    "block_size_kib": 1024,
                    "input_bytes": 10 * 1024**3,
                    "output_bytes": output_bytes,
                    "compress_seconds": seconds - 2,
                    "verify_seconds": 1.0,
                    "extract_seconds": 1.0,
                    "gpu_kernel_launches": 720 if lane == "GPU" else 0,
                    "memory_only": True,
                    "disk_write_bytes": 0,
                }
            )
    return {
        "schema_version": 1,
        "benchmark_kind": "flzip_ram",
        "source_commit": "a" * 40,
        "source_dirty": False,
        "binary_sha256": "b" * 64,
        "cpu_model": "Test CPU",
        "gpu_model": "Test GPU",
        "profile": "SparseRecord",
        "size_mib": 10240,
        "compression_level": 5,
        "runs": runs,
    }


# Purpose: Build independently stored pilot and fixed confirmation fixtures with full current telemetry.
# Inputs: None; timings are deterministic fixtures, never performance evidence.
# Outputs: Returns a schema-three record whose confirmation stability can be recomputed.
def sampling_fixture() -> dict:
    record = fixture_record()
    record.update(
        schema_version=3,
        gpu_utilization_metric="process_busiest_engine_pct",
        lane_order="alternating_when_both",
        inter_run_pause_ms=250,
    )
    for run in record["runs"]:
        run.update(
            archive_bytes=run["output_bytes"] + 1024,
            workers=32,
            inflight_chunks=32,
            codec_workers=1,
            decode_inflight_chunks=32,
            decode_codec_workers=1,
            resource_sample_count=50,
            gpu_sample_count=25 if run["lane"] == "GPU" else 0,
            gpu_h2d_bytes=4096,
            gpu_d2h_bytes=2048,
            gpu_device_allocation_bytes=8192,
        )
    record["pilot_runs"] = copy.deepcopy(record["runs"])
    record["sampling_policy"] = {
        "method": "pilot_fixed_confirmation",
        "minimum_count": 3,
        "maximum_count": 15,
        "pilot_count": 3,
        "minimum_measured_seconds": 1,
        "target_relative_standard_error_pct": 2,
        "max_relative_std_dev_pct": 5,
        "discarded_sample_count": 0,
        "warmup_count": 0,
        "block_order": "reverse_on_even_iterations",
        "inference": "descriptive_only_no_confidence_or_significance_claim",
        "geometry_policy": "exact_depths_frozen_before_pilot_abort_on_admission_or_identity_change",
        "geometry_plans": [
            {
                "case": f"{lane}:1024",
                "workers": 32,
                "inflight_chunks": 32,
                "codec_workers": 1,
                "decode_inflight_chunks": 32,
                "decode_codec_workers": 1,
            }
            for lane in ("CPU", "GPU")
        ],
        "case_plans": [
            {
                "block_size_kib": 1024,
                "requested_count": 3,
                "confirmation_count": 3,
                "count_capped": False,
                "stopping_rule": "count_fixed_before_confirmation",
            }
        ],
    }
    return record


# Purpose: Model one complete current cohort with frozen artifact snapshots and independently checkable wall planning.
# Inputs: None; values are deterministic test fixtures and never performance evidence.
# Outputs: Returns paired pilots/confirmations, declared app-local hashes and a maximum-pilot-time budget.
def artifact_fixture() -> dict:
    record = sampling_fixture()
    record.update(
        measurement_protocol="bytewise-regenerated-v2",
        measurement_identity_policy="source-artifacts-around-observation-v1",
        binary_dependencies_sha256={"fixture.dll": "c" * 64},
    )
    identity = {
        key: record[key] for key in ("source_commit", "source_dirty", "binary_sha256", "binary_dependencies_sha256")
    }
    for run in record["runs"] + record["pilot_runs"]:
        run.update(
            measurement_protocol="bytewise-regenerated-v2",
            validation_worker_limit=run["workers"],
            validated_bytes=run["input_bytes"],
            validation_seconds=2.5,
            wall_seconds=10.0,
            measurement_identity=copy.deepcopy(identity),
            observation_wall_seconds=12.0,
        )
    record["sampling_policy"].update(
        suite_timeout_seconds=300,
        confirmation_wall_budget={
            "method": "maximum_pilot_observation_wall_plus_pause_times_frozen_counts",
            "remaining_seconds": 200,
            "estimated_seconds": 73.5,
            "fits_in_remaining_time": True,
        },
    )
    return record


# Purpose: Create a portable receipt-bound cohort for validator regressions.
# Inputs: None; fixture hashes and outputs are synthetic test data.
# Outputs: Complete test record, never performance or successful real-build evidence.
def receipt_fixture() -> dict:
    record = artifact_fixture()
    recipe = {key: "" for key in RECIPE_KEYS}
    recipe.update(
        CMAKE_GENERATOR="Visual Studio 18 2026",
        CMAKE_GENERATOR_PLATFORM="x64",
        FLITZZIP_ENABLE_HIP="ON",
        FLITZZIP_HIP_ARCH="gfx1201",
        FLITZZIP_PACKAGE_VERSION="0.8.0",
        FLITZZIP_MSI_INSTALL_SCOPE="perMachine",
        FLITZZIP_BUILD_GUI="ON",
        FLITZZIP_BUILD_TESTS="ON",
        configuration="Release",
        configured_flags_sha256="f" * 64,
    )
    files = {"CMakeLists.txt": "a" * 64, "src/fixture.cpp": "a" * 64}
    build = {
        "schema_version": 1,
        "kind": "successful-native-invocation-v1",
        "transaction_id": "a" * 32,
        "status": "successful",
        "clean_first": True,
        "recipe": recipe,
        "inputs": {
            "schema_version": 1,
            "projection": "native-invocation-inputs-v1",
            "files": files,
            "inputs_sha256": hashlib.sha256(canonical(files)).hexdigest(),
        },
        "toolchain": {
            "scope": "cmake-msvc-abi-probe-and-critical-compiler-files-v2",
            "cmake_version": "4.4.3",
            "cmake_sha256": "a" * 64,
            "cmake_abi_header_sha256": "b" * 64,
            "host_compiler_id": "MSVC",
            "host_compiler_version": "19.51.36260.0",
            "host_compiler_files_sha256": {name: "a" * 64 for name in ("cl.exe", "c1xx.dll", "c2.dll")},
            "hip_sdk": {
                "scope": "lock-plus-hipcc-clang-import-version-and-device-bitcode-v1",
                "lock_sha256": "a" * 64,
                "critical_files_sha256": {
                    name: "a" * 64
                    for name in (
                        "bin/hipcc.exe",
                        "lib/llvm/bin/clang.exe",
                        "lib/amdhip64.lib",
                        "include/hip/hip_version.h",
                        "lib/llvm/amdgcn/bitcode/ocml.bc",
                    )
                },
            },
        },
        "outputs_sha256": {
            "build/Release/flitzzip_cli.exe": "b" * 64,
            "build/Release/FlitzZip.exe": "a" * 64,
            "build/Release/fixture.dll": "c" * 64,
            "build/Release/flitzzip-runtime-dependencies.json": "a" * 64,
        },
    }
    record.update(
        native_build_receipt=build,
        native_build_receipt_sha256=hashlib.sha256(canonical(build)).hexdigest(),
        native_inputs_sha256=build["inputs"]["inputs_sha256"],
        measurement_identity_policy="native-build-receipt-around-observation-v2",
    )
    for run in record["runs"] + record["pilot_runs"]:
        run["measurement_identity"].update(
            native_build_receipt_sha256=record["native_build_receipt_sha256"],
            native_inputs_sha256=record["native_inputs_sha256"],
        )
    return record


class BenchmarkGraphTests(unittest.TestCase):
    # Purpose: Refuse malformed trials and incomplete chart populations at the measured-record boundary.
    # Inputs: Independent counter, phase, identity and sample mutations plus empty or undersampled chart sets.
    # Outputs: Each malformed record or population raises ValueError; no sample is repaired or silently dropped.
    def test_run_and_chart_population_boundaries(self) -> None:
        invalid = (
            (("profile",), "not a profile", "workload profile"),
            (("size_mib",), True, "workload size"),
            (("gpu_utilization_metric",), "unknown", "GPU utilization"),
            (("lane_order",), "unknown", "lane order"),
            (("source_commit",), "bad", "complete Git SHA"),
            (("binary_sha256",), "bad", "SHA-256 digest"),
            (("runs",), [], "no runs"),
            (("runs", 0, "lane"), None, "lane"),
            (("runs", 0, "iteration"), 0, "iteration"),
            (("runs", 0, "block_size_kib"), 1, "production block size"),
            (("runs", 0, "input_bytes"), 1, "input size differs"),
            (("runs", 0, "output_bytes"), True, "exact positive integer"),
            (("runs", 0, "compress_seconds"), -1, "phase timing"),
        )
        for path, value, diagnostic in invalid:
            record = fixture_record()
            if path[0] == "gpu_utilization_metric":
                record["schema_version"] = 2
            target = record
            for key in path[:-1]:
                target = target[key]
            target[path[-1]] = value
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, diagnostic):
                graph.validate_record(record, False)
        with self.assertRaisesRegex(ValueError, "no benchmark cases"):
            graph.summarize_records([], False)
        with self.assertRaisesRegex(ValueError, "duplicate run across"):
            graph.summarize_records([fixture_record(), fixture_record()], False)
        record = fixture_record()
        record["runs"].append(copy.deepcopy(record["runs"][0]))
        with self.assertRaisesRegex(ValueError, "duplicate lane iteration"):
            graph.validate_record(record, False)
        record = fixture_record()
        record["runs"] = record["runs"][:4]
        with self.assertRaisesRegex(ValueError, "at least three"):
            graph.summarize_records([record], False)
        record = fixture_record()
        record["runs"][2]["output_bytes"] += 1
        with self.assertRaisesRegex(ValueError, "encoded size changed"):
            graph.summarize_records([record], False)
        identity, rows = graph.summarize_records([fixture_record()], False)
        with self.assertRaisesRegex(ValueError, "16 explicit cases"):
            graph.render_svg(identity, rows * 17)

    # Purpose: Enforce sampling and provenance contracts even when inputs stop before full admission.
    # Inputs: Independently malformed fixed plans, frozen geometry, budgets, protocol declarations and native receipts.
    # Outputs: Each helper refuses its own invalid boundary without silently repairing or discarding observations.
    def test_sampling_and_provenance_boundary_contracts(self) -> None:
        cases = (
            (
                graph.validate_sampling_plan,
                lambda: sampling_fixture()["sampling_policy"],
                (("case_plans",), []),
                "missing fixed",
            ),
            (
                graph.validate_sampling_plan,
                lambda: sampling_fixture()["sampling_policy"],
                (("case_plans", 0), None),
                "invalid confirmation plan",
            ),
            (graph.validate_confirmation_wall_budget, artifact_fixture, (("sampling_policy",), None), "budget policy"),
            (
                graph.validate_confirmation_wall_budget,
                artifact_fixture,
                (("sampling_policy", "confirmation_wall_budget"), None),
                "wall budget",
            ),
            (
                graph.validate_confirmation_wall_budget,
                artifact_fixture,
                (("sampling_policy", "case_plans"), [None]),
                "wall budget plans",
            ),
            (
                graph.validate_confirmation_wall_budget,
                artifact_fixture,
                (("sampling_policy", "case_plans", 0, "confirmation_count"), True),
                "wall budget count",
            ),
            (
                graph.validate_confirmation_wall_budget,
                artifact_fixture,
                (("sampling_policy", "case_plans", 0, "block_size_kib"), 2048),
                "lacks a pilot case",
            ),
            (
                graph.validate_integrity_protocol,
                artifact_fixture,
                (("measurement_protocol",), "unknown"),
                "unsupported integrity",
            ),
            (graph.validate_integrity_protocol, artifact_fixture, (("pilot_runs", 0), None), "invalid bytewise"),
            (
                graph.validate_artifact_measurement_identity,
                artifact_fixture,
                (("pilot_runs", 0), None),
                "identity observation",
            ),
            (
                graph.validate_artifact_measurement_identity,
                artifact_fixture,
                (("measurement_identity_policy",), "unknown"),
                "unsupported measurement",
            ),
            (
                graph.validate_artifact_measurement_identity,
                artifact_fixture,
                (("native_build_receipt_sha256",), "a" * 64),
                "policy declaration",
            ),
            (
                graph.validate_native_receipt_record,
                receipt_fixture,
                (("binary_dependencies_sha256",), {}),
                "app-local DLLs",
            ),
        )
        for validate, factory, (path, value), diagnostic in cases:
            record = factory()
            target = record
            for key in path[:-1]:
                target = target[key]
            target[path[-1]] = value
            with self.subTest(validator=validate.__name__, path=path), self.assertRaisesRegex(ValueError, diagnostic):
                validate(record)
        for field, value, diagnostic in (
            ("geometry_plans", None, "missing frozen"),
            ("geometry_plans", [{"case": "CPU:1024", "workers": 0}], "geometry bounds"),
        ):
            record = sampling_fixture()
            record["sampling_policy"][field] = value
            with self.subTest(field=field, value=value), self.assertRaisesRegex(ValueError, diagnostic):
                graph.validate_frozen_geometry(record, record["sampling_policy"])
        record = artifact_fixture()
        record["sampling_policy"]["method"] = "fixed_count"
        graph.validate_confirmation_wall_budget(record)

    # Purpose: Require matching confirmation cases, complete serialized sizes and genuine HIP build scope.
    # Inputs: A wrong frozen case key, payload-only sizes for every observation and a valid CPU-only receipt.
    # Outputs: Each internally consistent-looking cohort is rejected at its missing scientific boundary.
    def test_consistent_but_incomplete_measurement_evidence(self) -> None:
        record = sampling_fixture()
        record["sampling_policy"]["case_plans"][0]["block_size_kib"] = 2048
        with self.assertRaisesRegex(ValueError, "does not cover"):
            graph.validate_record(record, False)
        record = sampling_fixture()
        for run in record["runs"] + record["pilot_runs"]:
            run["archive_bytes"] = run["output_bytes"]
        with self.assertRaisesRegex(ValueError, "serialized archive size"):
            graph.validate_record(record, False)
        record = receipt_fixture()
        receipt = record["native_build_receipt"]
        receipt["recipe"]["FLITZZIP_ENABLE_HIP"] = "OFF"
        del receipt["toolchain"]["hip_sdk"]
        record["native_build_receipt_sha256"] = validate_receipt(receipt)
        with self.assertRaisesRegex(ValueError, "CPU-only build receipt"):
            graph.validate_native_receipt_record(record)

    # Purpose: Prevent malformed host metadata and nonfinite aggregate timings from becoming scientific evidence.
    # Inputs: Independent mutations of the top-level object, hardware labels, disk counter and finite phase values.
    # Outputs: Every invalid record raises ValueError instead of being admitted or failing in rendering arithmetic.
    def test_record_shapes_and_aggregate_timing_are_validated(self) -> None:
        for value in (None, [], "record"):
            with self.subTest(root=value), self.assertRaises(ValueError):
                graph.validate_record(value, False)
        for path, value in (
            (("cpu_model",), None),
            (("gpu_model",), 7),
            (("cpu_model",), "  "),
            (("runs", 0, "disk_write_bytes"), False),
        ):
            record = fixture_record()
            target = record
            for key in path[:-1]:
                target = target[key]
            target[path[-1]] = value
            with self.subTest(path=path), self.assertRaises(ValueError):
                graph.validate_record(record, False)
        record = fixture_record()
        for phase in ("compress_seconds", "verify_seconds", "extract_seconds"):
            record["runs"][0][phase] = 1e308
        with self.assertRaisesRegex(ValueError, "elapsed time"):
            graph.validate_record(record, False)

    # Purpose: Preserve exact historical receipt reading without admitting it as a current build.
    # Inputs: Current fixture reduced to the complete original toolchain schema.
    # Outputs: Historical graph acceptance, strict live-build rejection and separate cohort identity.
    def test_historical_receipt_scope(self):
        current = receipt_fixture()
        historical = copy.deepcopy(current)
        receipt = historical["native_build_receipt"]
        receipt["toolchain"]["scope"] = "cmake-msvc-probe-and-critical-compiler-files-v1"
        receipt["toolchain"].pop("cmake_abi_header_sha256")
        digest = hashlib.sha256(canonical(receipt)).hexdigest()
        historical["native_build_receipt_sha256"] = digest
        for run in historical["runs"] + historical["pilot_runs"]:
            run["measurement_identity"]["native_build_receipt_sha256"] = digest
        graph.validate_record(historical, False)
        with self.assertRaisesRegex(ValueError, "toolchain schema"):
            validate_receipt(receipt)
        with self.assertRaises(ValueError):
            graph.summarize_records([historical, current], False)
        receipt["toolchain"]["cmake_abi_header_sha256"] = "a" * 64
        with self.assertRaisesRegex(ValueError, "toolchain schema"):
            graph.validate_record(historical, False)

    # Purpose: Exercise the actual reviewed publication consumer when shared schemas change.
    # Inputs: Immutable historical beta record and its checked-in reviewed chart.
    # Outputs: Requires byte-identical graph regeneration without inventing newer toolchain evidence.
    def test_reviewed_historical_beta_regeneration(self):
        root = Path(__file__).resolve().parents[1]
        record = json.loads((root / "docs/benchmarks/data/fixtures/native-v1.json").read_text("utf-8"))
        self.assertNotIn("cmake_abi_header_sha256", record["native_build_receipt"]["toolchain"])
        identity, rows = graph.summarize_records([record], False)
        expected = (root / "resources/benchmarks/fixtures/native-v1.svg").read_bytes()
        self.assertEqual(graph.render_svg(identity, rows), expected)
        original = copy.deepcopy(record)
        with self.assertRaises(ValueError):
            graph.validate_receipt(record["native_build_receipt"])
        self.assertEqual(record, original)

    # Purpose: Reject inconsistent, private, incomplete and CPU-fallback receipt-backed publication metadata.
    # Inputs: Independently mutated exported fixture receipts.
    # Outputs: Accepts one complete fixture and rejects inconsistent mutations.
    def test_receipt_backed_publication(self):
        record = receipt_fixture()
        graph.validate_record(record, False)
        for section, key, value in (
            ("inputs", "inputs_sha256", "0" * 64),
            ("recipe", "FLITZZIP_ENABLE_HIP", "OFF"),
            ("recipe", "configuration", "Debug"),
            ("toolchain", "host_compiler_version", "C:/private/compiler"),
            ("outputs_sha256", "build/Release/flitzzip_cli.exe", "0" * 64),
        ):
            changed = copy.deepcopy(record)
            changed["native_build_receipt"][section][key] = value
            changed["native_build_receipt_sha256"] = hashlib.sha256(
                canonical(changed["native_build_receipt"])
            ).hexdigest()
            with self.subTest(section=section, key=key), self.assertRaises(ValueError):
                graph.validate_record(changed, False)
        for key in ("native_build_receipt", "native_build_receipt_sha256", "native_inputs_sha256"):
            changed = copy.deepcopy(record)
            del changed[key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                graph.validate_record(changed, False)
        changed = copy.deepcopy(record)
        changed["runs"][0]["measurement_identity"]["native_inputs_sha256"] = "0" * 64
        with self.assertRaises(ValueError):
            graph.validate_record(changed, False)

    # Purpose: Require the raw artifact cohort and observation lifetime, preserving historical compatibility honestly.
    # Inputs: One current record with independently mutated source, binary, runtime and wall-time fields.
    # Outputs: Rejects all identity changes and missing policy declarations before publishing a chart.
    def test_frozen_artifact_observation_identity(self) -> None:
        record = artifact_fixture()
        graph.validate_record(record, False)
        for collection in ("runs", "pilot_runs"):
            for field, value in (
                ("source_commit", "d" * 40),
                ("source_dirty", 0),
                ("binary_sha256", "d" * 64),
                ("binary_dependencies_sha256", {"fixture.dll": "d" * 64}),
            ):
                mutated = copy.deepcopy(record)
                mutated[collection][0]["measurement_identity"][field] = value
                with self.assertRaisesRegex(ValueError, "measurement identity"):
                    graph.validate_record(mutated, False)
            for wall in (None, True, float("nan"), float("inf"), 9.5):
                mutated = copy.deepcopy(record)
                mutated[collection][0]["observation_wall_seconds"] = wall
                with self.assertRaisesRegex(ValueError, "measurement identity"):
                    graph.validate_record(mutated, False)
        missing = copy.deepcopy(record)
        missing.pop("measurement_identity_policy")
        with self.assertRaisesRegex(ValueError, "policy declaration"):
            graph.validate_record(missing, False)
        for name in ("../fixture.dll", "C:fixture.dll", "fixture.exe"):
            mutated = copy.deepcopy(record)
            mutated["binary_dependencies_sha256"] = {name: "c" * 64}
            with self.assertRaisesRegex(ValueError, "runtime identity"):
                graph.validate_record(mutated, False)

    # Purpose: Derive confirmation wall cost from the raw pilot lifetimes rather than trusting a saved fit label.
    # Inputs: A valid frozen count with malformed, forged or insufficient budget metadata.
    # Outputs: Rejects every disagreement; historical records remain readable without invented budget fields.
    def test_confirmation_wall_budget_evidence(self) -> None:
        record = artifact_fixture()
        for field, value in (
            ("method", "discard_slow_pilots"),
            ("remaining_seconds", 60),
            ("remaining_seconds", 301),
            ("estimated_seconds", 72),
            ("estimated_seconds", float("nan")),
            ("fits_in_remaining_time", False),
        ):
            mutated = copy.deepcopy(record)
            mutated["sampling_policy"]["confirmation_wall_budget"][field] = value
            with self.assertRaisesRegex(ValueError, "confirmation wall budget"):
                graph.validate_record(mutated, False)
        missing = copy.deepcopy(record)
        missing["sampling_policy"].pop("confirmation_wall_budget")
        with self.assertRaisesRegex(ValueError, "confirmation wall budget"):
            graph.validate_record(missing, False)

    # Purpose: Admit complete bounded studies above the historical 64-run limit without weakening evidence checks.
    # Inputs: Deterministic paired fixtures, prescribed extended counts, and malformed budget/count mutations.
    # Outputs: Validates all retained observations and rejects incomplete samples, oversized counts and forged budgets.
    def test_extended_confirmation_counts(self) -> None:
        record = artifact_fixture()
        templates = record["runs"][:2]
        record["runs"] = [
            dict(copy.deepcopy(run), iteration=iteration)
            for iteration in range(1, 66)
            for run in (templates if iteration % 2 else reversed(templates))
        ]
        policy = record["sampling_policy"]
        policy["maximum_count"] = graph.MAX_CONFIRMATION_COUNT
        policy["case_plans"][0].update(requested_count=65, confirmation_count=65)
        policy["suite_timeout_seconds"] = 3600
        policy["confirmation_wall_budget"].update(estimated_seconds=65 * 24.5, remaining_seconds=2000)
        _, rows = graph.summarize_records([record], allow_dirty=False)
        self.assertEqual(rows[0]["iterations"], 65)
        self.assertEqual(len(record["runs"]), 130)
        for field, value in (("maximum_count", 1025), ("minimum_count", 1025)):
            invalid = copy.deepcopy(policy)
            invalid[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "count bound"):
                graph.validate_sampling_plan(invalid)
        for requested in (20_000_000_000, graph.MAX_EXACT_REQUESTED_COUNT):
            capped = copy.deepcopy(policy)
            capped["case_plans"][0].update(
                requested_count=requested, confirmation_count=graph.MAX_CONFIRMATION_COUNT, count_capped=True
            )
            self.assertEqual(graph.validate_sampling_plan(capped), {1024: 1024})
        invalid = copy.deepcopy(policy)
        invalid["case_plans"][0].update(requested_count=2**53, confirmation_count=1024, count_capped=True)
        with self.assertRaisesRegex(ValueError, "confirmation count"):
            graph.validate_sampling_plan(invalid)
        incomplete = copy.deepcopy(record)
        incomplete["runs"].pop()
        with self.assertRaisesRegex(ValueError, "confirmation sample is incomplete"):
            graph.validate_record(incomplete, False)
        forged = copy.deepcopy(record)
        forged["sampling_policy"]["confirmation_wall_budget"]["estimated_seconds"] -= 1
        with self.assertRaisesRegex(ValueError, "differs from raw pilots"):
            graph.validate_record(forged, False)

    # Purpose: Reject CRC-only, partial, or malformed observations labeled as fully bytewise validated.
    # Inputs: Current sampling fixtures with independent protocol and coverage mutations.
    # Outputs: Requires all pilots/confirmations to match and keeps historical protocols incompatible.
    def test_bytewise_integrity_protocol_coverage(self) -> None:
        record = sampling_fixture()
        record["measurement_protocol"] = "bytewise-regenerated-v1"
        for run in record["runs"] + record["pilot_runs"]:
            run.update(
                measurement_protocol="bytewise-regenerated-v1",
                validated_bytes=run["input_bytes"],
                validation_seconds=2.5,
                wall_seconds=10.0,
            )
        identity, _ = graph.validate_record(record, False)
        self.assertNotEqual(identity, graph.validate_record(sampling_fixture(), False)[0])
        missing = copy.deepcopy(record)
        missing.pop("measurement_protocol")
        with self.assertRaisesRegex(ValueError, "protocol declaration"):
            graph.validate_record(missing, False)
        malformed = copy.deepcopy(record)
        malformed["pilot_runs"] = None
        with self.assertRaisesRegex(ValueError, "observation collections"):
            graph.validate_record(malformed, False)
        for collection in ("runs", "pilot_runs"):
            for field, value in (
                ("measurement_protocol", None),
                ("validated_bytes", True),
                ("validated_bytes", 1),
                ("validation_seconds", float("nan")),
                ("validation_seconds", 0),
                ("wall_seconds", 1),
            ):
                mutated = copy.deepcopy(record)
                mutated[collection][0][field] = value
                with self.assertRaisesRegex(ValueError, "bytewise validation"):
                    graph.validate_record(mutated, False)

    # Purpose: Keep parallel validation geometry explicit and separate from the serial historical protocol.
    # Inputs: Complete current observations plus missing, fractional, boolean and changed worker limits.
    # Outputs: Accepts valid v2 coverage, rejects malformed budgets and refuses mixed protocol identities.
    def test_parallel_bytewise_worker_budget(self) -> None:
        record = sampling_fixture()
        record["measurement_protocol"] = "bytewise-regenerated-v2"
        for run in record["runs"] + record["pilot_runs"]:
            run.update(
                measurement_protocol="bytewise-regenerated-v2",
                validation_worker_limit=run["workers"],
                validated_bytes=run["input_bytes"],
                validation_seconds=2.5,
                wall_seconds=10.0,
            )
        identity, _ = graph.validate_record(record, False)
        serial = copy.deepcopy(record)
        serial["measurement_protocol"] = "bytewise-regenerated-v1"
        for run in serial["runs"] + serial["pilot_runs"]:
            run["measurement_protocol"] = "bytewise-regenerated-v1"
            run.pop("validation_worker_limit")
        self.assertNotEqual(identity, graph.validate_record(serial, False)[0])
        for collection in ("runs", "pilot_runs"):
            for value in (None, True, 0, 65, 32.5, 31):
                mutated = copy.deepcopy(record)
                mutated[collection][0]["validation_worker_limit"] = value
                with self.assertRaisesRegex(ValueError, "validation worker budget"):
                    graph.validate_record(mutated, False)
        missing = copy.deepcopy(record)
        missing.pop("measurement_protocol")
        with self.assertRaisesRegex(ValueError, "protocol declaration"):
            graph.validate_record(missing, False)

    # Purpose: Refuse stable-looking measurements that do not follow their frozen admission plan.
    # Inputs: Valid evidence with missing, altered, duplicate or unrelated geometry plans.
    # Outputs: Every configuration mismatch raises instead of producing a publishable chart.
    def test_predeclared_geometry_is_checked_independently(self) -> None:
        for mutation in ("missing", "changed", "duplicate", "unmatched"):
            record = sampling_fixture()
            plans = record["sampling_policy"]["geometry_plans"]
            if mutation == "missing":
                record["sampling_policy"].pop("geometry_policy")
            elif mutation == "changed":
                plans[0]["inflight_chunks"] = 31
            elif mutation == "duplicate":
                plans.append(copy.deepcopy(plans[0]))
            else:
                extra = copy.deepcopy(plans[0])
                extra["case"] = "CPU:256"
                plans.append(extra)
            with self.subTest(mutation=mutation), self.assertRaisesRegex(ValueError, "geometry"):
                graph.summarize_records([record], allow_dirty=False)
        record = sampling_fixture()
        pilot = copy.deepcopy(record["pilot_runs"][0])
        pilot["block_size_kib"] = 256
        record["pilot_runs"].append(pilot)
        plan = copy.deepcopy(record["sampling_policy"]["geometry_plans"][0])
        plan["case"] = "CPU:256"
        record["sampling_policy"]["geometry_plans"].append(plan)
        with self.assertRaisesRegex(ValueError, "pilot contains unmatched cases"):
            graph.summarize_records([record], allow_dirty=False)

    # Purpose: Preserve all timing variation and label inverse-median throughput precisely.
    # Inputs: Complete fixed-size observations with small, deliberately nonzero timing differences.
    # Outputs: Exact sample extrema, SD and range whiskers are retained without a confidence claim.
    def test_current_charts_preserve_full_sample_dispersion(self) -> None:
        record = sampling_fixture()
        for run in record["runs"]:
            for field in ("compress_seconds", "verify_seconds", "extract_seconds"):
                run[field] *= {1: 0.99, 2: 1.0, 3: 1.01}[run["iteration"]]
        identity, rows = graph.summarize_records([record], allow_dirty=False)
        for lane, metric in rows[0]["metrics"].items():
            timings = [
                sum(run[field] for field in ("compress_seconds", "verify_seconds", "extract_seconds"))
                for run in record["runs"]
                if run["lane"] == lane
            ]
            self.assertEqual(metric["throughput_min_gib_s"], 10 / max(timings))
            self.assertEqual(metric["throughput_max_gib_s"], 10 / min(timings))
            self.assertEqual(metric["sample_count"], 3)
            self.assertGreater(metric["elapsed_std_dev_seconds"], 0)
        root = parse_bounded_document(graph.render_svg(identity, rows))
        ranges = root.findall(f'.//{{{graph.SVG}}}g[@class="sample-range"]')
        self.assertIn("encoded payload bytes, excluding archive metadata", root.find(f"{{{graph.SVG}}}desc").text)
        self.assertEqual(rows[0]["metrics"]["GPU"]["output_bytes"], record["runs"][1]["output_bytes"])
        self.assertNotEqual(rows[0]["metrics"]["GPU"]["output_bytes"], record["runs"][1]["archive_bytes"])
        self.assertEqual(len(ranges), 2)
        for group in ranges:
            self.assertIn("not a confidence interval", group.find(f"{{{graph.SVG}}}title").text)
            self.assertEqual(len(group.findall(f"{{{graph.SVG}}}line")), 3)
        self.assertIn(b"median elapsed time", graph.render_svg(identity, rows))

    # Purpose: Keep identical observations honest instead of inventing a visual uncertainty width.
    # Inputs: Zero-variance current fixtures and unchanged historical chart evidence.
    # Outputs: Zero-variance ranges have zero width; historical schemas retain their observed dispersion.
    def test_zero_variance_range_is_not_artificially_widened(self) -> None:
        identity, rows = graph.summarize_records([sampling_fixture()], allow_dirty=False)
        root = parse_bounded_document(graph.render_svg(identity, rows))
        for group in root.findall(f'.//{{{graph.SVG}}}g[@class="sample-range"]'):
            line = group.find(f"{{{graph.SVG}}}line")
            self.assertEqual(line.attrib["x1"], line.attrib["x2"])
        identity, rows = graph.summarize_records([fixture_record()], allow_dirty=False)
        self.assertIn(b"sample-range", graph.render_svg(identity, rows))

    # Purpose: Enforce separate pilot retention and a prescribed complete confirmation count.
    # Inputs: Stable evidence and mutations of the predeclared method, sample count and preservation fields.
    # Outputs: Valid fixed experiments render; incomplete or undisclosed sampling fails closed.
    def test_sampling_protocol_requires_complete_retained_observations(self) -> None:
        record = sampling_fixture()
        _, rows = graph.summarize_records([record], allow_dirty=False)
        self.assertEqual(rows[0]["iterations"], 3)
        fixed = copy.deepcopy(record)
        fixed["pilot_runs"] = []
        fixed["sampling_policy"].update(method="fixed_count", pilot_count=0)
        graph.summarize_records([fixed], allow_dirty=False)
        for field, value in (
            ("discarded_sample_count", 1),
            ("discarded_sample_count", False),
            ("warmup_count", 1),
            ("pilot_count", 2),
            ("minimum_measured_seconds", float("nan")),
            ("maximum_count", 2),
            ("method", "stop_when_significant"),
        ):
            invalid = copy.deepcopy(record)
            invalid["sampling_policy"][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                graph.summarize_records([invalid], allow_dirty=False)
        for field in ("pilot_runs", "runs"):
            invalid = copy.deepcopy(record)
            invalid[field].pop()
            with self.subTest(field=field), self.assertRaises(ValueError):
                graph.summarize_records([invalid], allow_dirty=False)
        invalid = copy.deepcopy(record)
        invalid["sampling_policy"]["case_plans"][0]["confirmation_count"] = 4
        with self.assertRaisesRegex(ValueError, "confirmation count"):
            graph.summarize_records([invalid], allow_dirty=False)

    # Purpose: Prevent slow tails, absent telemetry or resource changes from being hidden by a stored quality label.
    # Inputs: Raw confirmation and pilot mutations plus a deliberately false stable summary.
    # Outputs: Scientific raw-data validation rejects every incompatible or inconclusive record.
    def test_raw_sampling_quality_is_recomputed_without_trimming(self) -> None:
        for stage, field, value in (
            ("runs", "compress_seconds", 100),
            ("runs", "verify_seconds", 0),
            ("runs", "workers", 16),
            ("pilot_runs", "archive_bytes", 123),
            ("pilot_runs", "inflight_chunks", 65),
            ("runs", "resource_sample_count", None),
            ("runs", "gpu_h2d_bytes", 0),
            ("runs", "gpu_sample_count", 0),
        ):
            invalid = sampling_fixture()
            invalid[stage][-1][field] = value
            invalid["case_quality"] = [{"status": "descriptively_stable"}]
            with self.subTest(stage=stage, field=field), self.assertRaises(ValueError):
                graph.summarize_records([invalid], allow_dirty=False)
        record = sampling_fixture()
        record["sampling_policy"]["minimum_measured_seconds"] = 600
        with self.assertRaisesRegex(ValueError, "measured confirmation time"):
            graph.summarize_records([record], allow_dirty=False)

    # Purpose: Keep graphs from silently combining driver runtime changes or inventing old provenance.
    # Inputs: Identified, historical/missing, unavailable, mismatched, and malformed runtime versions.
    # Outputs: Matching records render; unknown/known and differing runtime identities cannot be combined.
    def test_runtime_version_provenance_is_preserved(self) -> None:
        identified = fixture_record()
        identified["hip_runtime_version"] = "10.0.3679.0"
        same = copy.deepcopy(identified)
        same["profile"] = "Mixed"
        graph.summarize_records([identified, same], allow_dirty=False)
        for value in (None, "unavailable", "10.0.3665.0"):
            other = copy.deepcopy(same)
            other["hip_runtime_version"] = value
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "different binaries"):
                graph.summarize_records([identified, other], allow_dirty=False)
        for value in (True, 10, "", "10.0.65536.0", "010.0.3679.0", "10.0.3679.0/path", "10.0.3679"):
            other = copy.deepcopy(identified)
            other["hip_runtime_version"] = value
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "HIP runtime"):
                graph.summarize_records([other], allow_dirty=False)
        unknown = fixture_record()
        unknown["hip_runtime_version"] = "unavailable"
        unknown["profile"] = "Mixed"
        graph.summarize_records([fixture_record(), unknown], allow_dirty=False)

    # Purpose: Preserve historical charts without mixing incompatible resource measurement methods.
    # Inputs: Historical schema-one and current busiest-engine schema-two records.
    # Outputs: Both versions render alone; mixed versions and undefined GPU metrics are rejected.
    def test_resource_schema_versions_are_explicit(self) -> None:
        historical = fixture_record()
        current = copy.deepcopy(historical)
        current["schema_version"] = 2
        current["gpu_utilization_metric"] = "process_busiest_engine_pct"
        identity, rows = graph.summarize_records([current], allow_dirty=False)
        self.assertIn(b"Synthetic workloads only", graph.render_svg(identity, rows))
        reordered = copy.deepcopy(current)
        reordered["profile"] = "Compressible"
        reordered["lane_order"] = "alternating_when_both"
        reordered["inter_run_pause_ms"] = 250
        with self.assertRaisesRegex(ValueError, "measurement schemas"):
            graph.summarize_records([current, reordered], allow_dirty=False)
        graph.summarize_records([reordered], allow_dirty=False)
        for field, value in (("inter_run_pause_ms", -1), ("inter_run_pause_ms", True), ("lane_order", "random")):
            invalid = copy.deepcopy(reordered)
            invalid[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "lane order or pause"):
                graph.summarize_records([invalid], allow_dirty=False)
        historical["profile"] = "Mixed"
        with self.assertRaisesRegex(ValueError, "measurement schemas"):
            graph.summarize_records([historical, current], allow_dirty=False)
        current["gpu_utilization_metric"] = "summed_engines"
        with self.assertRaisesRegex(ValueError, "GPU utilization metric"):
            graph.summarize_records([current], allow_dirty=False)
        current["schema_version"] = True
        with self.assertRaisesRegex(ValueError, "schema"):
            graph.summarize_records([current], allow_dirty=False)
        current["schema_version"] = 4
        current["measurement_protocol"] = "bytewise-corpus-v1"
        with self.assertRaisesRegex(ValueError, "unsupported RAM benchmark schema"):
            graph.summarize_records([current], allow_dirty=False)

    # Purpose: Reject invalid utilization in the corrected resource schema without rewriting historical evidence.
    # Inputs: Schema-two records with a nonfinite, negative, excessive, or nonnumeric GPU percentage.
    # Outputs: All invalid values fail validation; unavailable and boundary percentages remain accepted.
    def test_busiest_engine_percentages_are_bounded(self) -> None:
        record = fixture_record()
        record["schema_version"] = 2
        record["gpu_utilization_metric"] = "process_busiest_engine_pct"
        for field in ("gpu_avg_pct", "gpu_peak_pct"):
            for value in (float("nan"), float("inf"), -1, 101, True, "90"):
                record["runs"][1][field] = value
                with self.subTest(field=field, value=value), self.assertRaisesRegex(ValueError, "GPU percentage"):
                    graph.summarize_records([record], allow_dirty=False)
            for value in (None, 0, 100):
                record["runs"][1][field] = value
                graph.summarize_records([record], allow_dirty=False)
            record["runs"][1][field] = None

    # Purpose: Verify exact bytes, medians, accessibility metadata, and deterministic SVG output.
    # Inputs: One valid paired-run fixture.
    # Outputs: A reproducible SVG whose metadata distinguishes synthetic data from broad claims.
    def test_valid_record_renders_deterministically(self) -> None:
        identity, rows = graph.summarize_records([fixture_record()], allow_dirty=False)
        self.assertEqual(rows[0]["metrics"]["GPU"]["output_bytes"], 171079680)
        self.assertAlmostEqual(rows[0]["metrics"]["GPU"]["throughput_gib_s"], 10 / 4.6)
        first = graph.render_svg(identity, rows)
        self.assertEqual(first, graph.render_svg(identity, rows))
        self.assertTrue(first.endswith(b"\r\n"))
        self.assertEqual(first.count(b"\n"), first.count(b"\r\n"))
        root = parse_bounded_document(first)
        self.assertEqual(root.attrib["role"], "img")
        self.assertIn("encoded payload size", root.find(f"{{{graph.SVG}}}desc").text)
        self.assertIn(b"Synthetic workloads only", first)
        self.assertIn(b"171,079,680 encoded bytes", first)

    # Purpose: Keep unreviewed worktree results out of publication while allowing labeled local previews.
    # Inputs: One otherwise valid record marked dirty.
    # Outputs: Publication validation rejects it; preview validation admits it explicitly.
    def test_dirty_source_requires_preview_opt_in(self) -> None:
        record = fixture_record()
        record["source_dirty"] = True
        with self.assertRaisesRegex(ValueError, "clean"):
            graph.summarize_records([record], allow_dirty=False)
        identity, rows = graph.summarize_records([record], allow_dirty=True)
        self.assertIn(b"DIRTY SOURCE - LOCAL PREVIEW ONLY", graph.render_svg(identity, rows))

    # Purpose: Reject a GPU lane whose native work or RAM-only provenance is not established.
    # Inputs: Mutated launch count, storage bytes, and lane pairing.
    # Outputs: Every invalid record fails before a chart can be emitted.
    def test_gpu_and_ram_evidence_is_required(self) -> None:
        for key, value in (("gpu_kernel_launches", 0), ("memory_only", False), ("disk_write_bytes", 1)):
            record = fixture_record()
            record["runs"][1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                graph.summarize_records([record], allow_dirty=False)
        record = fixture_record()
        record["runs"] = [run for run in record["runs"] if not (run["lane"] == "GPU" and run["iteration"] == 3)]
        with self.assertRaisesRegex(ValueError, "paired"):
            graph.summarize_records([record], allow_dirty=False)

    # Purpose: Refuse charts that average differing payloads or mix source binaries.
    # Inputs: A changed size within a lane and a second record from another commit.
    # Outputs: Both scientific comparison errors fail closed.
    def test_size_and_identity_must_be_stable(self) -> None:
        record = fixture_record()
        record["runs"][3]["output_bytes"] += 1
        with self.assertRaisesRegex(ValueError, "encoded size changed"):
            graph.summarize_records([record], allow_dirty=False)
        other = copy.deepcopy(fixture_record())
        other["source_commit"] = "c" * 40
        other["profile"] = "Mixed"
        with self.assertRaisesRegex(ValueError, "different binaries"):
            graph.summarize_records([fixture_record(), other], allow_dirty=False)

    # Purpose: Reject non-finite timings and noninteger encoded sizes that would distort a chart.
    # Inputs: One NaN phase or fractional output count.
    # Outputs: Validation raises ValueError for both cases.
    def test_numeric_fields_are_bounded(self) -> None:
        for key, value in (("compress_seconds", float("nan")), ("output_bytes", 171079680.5)):
            record = fixture_record()
            record["runs"][1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                graph.summarize_records([record], allow_dirty=False)


if __name__ == "__main__":
    unittest.main()
