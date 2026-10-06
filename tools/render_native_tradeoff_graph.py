"""Validate a paired RAM-only native effort sweep and render its tradeoff."""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from tools.render_benchmark_graph import SVG, add_text, validate_record
from tools.render_svg_graph import append_range_whisker, new_document

LEVELS = (1, 3, 5, 7, 9)
COLORS = {"CPU": "#b15a22", "GPU": "#087e83"}


# Purpose: Require five comparable, repeatable CPU/GPU RAM-only effort records.
# Inputs: Parsed benchmark records in any order.
# Outputs: Returns immutable source identity and ten exact-size, timed lane points.
def collect(records: list[dict]) -> tuple[tuple, list[dict]]:
    if len(records) != len(LEVELS):
        raise ValueError("native effort chart requires exactly five records")
    levels: dict[int, dict] = {}
    identity = None
    for record in records:
        current_identity, _ = validate_record(record, allow_dirty=False)
        level = record.get("compression_level")
        if level not in LEVELS or type(level) is not int or level in levels:
            raise ValueError("duplicate or unsupported native effort")
        if identity is None:
            identity = current_identity
        elif current_identity != identity:
            raise ValueError("native effort records use different source, binary, or hardware")
        if record.get("profile") != "Mixed" or record.get("size_mib") != 10240:
            raise ValueError("native effort chart requires the 10 GiB Mixed workload")
        levels[level] = record

    points = []
    for level in LEVELS:
        record = levels[level]
        runs = record["runs"]
        if len(runs) != 6:
            raise ValueError("each effort needs three paired CPU/GPU runs")
        for lane in ("CPU", "GPU"):
            selected = sorted((run for run in runs if run["lane"] == lane), key=lambda run: run["iteration"])
            if [run["iteration"] for run in selected] != [1, 2, 3]:
                raise ValueError("native effort iterations are incomplete")
            if any(run["block_size_kib"] != 16384 for run in selected):
                raise ValueError("native effort block size changed")
            sizes = {run.get("archive_bytes") for run in selected}
            if len(sizes) != 1 or any(type(size) is not int or size <= 0 for size in sizes):
                raise ValueError("native archive size is missing or unstable")
            if lane == "GPU" and any(run["gpu_kernel_launches"] <= 0 for run in selected):
                raise ValueError("GPU lane lacks HIP kernel evidence")
            times = [run["compress_seconds"] for run in selected]
            if any(time <= 0 for time in times):
                raise ValueError("native compression time must be positive")
            points.append(
                {
                    "level": level,
                    "lane": lane,
                    "bytes": sizes.pop(),
                    "seconds": (statistics.median(times), min(times), max(times)),
                    "runs": times,
                }
            )
    return identity, points


