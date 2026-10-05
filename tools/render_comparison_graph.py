"""Validate a reviewed application comparison and render a deterministic SVG."""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from tools.render_svg_graph import SVG, new_document
from tools.run_archive_comparison import CASES, EXPECTED, command_templates

HEX40 = re.compile(r"[0-9a-f]{40}\Z")
HEX64 = re.compile(r"[0-9a-f]{64}\Z")


# Purpose: Require a finite, positive, five-run timing distribution.
# Inputs: Raw sample list and its field name for a precise failure.
# Outputs: Returns median/min/max seconds or raises ValueError.
def timing(samples: object, label: str) -> tuple[float, float, float]:
    if not isinstance(samples, list) or len(samples) < 5 or len(samples) > 10:
        raise ValueError(f"{label} requires 5-10 individual runs")
    if any(type(value) not in (int, float) or not math.isfinite(value) or value <= 0 for value in samples):
        raise ValueError(f"{label} contains a nonpositive or nonfinite time")
    return statistics.median(samples), min(samples), max(samples)


# Purpose: Compare portable command placeholders without weakening exact CLI flag checks.
# Inputs: Recorded command vector and expected command vector for one supported tool.
# Outputs: True only for exact flags and equivalent slash direction in placeholder paths.
def matching_command(recorded: object, expected: list[str]) -> bool:
    if not isinstance(recorded, list) or len(recorded) != len(expected):
        return False
    for actual, wanted in zip(recorded, expected, strict=True):
        if not isinstance(actual, str):
            return False
        if "{output}" in wanted or "{archive}" in wanted:
            if actual.replace("\\", "/") != wanted.replace("\\", "/"):
                return False
        elif actual != wanted:
            return False
    return True


