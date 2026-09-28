"""Render reviewed SuperZip RAM benchmark records as a deterministic SVG chart."""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path

SVG = "http://www.w3.org/2000/svg"
ET.register_namespace("", SVG)


# Purpose: Reject malformed or unreviewed benchmark data before charting it.
# Inputs: A parsed schema-one record and a local-preview switch.
# Outputs: Returns normalized run groups or raises ValueError with the invalid field.
def validate_record(record: dict, allow_dirty: bool) -> tuple[tuple, dict]:
    if record.get("schema_version") != 1 or record.get("benchmark_kind") != "suzip_ram":
        raise ValueError("unsupported RAM benchmark schema or kind")
    if type(record.get("source_dirty")) is not bool or (record["source_dirty"] and not allow_dirty):
        raise ValueError("benchmark source must be clean for publication")
    commit = record.get("source_commit")
    binary_hash = record.get("binary_sha256")
    if not isinstance(commit, str) or not re.fullmatch(r"[0-9a-fA-F]{40}", commit):
        raise ValueError("source_commit must be a complete Git SHA")
    if not isinstance(binary_hash, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", binary_hash):
        raise ValueError("binary_sha256 must be a SHA-256 digest")
    profile = record.get("profile")
    if not isinstance(profile, str) or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]*", profile):
        raise ValueError("invalid workload profile")
    size_mib = record.get("size_mib")
    level = record.get("compression_level")
    if type(size_mib) is not int or size_mib < 10240 or type(level) is not int or level not in range(1, 10):
        raise ValueError("invalid workload size or effort")
    identity = (
        commit.lower(),
        binary_hash.lower(),
        record.get("cpu_model"),
        record.get("gpu_model"),
        record["source_dirty"],
    )
    groups = defaultdict(lambda: defaultdict(dict))
    runs = record.get("runs")
    if not isinstance(runs, list) or not runs:
        raise ValueError("benchmark record has no runs")
    for run in runs:
        if not isinstance(run, dict) or run.get("lane") not in ("CPU", "GPU"):
            raise ValueError("invalid benchmark lane")
        if run.get("memory_only") is not True or run.get("disk_write_bytes") != 0:
            raise ValueError("benchmark run is not RAM-only")
        block_kib = run.get("block_size_kib")
        iteration = run.get("iteration")
        if type(block_kib) is not int or block_kib not in (256, 512, 1024, 2048, 4096, 8192, 16384):
            raise ValueError("invalid production block size")
        if type(iteration) is not int or iteration < 1:
            raise ValueError("invalid iteration")
        if type(run.get("input_bytes")) is not int or run["input_bytes"] != size_mib * 1024 * 1024:
            raise ValueError("run input size differs from workload")
        output_bytes = run.get("output_bytes")
        if type(output_bytes) is not int or output_bytes <= 0:
            raise ValueError("output size must be an exact positive integer")
        phases = [run.get(name) for name in ("compress_seconds", "verify_seconds", "extract_seconds")]
        if any(type(value) not in (int, float) or not math.isfinite(value) or value < 0 for value in phases):
            raise ValueError("invalid phase timing")
        seconds = sum(phases)
        if seconds <= 0:
            raise ValueError("run has no positive elapsed time")
        if run["lane"] == "GPU" and (
            type(run.get("gpu_kernel_launches")) is not int or run["gpu_kernel_launches"] <= 0
        ):
            raise ValueError("GPU lane lacks HIP launch evidence")
        key = (profile, size_mib, level, block_kib)
        lane_runs = groups[key][run["lane"]]
        if iteration in lane_runs:
            raise ValueError("duplicate lane iteration")
        lane_runs[iteration] = (output_bytes, seconds)
    return identity, groups


