"""Regression tests for evidence validation and deterministic benchmark graphs."""

from __future__ import annotations

import copy
import unittest
import xml.etree.ElementTree as ET

from tools import render_benchmark_graph as graph


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
        "benchmark_kind": "suzip_ram",
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


class BenchmarkGraphTests(unittest.TestCase):
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
        root = ET.fromstring(first)
        self.assertEqual(root.attrib["role"], "img")
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
