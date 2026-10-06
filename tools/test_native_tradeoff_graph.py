"""Regression tests for reviewed native CPU/GPU effort tradeoffs."""

from __future__ import annotations

import copy
import json
import unittest
from pathlib import Path

from tools.render_benchmark_graph import validate_record
from tools.render_native_tradeoff_graph import LEVELS, collect, render
from tools.render_svg_graph import parse_bounded_document
from tools.test_benchmark_graph import fixture_record


# Purpose: Construct complete paired native records without performing a benchmark.
# Inputs: None.
# Outputs: Returns five same-binary records with exact, stable archive bytes.
def sweep_fixture() -> list[dict]:
    records = []
    for level in LEVELS:
        record = fixture_record()
        record["profile"] = "Mixed"
        record["compression_level"] = level
        for run in record["runs"]:
            run["block_size_kib"] = 16384
            run["archive_bytes"] = run["output_bytes"] + 1000 + level
            run["compress_seconds"] += level / 10 + run["iteration"] / 100
        records.append(record)
    return records


class NativeTradeoffGraphTests(unittest.TestCase):
    # Purpose: Preserve interpretation of legacy paired and GPU-only records from one clean binary.
    # Inputs: Immutable regression fixtures, including a real nonmonotonic effort-size observation.
    # Outputs: Rejects missing, inconsistent, repeated-size, or mislabeled diagnostic evidence.
    def test_legacy_effort_record_sizes(self) -> None:
        data = Path(__file__).resolve().parents[1] / "docs/benchmarks/data/fixtures/effort"
        identity = None
        sizes = {}
        for level in range(1, 10):
            suffix = f"effort-native-L{level}.json" if level in LEVELS else f"effort-native-diagnostic-L{level}.json"
            record = json.loads((data / suffix).read_text(encoding="utf-8"))
            current_identity, _ = validate_record(record, allow_dirty=False)
            self.assertEqual(record["compression_level"], level)
            self.assertEqual(record["profile"], "Mixed")
            self.assertEqual(len(record["runs"]), 6 if level in LEVELS else 1)
            if identity is None:
                identity = current_identity
            self.assertEqual(current_identity, identity)
            gpu_runs = [run for run in record["runs"] if run["lane"] == "GPU"]
            self.assertEqual(len(gpu_runs), 3 if level in LEVELS else 1)
            self.assertEqual({run["block_size_kib"] for run in gpu_runs}, {16384})
            self.assertEqual(len({run["archive_bytes"] for run in gpu_runs}), 1)
            self.assertIs(type(gpu_runs[0]["archive_bytes"]), int)
            self.assertGreater(gpu_runs[0]["archive_bytes"], 0)
            sizes[level] = gpu_runs[0]["archive_bytes"]
        self.assertEqual(len(set(sizes.values())), 9)
        self.assertEqual(sizes[6] - sizes[5], 1660)

    # Purpose: Prove a complete sweep renders deterministically with accessible exact-size evidence.
    # Inputs: Same-host synthetic paired records at all five levels.
    # Outputs: Ten plotted points and byte-identical CRLF SVG renders.
    def test_complete_sweep_renders(self) -> None:
        identity, points = collect(sweep_fixture())
        self.assertEqual(len(points), 10)
        image = render(identity, points)
        self.assertEqual(image, render(identity, points))
        self.assertIn(b"truncated axis", image)
        self.assertIn(b"257,130,692", image)
        self.assertEqual(image.count(b"\n"), image.count(b"\r\n"))
        root = parse_bounded_document(image)
        self.assertEqual(root.attrib["role"], "img")

    # Purpose: Prevent incomplete, dirty, or mismatched native records from being published.
    # Inputs: Independently corrupted effort sweeps.
    # Outputs: Each mutation is rejected before chart generation.
    def test_invalid_evidence_is_rejected(self) -> None:
        for mutation in ("missing", "duplicate", "dirty", "source", "profile", "iterations", "size", "disk", "gpu"):
            records = copy.deepcopy(sweep_fixture())
            if mutation == "missing":
                records.pop()
            elif mutation == "duplicate":
                records[-1]["compression_level"] = 1
            elif mutation == "dirty":
                records[0]["source_dirty"] = True
            elif mutation == "source":
                records[0]["source_commit"] = "c" * 40
            elif mutation == "profile":
                records[0]["profile"] = "Compressible"
            elif mutation == "iterations":
                records[0]["runs"].pop()
            elif mutation == "size":
                records[0]["runs"][0]["archive_bytes"] += 1
            elif mutation == "disk":
                records[0]["runs"][0]["disk_write_bytes"] = 1
            else:
                records[0]["runs"][1]["gpu_kernel_launches"] = 0
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                collect(records)


if __name__ == "__main__":
    unittest.main()