# Purpose: Reject unreviewed, incomplete, or scientifically incomparable records.
# Inputs: Parsed comparison record, expected effort, immutable source, and full Silesia manifest.
# Outputs: Returns ordered case rows with medians/ranges and exact archive bytes.
def summarize(record: dict, expected_level: int = 5) -> tuple[str, list[dict]]:
    if type(expected_level) is not int or not 1 <= expected_level <= 9:
        raise ValueError("unsupported comparison effort")
    if record.get("schema_version") != 1 or record.get("benchmark_kind") != "archive_application_comparison":
        raise ValueError("unsupported comparison schema")
    if (
        record.get("source_dirty") is not False
        or not isinstance(record.get("source_commit"), str)
        or not HEX40.fullmatch(record["source_commit"])
    ):
        raise ValueError("comparison requires a clean, full source identity")
    if not isinstance(record.get("superzip_binary_sha256"), str) or not HEX64.fullmatch(
        record["superzip_binary_sha256"]
    ):
        raise ValueError("missing tested SuperZip binary SHA-256")
    build = record.get("build")
    if (
        not isinstance(build, dict)
        or build.get("configuration") != "Release"
        or build.get("platform") != "x64"
        or build.get("hip_enabled") is not True
        or not build.get("generator")
        or not build.get("hip_arch")
        or not isinstance(build.get("cmake_cache_sha256"), str)
        or not HEX64.fullmatch(build["cmake_cache_sha256"])
    ):
        raise ValueError("tested Release/HIP build identity is incomplete")
    if record.get("methodology") != "docs/comparative-benchmark-methodology.md":
        raise ValueError("comparison methodology path differs")
    host = record.get("host")
    if not isinstance(host, dict) or any(
        not host.get(key)
        for key in (
            "os",
            "os_name",
            "os_build",
            "cpu",
            "cpu_cores",
            "cpu_threads",
            "gpu",
            "gpu_driver",
            "ram_bytes",
            "ram_modules",
            "ram_configured_mts",
            "storage_model",
            "storage_bus_type",
        )
    ):
        raise ValueError("host identity is incomplete")
    if any(
        type(host[field]) is not int or host[field] <= 0
        for field in ("ram_bytes", "cpu_cores", "cpu_threads", "ram_modules", "ram_configured_mts")
    ):
        raise ValueError("invalid host numeric specification")
    tools = record.get("tools")
    if not isinstance(tools, list) or {tool.get("name") for tool in tools if isinstance(tool, dict)} != {
        "SuperZip",
        "7-Zip",
        "Zstd",
    }:
        raise ValueError("required comparison tools are missing")
    for tool in tools:
        if not isinstance(tool.get("binary_sha256"), str) or not HEX64.fullmatch(tool["binary_sha256"]):
            raise ValueError("tool executable has no SHA-256")
        if (
            not isinstance(tool.get("version"), str)
            or not tool["version"]
            or not str(tool.get("source_url", "")).startswith("https://")
        ):
            raise ValueError("tool version or provenance is missing")
    if next(tool["binary_sha256"] for tool in tools if tool["name"] == "SuperZip") != record["superzip_binary_sha256"]:
        raise ValueError("SuperZip binary identity differs")
    corpus = record.get("corpus")
    if (
        not isinstance(corpus, dict)
        or corpus.get("source_url") != "https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia"
    ):
        raise ValueError("wrong corpus provenance")
    files = corpus.get("files")
    if not isinstance(files, list) or len(files) != len(EXPECTED):
        raise ValueError("incomplete Silesia manifest")
    by_name = {}
    for entry in files:
        if not isinstance(entry, dict) or entry.get("name") in by_name or entry.get("name") not in EXPECTED:
            raise ValueError("duplicate or unknown corpus file")
        name = entry["name"]
        if (entry.get("bytes"), entry.get("sha256")) != EXPECTED[name]:
            raise ValueError(f"corpus size or hash evidence differs: {name}")
        if (
            entry.get("download_url") != f"https://sun.aei.polsl.pl/~sdeor/corpus/{name}.bz2"
            or not isinstance(entry.get("download_sha256"), str)
            or not HEX64.fullmatch(entry["download_sha256"])
        ):
            raise ValueError(f"corpus download provenance is missing: {name}")
        by_name[name] = entry
    if set(by_name) != set(EXPECTED):
        raise ValueError("corpus manifest omits required files")
    settings = record.get("settings")
    if (
        not isinstance(settings, dict)
        or type(settings.get("level")) is not int
        or settings.get("level") != expected_level
        or settings.get("warmups_per_command") != 1
        or settings.get("order") != "alternating AB/BA"
        or settings.get("cache") != "warm"
    ):
        raise ValueError("comparison settings differ from declared method")
    if type(settings.get("runs")) is not int or not 5 <= settings["runs"] <= 10:
        raise ValueError("comparison run count is missing or too small")
    pause = settings.get("round_pause_ms", 0)
    if type(pause) is not int or not 0 <= pause <= 1000:
        raise ValueError("invalid inter-round pause")
    cases = record.get("cases")
    if not isinstance(cases, list) or len(cases) != len(CASES):
        raise ValueError("comparison case set is incomplete")
    rows = []
    for actual, expected in zip(cases, CASES, strict=True):
        name, fmt, file_names, tool_names = expected
        if (actual.get("name"), actual.get("format"), actual.get("files")) != (name, fmt, list(file_names)):
            raise ValueError(f"case identity differs: {name}")
        size = sum(by_name[file]["bytes"] for file in file_names)
        if actual.get("input_bytes") != size or size > 64 * 1024 * 1024:
            raise ValueError("case input size differs or exceeds 64 MiB")
        for field in ("resource_before", "resource_after"):
            sample = actual.get(field)
            if not isinstance(sample, dict) or any(
                sample.get(key) is None
                for key in (
                    "cpu_load_percent",
                    "free_ram_bytes",
                    "paging_pages_per_second",
                    "disk_busy_percent",
                    "gpu_engine_max_percent",
                    "workspace_bus_type",
                )
            ):
                raise ValueError("resource context is incomplete")
        results = actual.get("results")
        if not isinstance(results, list) or [item.get("tool") for item in results] != list(tool_names):
            raise ValueError(f"tool pairing differs: {name}")
        reference_sha = actual.get("extract_reference_sha256")
        reference_bytes = actual.get("extract_reference_bytes")
        if (
            actual.get("extract_reference_tool") != tool_names[1]
            or not isinstance(reference_sha, str)
            or not HEX64.fullmatch(reference_sha)
            or type(reference_bytes) is not int
            or reference_bytes <= 0
        ):
            raise ValueError(f"extraction reference is missing or differs: {name}")
        metrics = []
        for item in results:
            tool = item["tool"]
            create, extract = command_templates(tool, fmt, file_names, expected_level)
            if not matching_command(item.get("create_argv"), create) or not matching_command(
                item.get("extract_argv"), extract
            ):
                raise ValueError("recorded command does not match the reviewed flags")
            if item.get("independent_verified") is not True:
                raise ValueError("independent archive decoding did not pass")
            if item.get("extract_reference_sha256") != reference_sha:
                raise ValueError("extraction did not use the common reference archive")
            compressed = timing(item.get("compress_seconds"), "compression")
            extracted = timing(item.get("extract_seconds"), "extraction")
            if len(item["compress_seconds"]) != settings["runs"] or len(item["extract_seconds"]) != settings["runs"]:
                raise ValueError("case run count differs from declared settings")
            archives = item.get("archive_bytes")
            hashes = item.get("archive_sha256")
            if (
                not isinstance(archives, list)
                or len(archives) != len(item["compress_seconds"])
                or any(type(value) is not int or value <= 0 for value in archives)
                or len(set(archives)) != 1
            ):
                raise ValueError("archive size is missing or varies between identical runs")
            if (
                not isinstance(hashes, list)
                or len(hashes) != len(archives)
                or any(not isinstance(value, str) or not HEX64.fullmatch(value) for value in hashes)
            ):
                raise ValueError("archive SHA-256 evidence is incomplete")
            metrics.append(
                {
                    "tool": tool,
                    "bytes": archives[0],
                    "compression": compressed,
                    "extraction": extracted,
                    "runs": len(archives),
                }
            )
        comparator = results[1]
        if comparator["archive_bytes"][-1] != reference_bytes or comparator["archive_sha256"][-1] != reference_sha:
            raise ValueError("extraction reference differs from the comparator's measured archive")
        rows.append({"name": name, "format": fmt, "input_bytes": size, "metrics": metrics})
    return record["source_commit"], rows


