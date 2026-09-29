"""Validate effort-sweep records and render a size-versus-time SVG."""

from __future__ import annotations

import argparse
import json
import math
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from tools.render_comparison_graph import SVG, label, summarize

LEVELS = (1, 3, 5, 7, 9)
COLORS = {"SuperZip": "#087e83", "7-Zip": "#b15a22", "Zstd": "#b15a22"}
ET.register_namespace("", SVG)


# Purpose: Validate a complete same-host, same-binary effort sweep.
# Inputs: Five parsed application comparison records in arbitrary order.
# Outputs: Returns the source commit and case-wise points, rejecting missing or mixed evidence.
def collect(records: list[dict]) -> tuple[str, list[dict]]:
    if len(records) != len(LEVELS):
        raise ValueError("tradeoff graph requires exactly five effort records")
    indexed = {}
    for record in records:
        level = record.get("settings", {}).get("level")
        if type(level) is not int or level not in LEVELS or level in indexed:
            raise ValueError("duplicate or unsupported effort")
        if record.get("settings", {}).get("runs", 0) < 10:
            raise ValueError("tradeoff publication requires at least ten runs per point")
        if record.get("settings", {}).get("round_pause_ms") != 250:
            raise ValueError("tradeoff publication requires the declared 250 ms inter-round pause")
        indexed[level] = record
    if set(indexed) != set(LEVELS):
        raise ValueError("effort sweep is incomplete")
    first = indexed[LEVELS[0]]
    identity = (
        first.get("source_commit"),
        first.get("superzip_binary_sha256"),
        first.get("build"),
        first.get("corpus"),
        first.get("tools"),
        first.get("host", {}).get("cpu"),
        first.get("host", {}).get("gpu"),
        first.get("host", {}).get("storage_model"),
    )
    panels = []
    for level in LEVELS:
        record = indexed[level]
        actual_identity = (
            record.get("source_commit"),
            record.get("superzip_binary_sha256"),
            record.get("build"),
            record.get("corpus"),
            record.get("tools"),
            record.get("host", {}).get("cpu"),
            record.get("host", {}).get("gpu"),
            record.get("host", {}).get("storage_model"),
        )
        if actual_identity != identity:
            raise ValueError("effort records have different source, corpus, tool, build, or host identity")
        commit, rows = summarize(record, expected_level=level)
        if not panels:
            panels = [
                {"name": row["name"], "format": row["format"], "input_bytes": row["input_bytes"], "points": []}
                for row in rows
            ]
        for panel, row in zip(panels, rows, strict=True):
            if (panel["name"], panel["format"], panel["input_bytes"]) != (
                row["name"],
                row["format"],
                row["input_bytes"],
            ):
                raise ValueError("case identity changed across effort levels")
            for metric in row["metrics"]:
                panel["points"].append(
                    {
                        "level": level,
                        "tool": metric["tool"],
                        "bytes": metric["bytes"],
                        "seconds": metric["compression"],
                        "extract_seconds": metric["extraction"],
                    }
                )
    return commit, panels


# Purpose: Identify measured points with no strictly better size/time alternative in one case.
# Inputs: Points for one format and one immutable input corpus.
# Outputs: Returns point identities on the measured, not statistically inferred, Pareto frontier.
def frontier(points: list[dict]) -> set[tuple[str, int]]:
    return {
        (point["tool"], point["level"])
        for point in points
        if not any(
            other is not point
            and other["bytes"] <= point["bytes"]
            and other["seconds"][0] <= point["seconds"][0]
            and (other["bytes"] < point["bytes"] or other["seconds"][0] < point["seconds"][0])
            for other in points
        )
    }


# Purpose: Map exact archive bytes to a zero-origin horizontal plot axis.
# Inputs: Archive bytes, panel x bounds, and the maximum plotted byte count.
# Outputs: Returns an SVG x coordinate.
def x_of(value: int, left: int, right: int, largest_bytes: int) -> float:
    return left + (right - left) * value / largest_bytes


