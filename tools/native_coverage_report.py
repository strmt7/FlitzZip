"""Validate and normalize measured Cobertura paths without changing line-hit evidence."""

from __future__ import annotations

import re
import xml.etree.ElementTree as ET
from pathlib import Path


# Purpose: Locate a reported source using Cobertura's sources roots and reject foreign or ambiguous files.
# Inputs: Class filename, declared roots and checkout. Outputs: Existing first-party repository-relative source path.
def source_path(filename: str, sources: list[str], root: Path) -> str:
    root = root.resolve(strict=True)
    name = Path(filename.replace("\\", "/"))
    candidates = [name] if name.is_absolute() else [Path(base.replace("\\", "/")) / name for base in sources]
    resolved = set()
    for candidate in candidates:
        if not candidate.is_absolute():
            candidate = root / candidate
        candidate = candidate.resolve()
        if candidate.is_relative_to(root / "src") and candidate.is_file():
            resolved.add(candidate.relative_to(root).as_posix())
    if len(resolved) != 1:
        raise ValueError("Coverage source is foreign, missing or ambiguous")
    return resolved.pop()


# Purpose: Normalize portable report paths while preserving every measured class, line and hit.
# Inputs: Collector XML, output path and checkout. Outputs: Cobertura and independently counted unique line coverage.
def normalize_report(source: Path, destination: Path, root: Path) -> dict:
    with source.open("rb") as stream:
        raw = stream.read(32 * 1024 * 1024 + 1)
    if len(raw) > 32 * 1024 * 1024 or b"<!ENTITY" in raw.upper():
        raise ValueError("Coverage XML exceeds the report contract")
    report = ET.fromstring(raw)
    if report.tag != "coverage":
        raise ValueError("Expected a Cobertura coverage report")
    sources = [item.text for item in report.findall("./sources/source") if item.text] or [str(root)]
    measured: dict[tuple[str, int], bool] = {}
    source_lines: dict[str, int] = {}
    classes = report.findall("./packages/package/classes/class")
    if not classes:
        raise ValueError("Coverage contains no measured native classes")
    for item in classes:
        relative = source_path(item.attrib.get("filename", ""), sources, root)
        if relative not in source_lines:
            with (root / relative).open("rb") as stream:
                source_lines[relative] = sum(1 for _ in stream)
        item.set("filename", relative)
        lines = item.findall("./lines/line")
        if not lines:
            raise ValueError("Coverage class contains no executable line evidence")
        for line in lines:
            number, hits = line.attrib.get("number", ""), line.attrib.get("hits", "")
            if not re.fullmatch(r"[1-9][0-9]*", number) or not re.fullmatch(r"[0-9]+", hits):
                raise ValueError("Coverage line numbers and hits must be nonnegative integers")
            if int(number) > source_lines[relative]:
                raise ValueError("Coverage line exceeds its current source file")
            identity = (relative, int(number))
            measured[identity] = measured.get(identity, False) or int(hits) > 0
    covered = sum(measured.values())
    if not covered:
        raise ValueError("Coverage recorded no executed source lines")
    roots = report.find("sources")
    if roots is None:
        roots = ET.SubElement(report, "sources")
    roots.clear()
    ET.SubElement(roots, "source").text = "."
    ET.ElementTree(report).write(destination, encoding="utf-8", xml_declaration=True)
    return {"files": sorted({path for path, _ in measured}), "lines_valid": len(measured), "lines_covered": covered}
