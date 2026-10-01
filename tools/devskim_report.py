"""Publish DevSkim SARIF without empty excerpt objects or lost findings."""

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
from pathlib import Path

from tools.devskim_provenance import unique_fields

MAX_REPORT_BYTES = 64 * 1024 * 1024
MAX_NODES = 1_000_000
MAX_DEPTH = 128


# Purpose: Reject source excerpts and unknown snippet fields at the publication boundary.
# Inputs: Optional region snippet produced while DevSkim excerpt collection is disabled.
# Outputs: None for empty known fields; unexpected content raises a value-free ValueError.
def require_empty_snippet(snippet: object) -> None:
    if not isinstance(snippet, dict) or set(snippet) - {"text", "rendered"}:
        raise ValueError("Unexpected DevSkim snippet shape")
    if snippet.get("text", "") != "":
        raise ValueError("Unexpected DevSkim source excerpt")
    if "rendered" in snippet:
        rendered = snippet["rendered"]
        if not isinstance(rendered, dict) or set(rendered) - {"text", "markdown"}:
            raise ValueError("Unexpected DevSkim rendered snippet shape")
        if rendered.get("text", "") != "" or rendered.get("markdown", "") not in ("", "``"):
            raise ValueError("Unexpected DevSkim rendered source excerpt")


# Purpose: Remove only empty optional physical-region excerpts from the pinned producer.
# Inputs: Decoded SARIF report; caller owns the object; no findings or extension bags are filtered.
# Outputs: Mutates empty snippet fields only and returns their count; malformed input fails closed.
def prepare_report(report: object) -> int:
    if not isinstance(report, dict) or report.get("version") != "2.1.0":
        raise ValueError("Unsupported DevSkim SARIF version")
    runs = report.get("runs")
    if not isinstance(runs, list) or not 1 <= len(runs) <= 16:
        raise ValueError("Invalid DevSkim SARIF runs")
    for run in runs:
        tool = run.get("tool") if isinstance(run, dict) else None
        driver = tool.get("driver") if isinstance(tool, dict) else None
        if not isinstance(driver, dict) or driver.get("name") != "devskim":
            raise ValueError("Unexpected SARIF producer")
        if not isinstance(run.get("results", []), list):
            raise ValueError("Invalid DevSkim SARIF results")
    stack = [(report, 0, 0)]
    visited = removed = 0
    while stack:
        node, depth, location_state = stack.pop()
        visited += 1
        if visited > MAX_NODES or len(stack) > MAX_NODES or depth > MAX_DEPTH:
            raise ValueError("DevSkim SARIF exceeds its structure budget")
        if isinstance(node, dict):
            if location_state == 2 and "snippet" in node:
                require_empty_snippet(node["snippet"])
                del node["snippet"]
                removed += 1
            for key, child in node.items():
                if key != "properties" and isinstance(child, (dict, list)):
                    child_state = 1 if key == "physicalLocation" else 0
                    if location_state == 1 and key in ("region", "contextRegion"):
                        child_state = 2
                    stack.append((child, depth + 1, child_state))
        elif isinstance(node, list):
            stack.extend((child, depth + 1, 0) for child in node if isinstance(child, (dict, list)))
    return removed


# Purpose: Reject non-JSON numeric constants before they enter a published report.
# Inputs: Decoder constant spelling; the value is never echoed to logs.
# Outputs: Always raises ValueError.
def reject_constant(_value: str) -> None:
    raise ValueError("Invalid DevSkim JSON numeric constant")


# Purpose: Publish validated bytes atomically without overwriting an existing report.
# Inputs: Bounded raw scanner file and trusted output path on the local filesystem.
# Outputs: Finding/snippet counts; all pre-publication failures leave the output absent.
def publish_report(source: Path, destination: Path) -> tuple[int, int]:
    with source.open("rb") as stream:
        payload = stream.read(MAX_REPORT_BYTES + 1)
    if len(payload) > MAX_REPORT_BYTES:
        raise ValueError("DevSkim SARIF exceeds its byte budget")
    report = json.loads(payload, object_pairs_hook=unique_fields, parse_constant=reject_constant)
    removed = prepare_report(report)
    published = json.dumps(report, ensure_ascii=True, allow_nan=False, separators=(",", ":")).encode("utf-8")
    if len(published) > MAX_REPORT_BYTES:
        raise ValueError("Published DevSkim SARIF exceeds its byte budget")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=destination.parent, prefix=".devskim-report-", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(published)
        # Linking the completed sibling preserves both atomic visibility and overwrite refusal.
        os.link(temporary, destination)
    finally:
        if temporary is not None:
            temporary.unlink()
    return sum(len(run.get("results", [])) for run in report["runs"]), removed


# Purpose: Supply CI's lossless publication boundary without logging raw report content.
# Inputs: Raw scanner report and destination CLI paths; GitHub validates the complete schema next.
# Outputs: Metadata-only status and exit zero; validation/I/O failures return two.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    try:
        findings, removed = publish_report(args.source, args.destination)
    except (ValueError, TypeError, KeyError, AttributeError, OSError, RecursionError):
        print("DevSkim SARIF publication failed; raw report content was not logged.", file=sys.stderr)
        return 2
    print(f"DevSkim SARIF published: {findings} findings preserved; {removed} empty excerpts removed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