# Purpose: Map positive wall time to the explicitly labeled logarithmic plot axis.
# Inputs: Seconds, panel y bounds, and positive lower/upper time limits.
# Outputs: Returns an SVG y coordinate.
def y_of(value: float, bottom: int, plot_top: int, low: float, high: float) -> float:
    return bottom - (bottom - plot_top) * math.log(value / low) / math.log(high / low)


# Purpose: Plot exact archive size against median whole-command creation time.
# Inputs: Validated case panels and the tested source commit.
# Outputs: Returns a deterministic, accessible CRLF SVG with observed timing ranges.
def render(commit: str, panels: list[dict]) -> bytes:
    width, height = 1240, 1280
    root = ET.Element(
        f"{{{SVG}}}svg",
        {
            "width": str(width),
            "height": str(height),
            "viewBox": f"0 0 {width} {height}",
            "role": "img",
            "aria-labelledby": "title description",
        },
    )
    ET.SubElement(root, f"{{{SVG}}}title", {"id": "title"}).text = "Archive size versus compression time by effort"
    ET.SubElement(root, f"{{{SVG}}}desc", {"id": "description"}).text = (
        "Silesia Windows application benchmark. ZIP and Zstandard cases are separate. "
        "The vertical time axis is logarithmic; lines connect each tool's explicit effort settings. "
        "Outlined points are measured non-dominated points, not confidence-backed winners. "
        "Rows list exact archive bytes and median seconds with observed min-max ranges."
    )
    ET.SubElement(root, f"{{{SVG}}}rect", {"width": str(width), "height": str(height), "fill": "#ffffff"})
    label(root, 36, 45, "Compression tradeoffs on real files", 24, "bold", "#24343a")
    label(root, 36, 69, "Smaller archive to the left; faster creation lower on each logarithmic time axis", 13)
    label(root, 36, 91, f"Silesia subset  |  source {commit[:12]}  |  10 warm-cache runs per point", 12)
    for index, panel in enumerate(panels):
        top = 125 + index * 375
        points = panel["points"]
        name = f"{panel['format'].upper()} / {panel['name']}"
        label(root, 36, top + 18, name, 18, "bold")
        label(root, 36, top + 39, f"Input {panel['input_bytes']:,} bytes  |  exact archive size; create wall time", 12)
        left, right, bottom, plot_top = 72, 646, top + 304, top + 75
        largest_bytes = max(panel["input_bytes"], *(point["bytes"] for point in points))
        low = 10 ** math.floor(math.log10(min(point["seconds"][1] for point in points) / 1.2))
        high = 10 ** math.ceil(math.log10(max(point["seconds"][2] for point in points) * 1.2))
        if high <= low:
            high = low * 10

        for tick in (0, 0.25, 0.5, 0.75, 1):
            x = x_of(round(largest_bytes * tick), left, right, largest_bytes)
            ET.SubElement(
                root,
                f"{{{SVG}}}line",
                {"x1": f"{x:.1f}", "x2": f"{x:.1f}", "y1": str(plot_top), "y2": str(bottom), "stroke": "#e5e9ea"},
            )
            label(root, round(x) - 12, bottom + 19, f"{largest_bytes * tick / 1048576:.0f}", 11)
        exponent = math.ceil(math.log10(low))
        while 10**exponent <= high:
            tick = 10**exponent
            y = y_of(tick, bottom, plot_top, low, high)
            ET.SubElement(
                root,
                f"{{{SVG}}}line",
                {"x1": str(left), "x2": str(right), "y1": f"{y:.1f}", "y2": f"{y:.1f}", "stroke": "#e5e9ea"},
            )
            label(root, 18, round(y) + 4, f"{tick:g}s", 11)
            exponent += 1
        ET.SubElement(
            root,
            f"{{{SVG}}}rect",
            {
                "x": str(left),
                "y": str(plot_top),
                "width": str(right - left),
                "height": str(bottom - plot_top),
                "fill": "none",
                "stroke": "#90a2a6",
            },
        )
        label(root, 265, bottom + 38, "Archive size (MiB)", 12)
        winners = frontier(points)
        for tool in dict.fromkeys(point["tool"] for point in points):
            series = [point for point in points if point["tool"] == tool]
            coords = " ".join(
                f"{x_of(point['bytes'], left, right, largest_bytes):.1f},"
                f"{y_of(point['seconds'][0], bottom, plot_top, low, high):.1f}"
                for point in series
            )
            ET.SubElement(
                root,
                f"{{{SVG}}}polyline",
                {
                    "points": coords,
                    "fill": "none",
                    "stroke": COLORS[tool],
                    "stroke-width": "2",
                    "stroke-opacity": "0.55",
                },
            )
        for point in points:
            x = x_of(point["bytes"], left, right, largest_bytes)
            median_time = point["seconds"][0]
            y = y_of(median_time, bottom, plot_top, low, high)
            point_description = f"{point['tool']} L{point['level']}: {point['bytes']} bytes, {median_time:.3f} s"
            group = ET.SubElement(
                root,
                f"{{{SVG}}}g",
                {"aria-label": point_description},
            )
            ET.SubElement(group, f"{{{SVG}}}title").text = group.attrib["aria-label"]
            ET.SubElement(
                group,
                f"{{{SVG}}}line",
                {
                    "x1": f"{x:.1f}",
                    "x2": f"{x:.1f}",
                    "y1": f"{y_of(point['seconds'][1], bottom, plot_top, low, high):.1f}",
                    "y2": f"{y_of(point['seconds'][2], bottom, plot_top, low, high):.1f}",
                    "stroke": COLORS[point["tool"]],
                    "stroke-width": "2",
                },
            )
            ET.SubElement(
                group,
                f"{{{SVG}}}circle",
                {
                    "cx": f"{x:.1f}",
                    "cy": f"{y:.1f}",
                    "r": "6",
                    "fill": COLORS[point["tool"]],
                    "stroke": "#24343a" if (point["tool"], point["level"]) in winners else "#ffffff",
                    "stroke-width": "2",
                },
            )
        label(root, 683, top + 67, "Tool / effort", 12, "bold")
        label(root, 854, top + 67, "Archive bytes", 12, "bold")
        label(root, 998, top + 67, "Create s [min-max]", 12, "bold")
        ordered = sorted(points, key=lambda point: (point["tool"] != "SuperZip", point["level"]))
        for row_index, point in enumerate(ordered):
            row_y = top + 92 + row_index * 23
            ET.SubElement(
                root, f"{{{SVG}}}circle", {"cx": "693", "cy": str(row_y - 4), "r": "5", "fill": COLORS[point["tool"]]}
            )
            label(root, 708, row_y, f"{point['tool']} L{point['level']}", 12)
            label(root, 854, row_y, f"{point['bytes']:,}", 12)
            median, minimum, maximum = point["seconds"]
            label(root, 998, row_y, f"{median:.3f} [{minimum:.3f}-{maximum:.3f}]", 12)
        label(root, 684, top + 341, "Dark outline: measured non-dominated point within this case", 11)
    label(
        root, 36, height - 29, "Observed ranges are not confidence intervals; format/corpus panels are not pooled.", 12
    )
    return (ET.tostring(root, encoding="utf-8", xml_declaration=True) + b"\n").replace(b"\n", b"\r\n")


# Purpose: Render only complete reviewed inputs to a chosen output file.
# Inputs: Five JSON paths and one output SVG path.
# Outputs: Writes deterministic SVG or returns a nonzero status with a concise error.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", action="append", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    records = [json.loads(path.read_text(encoding="utf-8")) for path in args.input]
    commit, panels = collect(records)
    args.output.write_bytes(render(commit, panels))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"tradeoff graph failed: {error}", file=sys.stderr)
        sys.exit(1)
