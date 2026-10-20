"""Regression tests for reviewed multi-effort comparison graphs."""

from __future__ import annotations

import copy
import math
import unittest

from tools.render_svg_graph import SVG, parse_bounded_document
from tools.render_tradeoff_graph import LEVELS, collect, frontier, render, y_of
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
    # Purpose: Preserve logarithmic geometry when a valid time ratio overflows ordinary floating-point division.
    # Inputs: Positive finite bounds separated by six hundred decades and their geometric midpoint.
    # Outputs: The midpoint maps to the center instead of silently collapsing onto the lower plot edge.
    def test_logarithmic_coordinate_does_not_form_overflowing_ratios(self) -> None:
        self.assertAlmostEqual(y_of(1.0, 100, 0, 1e-300, 1e300), 50.0)
        self.assertAlmostEqual(y_of(1e-300, 100, 0, 1e-300, 1e300), 100.0)
        self.assertAlmostEqual(y_of(1e300, 100, 0, 1e-300, 1e300), 0.0)

    # Purpose: Keep finite positive timings renderable without axis underflow, overflow or unbounded tick density.
    # Inputs: Valid collected panels with subnormal, very large and widely separated timing observations.
    # Outputs: Every point remains inside its plot, numeric geometry stays finite and tick counts stay bounded.
    def test_extreme_finite_time_axes_preserve_point_geometry(self) -> None:
        for observations in ((5e-324,), (1e308,), (5e-324, 1.0, 1e308)):
            commit, panels = collect(sweep_fixture())
            for panel in panels:
                for index, point in enumerate(panel["points"]):
                    time = observations[index % len(observations)]
                    point["seconds"] = (time, time, time)
            with self.subTest(observations=observations):
                root = parse_bounded_document(render(commit, panels))
                for node in root.iter():
                    for name in ("x", "y", "cx", "cy", "x1", "x2", "y1", "y2", "r"):
                        if name in node.attrib:
                            self.assertTrue(math.isfinite(float(node.attrib[name])), (name, node.attrib[name]))
                groups = root.findall(f".//{{{SVG}}}g")
                self.assertEqual(len(groups), 30)
                for index, group in enumerate(groups):
                    point = group.find(f"{{{SVG}}}circle")
                    top = 125 + (index // 10) * 375
                    self.assertGreaterEqual(float(point.attrib["cy"]), top + 75 - 0.1)
                    self.assertLessEqual(float(point.attrib["cy"]), top + 304 + 0.1)
                horizontal = [
                    node
                    for node in root.findall(f".//{{{SVG}}}line")
                    if node.attrib.get("x1") == "72" and node.attrib.get("x2") == "646"
                ]
                self.assertLessEqual(len(horizontal), 24)
                table_values = [
                    node.text
                    for node in root.findall(f".//{{{SVG}}}text")
                    if node.attrib.get("x") == "998" and "[" in (node.text or "")
                ]
                self.assertTrue(all(len(text) <= 40 for text in table_values))

    # Purpose: Prevent positive sub-millisecond observations from being printed as zero-second timings.
    # Inputs: Valid collected panels with small nonzero median/minimum/maximum values.
    # Outputs: Labels retain their magnitude and never publish a false zero duration.
    def test_small_positive_time_labels_are_not_zero(self) -> None:
        commit, panels = collect(sweep_fixture())
        for panel in panels:
            for point in panel["points"]:
                point["seconds"] = (5e-5, 5e-5, 5e-5)
        root = parse_bounded_document(render(commit, panels))
        text = " ".join(node.text or "" for node in root.iter())
        self.assertNotRegex(text, r"(?<![\d.])0\.000(?!\d)")
        self.assertIn("5e-05", text)

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
        root = parse_bounded_document(image)
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
            {"tool": "FlitzZip", "level": 1, "bytes": 100, "seconds": (1.0, 0.9, 1.1)},
            {"tool": "FlitzZip", "level": 3, "bytes": 90, "seconds": (2.0, 1.9, 2.1)},
            {"tool": "7-Zip", "level": 5, "bytes": 110, "seconds": (3.0, 2.9, 3.1)},
        ]
        self.assertEqual(frontier(points), {("FlitzZip", 1), ("FlitzZip", 3)})


if __name__ == "__main__":
    unittest.main()