# Purpose: Compute comparable medians while rejecting missing lanes and unstable encoded sizes.
# Inputs: Validated records from the same binary and host, with at least three paired runs per case.
# Outputs: Returns sorted chart rows with exact bytes and median end-to-end throughput.
def summarize_records(records: list[dict], allow_dirty: bool) -> tuple[tuple, list[dict]]:
    identity = None
    groups = defaultdict(lambda: defaultdict(dict))
    for record in records:
        current_identity, current_groups = validate_record(record, allow_dirty)
        if identity is None:
            identity = current_identity
        elif identity != current_identity:
            raise ValueError("benchmark records use different binaries or hardware")
        for key, lanes in current_groups.items():
            for lane, iterations in lanes.items():
                if set(groups[key][lane]).intersection(iterations):
                    raise ValueError("duplicate run across benchmark records")
                groups[key][lane].update(iterations)
    if not groups:
        raise ValueError("no benchmark cases")
    rows = []
    for key, lanes in sorted(groups.items()):
        if set(lanes) != {"CPU", "GPU"} or set(lanes["CPU"]) != set(lanes["GPU"]):
            raise ValueError("every chart case needs paired CPU and HIP iterations")
        if len(lanes["CPU"]) < 3:
            raise ValueError("publishable charts need at least three paired iterations")
        metrics = {}
        for lane, iterations in lanes.items():
            sizes = {size for size, _ in iterations.values()}
            if len(sizes) != 1:
                raise ValueError("encoded size changed between identical lane runs")
            elapsed = statistics.median(seconds for _, seconds in iterations.values())
            metrics[lane] = {
                "output_bytes": sizes.pop(),
                "throughput_gib_s": (key[1] / 1024) / elapsed,
            }
        rows.append(
            {
                "profile": key[0],
                "size_mib": key[1],
                "level": key[2],
                "block_kib": key[3],
                "iterations": len(lanes["CPU"]),
                "metrics": metrics,
            }
        )
    return identity, rows


# Purpose: Add one accessible SVG text label without manual XML escaping.
# Inputs: Parent element, placement, label, style, and optional text anchor.
# Outputs: Appends a namespaced SVG text node.
def add_text(
    parent: ET.Element,
    x: int,
    y: int,
    label: str,
    size: int = 14,
    weight: str = "normal",
    fill: str = "#263238",
    anchor: str = "start",
) -> None:
    node = ET.SubElement(
        parent,
        f"{{{SVG}}}text",
        {
            "x": str(x),
            "y": str(y),
            "font-size": str(size),
            "font-weight": weight,
            "fill": fill,
            "text-anchor": anchor,
            "letter-spacing": "0",
        },
    )
    node.text = label


# Purpose: Render one lane bar with an exact-byte tooltip and a fixed external value column.
# Inputs: Row placement, lane metric, normalization maxima, and selected size/throughput panel.
# Outputs: Adds a bar and its legible numeric label without changing chart geometry.
def add_lane_bar(parent: ET.Element, y: int, x: int, metric: dict, maximum: float, kind: str, color: str) -> None:
    value = metric["output_bytes"] / (1024 * 1024) if kind == "size" else metric["throughput_gib_s"]
    bar = ET.SubElement(
        parent,
        f"{{{SVG}}}rect",
        {
            "x": str(x),
            "y": str(y - 13),
            "width": str(max(2, round(260 * value / maximum))),
            "height": "17",
            "rx": "2",
            "fill": color,
        },
    )
    title = ET.SubElement(bar, f"{{{SVG}}}title")
    title.text = (
        f"{metric['output_bytes']:,} encoded bytes"
        if kind == "size"
        else f"{metric['throughput_gib_s']:.3f} GiB/s, encode + verify + decode"
    )
    label = f"{value:,.1f} MiB" if kind == "size" else f"{value:,.2f} GiB/s"
    add_text(parent, x + 275, y + 1, label, 13, fill="#263238")