# Purpose: Place accessible SVG text through XML escaping rather than string concatenation.
# Inputs: Parent element, coordinates, literal label, and optional style.
# Outputs: Appends one deterministic text element.
def label(
    parent: ET.Element, x: int, y: int, value: str, size: int = 13, weight: str = "normal", color: str = "#24343a"
) -> None:
    element = ET.SubElement(
        parent,
        f"{{{SVG}}}text",
        {"x": str(x), "y": str(y), "font-size": str(size), "font-weight": weight, "fill": color, "letter-spacing": "0"},
    )
    element.text = value


# Purpose: Draw one zero-origin measured bar with a min/max timing whisker.
# Inputs: Panel x/y, scaled value/range, exact display text, and tool color.
# Outputs: Adds a bar, range ticks where applicable, and external numeric label.
def bar(
    parent: ET.Element, x: int, y: int, value: float, low: float, high: float, maximum: float, display: str, color: str
) -> None:
    span = 205
    width = max(2, round(span * value / maximum))
    ET.SubElement(
        parent,
        f"{{{SVG}}}rect",
        {"x": str(x), "y": str(y - 13), "width": str(width), "height": "18", "rx": "2", "fill": color},
    )
    if low != high:
        left = x + round(span * low / maximum)
        right = x + round(span * high / maximum)
        ET.SubElement(
            parent,
            f"{{{SVG}}}line",
            {
                "x1": str(left),
                "x2": str(right),
                "y1": str(y - 17),
                "y2": str(y - 17),
                "stroke": "#17252b",
                "stroke-width": "2",
            },
        )
        for edge in (left, right):
            ET.SubElement(
                parent,
                f"{{{SVG}}}line",
                {
                    "x1": str(edge),
                    "x2": str(edge),
                    "y1": str(y - 20),
                    "y2": str(y - 14),
                    "stroke": "#17252b",
                    "stroke-width": "2",
                },
            )
    label(parent, x + span + 10, y + 1, display, 12)


