"""Regression tests for reviewed multi-effort comparison graphs."""

from __future__ import annotations

import copy
import unittest
import xml.etree.ElementTree as ET

from tools.render_tradeoff_graph import LEVELS, collect, frontier, render
from tools.run_archive_comparison import command_templates
from tools.test_comparison_graph import fixture


# Purpose: Construct a complete deterministic five-effort test sweep.
# Inputs: None; synthetic but schema-correct comparison records are used.
# Outputs: Returns ten-sample records with distinct archive sizes and commands.
def sweep_fixture() -> list[dict]:
    records = []
    for level in LEVELS:
        record = fixture()
        record["settings"]["level"] = level
        record["settings"]["runs"] = 10
        record["settings"]["round_pause_ms"] = 250
        for case in record["cases"]:
            for result in case["results"]:
                result["create_argv"], result["extract_argv"] = command_templates(
                    result["tool"], case["format"], tuple(case["files"]), level
                )
                result["archive_bytes"] = [result["archive_bytes"][0] - level * 100] * 10
                result["archive_sha256"] = ["b" * 64] * 10
                result["compress_seconds"] = [level / 10 + step / 1000 for step in range(10)]
                result["extract_seconds"] = [0.2 + step / 1000 for step in range(10)]
            case["extract_reference_bytes"] = case["results"][1]["archive_bytes"][-1]
        records.append(record)
    return records


class TradeoffGraphTests(unittest.TestCase):
    # Purpose: Prove data collection and accessible SVG are deterministic and portable.
    # Inputs: A complete ten-run-per-point synthetic effort sweep.
    # Outputs: Valid three-case graph with CRLF bytes and explicit log-time disclosure.
    def test_complete_sweep_renders(self) -> None:
        commit, panels = collect(sweep_fixture())
        self.assertEqual(len(panels), 3)
        self.assertEqual(len(panels[0]["points"]), 10)
        image = render(commit, panels)
        self.assertEqual(image, render(commit, panels))
        self.assertIn(b"logarithmic", image)
        self.assertIn(b"?>\r\n<svg", image)
        self.assertNotIn(b"\n", image.replace(b"\r\n", b""))
        root = ET.fromstring(image)
        self.assertEqual(root.attrib["role"], "img")
        self.assertIn("Segoe UI", root.attrib["font-family"])

    # Purpose: Prevent incomplete, dirty, or method-inconsistent effort evidence from publication.
    # Inputs: Independently corrupted sweep variants.
    # Outputs: Every invalid variant is rejected before rendering.
    def test_incomplete_or_mixed_evidence_is_rejected(self) -> None:
        for mutation in ("missing", "duplicate", "short", "pause", "source", "host", "command", "decode"):
            records = copy.deepcopy(sweep_fixture())
            if mutation == "missing":
                records.pop()
            elif mutation == "duplicate":
                records[-1]["settings"]["level"] = 1
            elif mutation == "short":
                records[0]["settings"]["runs"] = 5
            elif mutation == "pause":
                records[0]["settings"]["round_pause_ms"] = 0
            elif mutation == "source":
                records[1]["source_commit"] = "f" * 40
            elif mutation == "host":
                records[1]["host"]["gpu_driver"] = "other driver"
            elif mutation == "command":
                records[2]["cases"][0]["results"][0]["create_argv"].append("--extra")
            else:
                records[3]["cases"][1]["results"][0]["independent_verified"] = False
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                collect(records)

    # Purpose: Check Pareto labeling uses size and time within a case only.
    # Inputs: Three simple measured points with one dominated candidate.
    # Outputs: Only the two non-dominated identities are returned.
    def test_frontier(self) -> None:
        points = [
            {"tool": "SuperZip", "level": 1, "bytes": 100, "seconds": (1.0, 0.9, 1.1)},
            {"tool": "SuperZip", "level": 3, "bytes": 90, "seconds": (2.0, 1.9, 2.1)},
            {"tool": "7-Zip", "level": 5, "bytes": 110, "seconds": (3.0, 2.9, 3.1)},
        ]
        self.assertEqual(frontier(points), {("SuperZip", 1), ("SuperZip", 3)})


if __name__ == "__main__":
    unittest.main()
