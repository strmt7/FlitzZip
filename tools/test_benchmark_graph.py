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
    # Purpose: Verify exact bytes, medians, accessibility metadata, and deterministic SVG output.
    # Inputs: One valid paired-run fixture.
    # Outputs: A reproducible SVG whose metadata distinguishes synthetic data from broad claims.
    def test_valid_record_renders_deterministically(self) -> None:
        identity, rows = graph.summarize_records([fixture_record()], allow_dirty=False)
        self.assertEqual(rows[0]["metrics"]["GPU"]["output_bytes"], 171079680)
        self.assertAlmostEqual(rows[0]["metrics"]["GPU"]["throughput_gib_s"], 10 / 4.6)
        first = graph.render_svg(identity, rows)
        self.assertEqual(first, graph.render_svg(identity, rows))
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