# Purpose: Render exact size plus both operation times without hiding unfavorable results.
# Inputs: Validated ordered rows and the immutable source SHA.
# Outputs: Returns repeatable, accessible UTF-8 SVG bytes.
def render(commit: str, rows: list[dict]) -> bytes:
    height = 176 + 145 * len(rows)
    root = new_document(
        {
            "viewBox": f"0 0 1600 {height}",
            "width": "1600",
            "height": str(height),
            "role": "img",
            "aria-labelledby": "title desc",
            "font-family": "Segoe UI, Arial, sans-serif",
        },
        "SuperZip application comparison on Silesia files",
        "Same-host, same-format ZIP and Zstandard comparisons. Exact archive sizes and median full CLI "
        "compression times. Both extraction times in a case use the same comparator-created archive. "
        "Time whiskers show observed minimum and maximum. Lower is better.",
    )
    label(root, 28, 42, "Archive application comparison", 25, "bold", "#17252b")
    minimum_runs = min(metric["runs"] for row in rows for metric in row["metrics"])
    label(
        root,
        28,
        68,
        f"Silesia subset | same host and format | {minimum_runs} warm-cache runs per direction",
        14,
        color="#53656d",
    )
    for x, title in ((310, "Archive size"), (730, "Compression"), (1150, "Extraction")):
        label(root, x, 109, title, 16, "bold")
        label(root, x, 128, "Lower is better", 12, color="#53656d")
    max_size = max(metric["bytes"] for row in rows for metric in row["metrics"])
    max_compress = max(metric["compression"][2] for row in rows for metric in row["metrics"])
    max_extract = max(metric["extraction"][2] for row in rows for metric in row["metrics"])
    for index, row in enumerate(rows):
        top = 154 + 145 * index
        ET.SubElement(
            root,
            f"{{{SVG}}}line",
            {"x1": "28", "x2": "1570", "y1": str(top - 10), "y2": str(top - 10), "stroke": "#dce5e7"},
        )
        label(
            root,
            28,
            top + 14,
            f"{row['format'].upper()} | {row['name']} | {row['input_bytes']:,} input bytes",
            14,
            "bold",
        )
        for lane, metric in enumerate(row["metrics"]):
            y = top + 50 + lane * 37
            color = "#087f8c" if metric["tool"] == "SuperZip" else "#46545b"
            label(root, 38, y + 1, metric["tool"], 13)
            bar(
                root,
                310,
                y,
                metric["bytes"],
                metric["bytes"],
                metric["bytes"],
                max_size,
                f"{metric['bytes']:,} B ({metric['bytes'] / row['input_bytes']:.1%})",
                color,
            )
            for x, key, maximum in ((730, "compression", max_compress), (1150, "extraction", max_extract)):
                median, minimum, maximum_run = metric[key]
                bar(
                    root,
                    x,
                    y,
                    median,
                    minimum,
                    maximum_run,
                    maximum,
                    f"{median:.3f} s [{minimum:.3f}-{maximum_run:.3f}]",
                    color,
                )
    label(
        root,
        28,
        height - 26,
        f"Common extraction archive per case; ranges are observed, not confidence intervals | {commit[:12]}",
        12,
        color="#53656d",
    )
    return (ET.tostring(root, encoding="utf-8", xml_declaration=True) + b"\n").replace(b"\n", b"\r\n")


# Purpose: Generate or byte-check the reviewed comparison chart.
# Inputs: JSON path, SVG path, explicit effort 1-9, and optional non-writing check mode.
# Outputs: Returns zero only when validation and generation/check succeed.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--level", type=int, choices=range(1, 10), default=5)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    record = json.loads(args.input.read_text(encoding="utf-8"))
    commit, rows = summarize(record, expected_level=args.level)
    image = render(commit, rows)
    if args.check:
        if not args.output.is_file() or args.output.read_bytes() != image:
            raise ValueError("committed comparison graph does not match reviewed record")
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(image)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"comparison graph failed: {error}", file=sys.stderr)
        sys.exit(1)
