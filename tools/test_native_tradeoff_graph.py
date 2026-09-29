"""Regression tests for reviewed native CPU/GPU effort tradeoffs."""

from __future__ import annotations

import copy
import unittest
import xml.etree.ElementTree as ET

from tools.render_native_tradeoff_graph import LEVELS, collect, render
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
        root = ET.fromstring(image)
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