# Purpose: Draw measured archive bytes against median compression time without hiding the narrow size range.
# Inputs: Complete validated points and their immutable source identity.
# Outputs: Returns an accessible deterministic CRLF SVG with exact numeric table and observed timing ranges.
def render(identity: tuple, points: list[dict]) -> bytes:
    width, height = 1230, 670
    root = new_document(
        {
            "width": str(width),
            "height": str(height),
            "viewBox": f"0 0 {width} {height}",
            "role": "img",
            "aria-labelledby": "title description",
            "font-family": "Segoe UI, Arial, sans-serif",
        },
        "Native archive size versus compression time by effort",
        "Ten GiB RAM-only Mixed workload with three paired runs per level. "
        "The archive-size axis is truncated and explicitly labeled; table values are exact bytes. "
        "Timing ranges are observed minima and maxima, not confidence intervals.",
    )
    add_text(root, 35, 44, "Native CPU and GPU effort tradeoffs", 24, "bold")
    add_text(root, 35, 70, "10 GiB Mixed data in RAM  |  16 MiB blocks  |  three paired runs per level", 13)
    add_text(root, 35, 91, f"Source {identity[0][:12]}  |  smaller archive left, faster compression lower", 12)

    left, right, top, bottom = 84, 655, 130, 542
    sizes_mib = [point["bytes"] / 1048576 for point in points]
    low = math.floor((min(sizes_mib) - 10) / 25) * 25
    high = math.ceil((max(sizes_mib) + 10) / 25) * 25
    time_high = math.ceil(max(point["seconds"][2] for point in points) + 1)

    def x_pos(point: dict) -> float:
        return left + (right - left) * (point["bytes"] / 1048576 - low) / (high - low)

    def y_pos(seconds: float) -> float:
        return bottom - (bottom - top) * seconds / time_high

    for tick in range(low, high + 1, 25):
        x = left + (right - left) * (tick - low) / (high - low)
        ET.SubElement(
            root,
            f"{{{SVG}}}line",
            {"x1": f"{x:.1f}", "x2": f"{x:.1f}", "y1": str(top), "y2": str(bottom), "stroke": "#e2e8e9"},
        )
        add_text(root, round(x), bottom + 22, str(tick), 11, anchor="middle")
    for tick in range(0, time_high + 1, 2):
        y = y_pos(tick)
        ET.SubElement(
            root,
            f"{{{SVG}}}line",
            {"x1": str(left), "x2": str(right), "y1": f"{y:.1f}", "y2": f"{y:.1f}", "stroke": "#e2e8e9"},
        )
        add_text(root, left - 10, round(y) + 4, str(tick), 11, anchor="end")
    ET.SubElement(
        root,
        f"{{{SVG}}}rect",
        {
            "x": str(left),
            "y": str(top),
            "width": str(right - left),
            "height": str(bottom - top),
            "fill": "none",
            "stroke": "#91a3a7",
        },
    )
    add_text(root, 370, bottom + 48, "Archive size (MiB; truncated axis)", 12, anchor="middle")
    add_text(root, 35, 118, "Compression seconds", 12)

    for lane in ("CPU", "GPU"):
        series = [point for point in points if point["lane"] == lane]
        ET.SubElement(
            root,
            f"{{{SVG}}}polyline",
            {
                "points": " ".join(f"{x_pos(point):.1f},{y_pos(point['seconds'][0]):.1f}" for point in series),
                "fill": "none",
                "stroke": COLORS[lane],
                "stroke-width": "2",
            },
        )
        for point in series:
            x, median, minimum, maximum = x_pos(point), *point["seconds"]
            group = ET.SubElement(
                root,
                f"{{{SVG}}}g",
                {"aria-label": f"{lane} level {point['level']}: {point['bytes']} bytes, {median:.3f} seconds"},
            )
            ET.SubElement(group, f"{{{SVG}}}title").text = group.attrib["aria-label"]
            append_range_whisker(group, (x, y_pos(minimum)), (x, y_pos(maximum)), COLORS[lane], axis="y")
            ET.SubElement(
                group,
                f"{{{SVG}}}circle",
                {
                    "cx": f"{x:.1f}",
                    "cy": f"{y_pos(median):.1f}",
                    "r": "5",
                    "fill": COLORS[lane],
                    "stroke": "#ffffff",
                    "stroke-width": "1.5",
                },
            )

    add_text(root, 700, 137, "Lane / effort", 12, "bold")
    add_text(root, 837, 137, "Exact archive bytes", 12, "bold")
    add_text(root, 1013, 137, "Compress s [min-max]", 12, "bold")
    for index, point in enumerate(points):
        y = 166 + index * 35
        ET.SubElement(
            root, f"{{{SVG}}}circle", {"cx": "709", "cy": str(y - 4), "r": "5", "fill": COLORS[point["lane"]]}
        )
        add_text(root, 723, y, f"{point['lane']} L{point['level']}", 12)
        add_text(root, 837, y, f"{point['bytes']:,}", 12)
        median, minimum, maximum = point["seconds"]
        add_text(root, 1013, y, f"{median:.3f} [{minimum:.3f}-{maximum:.3f}]", 12)
    add_text(
        root,
        35,
        630,
        "CPU and GPU use different native codecs; equal numeric levels do not imply equal internal work.",
        12,
    )
    add_text(
        root,
        35,
        651,
        "Observed ranges are not confidence intervals; these data do not predict other files or hardware.",
        12,
    )
    return (ET.tostring(root, encoding="utf-8", xml_declaration=True) + b"\n").replace(b"\n", b"\r\n")


# Purpose: Generate the native effort SVG only from validated reviewed records.
# Inputs: Five JSON input paths and one output path.
# Outputs: Writes deterministic SVG or raises a precise validation error.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", action="append", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    identity, points = collect([json.loads(path.read_text(encoding="utf-8")) for path in args.input])
    args.output.write_bytes(render(identity, points))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"native tradeoff graph failed: {error}", file=sys.stderr)
        sys.exit(1)