# Purpose: Build a reviewable two-panel graphic from exact size and paired median throughput data.
# Inputs: One source/binary identity and validated chart rows.
# Outputs: Returns deterministic UTF-8 SVG bytes without network access or third-party graphics packages.
def render_svg(identity: tuple, rows: list[dict]) -> bytes:
    if len(rows) > 16:
        raise ValueError("README chart is limited to 16 explicit cases; split larger studies")
    height = 182 + len(rows) * 92
    root = ET.Element(
        f"{{{SVG}}}svg",
        {
            "viewBox": f"0 0 1200 {height}",
            "width": "1200",
            "height": str(height),
            "font-family": "Segoe UI, Arial, sans-serif",
            "role": "img",
            "aria-labelledby": "title desc",
        },
    )
    ET.SubElement(root, f"{{{SVG}}}title", {"id": "title"}).text = "SuperZip RAM-only benchmark"
    ET.SubElement(root, f"{{{SVG}}}desc", {"id": "desc"}).text = (
        "Paired forced-CPU and required-AMD-HIP SUZIP results. Left: exact archive size, lower is better. "
        "Right: median encode, verify, and decode throughput, higher is better. Synthetic workloads only."
    )
    ET.SubElement(root, f"{{{SVG}}}rect", {"width": "1200", "height": str(height), "fill": "#ffffff"})
    add_text(root, 32, 42, "SuperZip | measured native-format performance", 25, "bold", "#17252b")
    subtitle = "RAM-only synthetic workloads. Paired runs; no archive disk writes."
    if identity[4]:
        subtitle += " DIRTY SOURCE - LOCAL PREVIEW ONLY."
    add_text(root, 32, 70, subtitle, 14, fill="#53656d")
    add_text(root, 318, 116, "Encoded size", 16, "bold")
    add_text(root, 318, 136, "Lower is better", 12, fill="#53656d")
    add_text(root, 786, 116, "End-to-end throughput", 16, "bold")
    add_text(root, 786, 136, "Higher is better", 12, fill="#53656d")
    max_size = max(metric["output_bytes"] / (1024 * 1024) for row in rows for metric in row["metrics"].values())
    max_speed = max(metric["throughput_gib_s"] for row in rows for metric in row["metrics"].values())
    for index, row in enumerate(rows):
        top = 158 + index * 92
        ET.SubElement(
            root,
            f"{{{SVG}}}line",
            {"x1": "32", "x2": "1168", "y1": str(top - 7), "y2": str(top - 7), "stroke": "#dce5e7"},
        )
        size_label = f"{row['size_mib'] / 1024:g} GiB"
        add_text(
            root,
            32,
            top + 10,
            f"{row['profile']} | {size_label} | level {row['level']} | {row['block_kib']} KiB blocks",
            14,
            "bold",
        )
        for lane, offset, color in (("CPU", 38, "#394b53"), ("GPU", 66, "#087f8c")):
            y = top + offset
            add_text(root, 50, y + 1, "Required HIP" if lane == "GPU" else "Forced CPU", 13)
            add_lane_bar(root, y, 318, row["metrics"][lane], max_size, "size", color)
            add_lane_bar(root, y, 786, row["metrics"][lane], max_speed, "speed", color)
    footer_y = height - 20
    minimum_runs = min(row["iterations"] for row in rows)
    add_text(
        root,
        32,
        footer_y,
        f"Median of at least {minimum_runs} paired runs | "
        f"commit {identity[0][:12]} | binary SHA-256 {identity[1][:12]}...",
        12,
        fill="#53656d",
    )
    return (ET.tostring(root, encoding="utf-8", xml_declaration=True) + b"\n").replace(b"\n", b"\r\n")


# Purpose: Validate input records and either create or byte-check the committed chart.
# Inputs: Command-line paths and the local-preview dirty-source policy.
# Outputs: Returns zero only when generation or deterministic comparison succeeds.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", action="append", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--allow-dirty", action="store_true", help="Local preview only; not for publication")
    args = parser.parse_args()
    try:
        records = [json.loads(path.read_text(encoding="utf-8")) for path in args.input]
        identity, rows = summarize_records(records, args.allow_dirty)
        graphic = render_svg(identity, rows)
        if args.check:
            if not args.output.exists() or args.output.read_bytes() != graphic:
                raise ValueError("committed benchmark graph differs from reviewed JSON; regenerate it")
        else:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(graphic)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"benchmark graph: {exc}", file=sys.stderr)
        return 1
    print(f"benchmark graph: {'verified' if args.check else 'created'}; cases={len(rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
