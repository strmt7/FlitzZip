"""Instantiate the shared, passive SVG document used by benchmark renderers."""

from __future__ import annotations

import copy
import math
import xml.etree.ElementTree as ET
from pathlib import Path

TEMPLATE_PATH = Path(__file__).resolve().parents[1] / "resources/benchmarks/chart-template.svg"
MAX_TEMPLATE_BYTES = 1024
MAX_DOCUMENT_BYTES = 1024 * 1024
MAX_DOCUMENT_DEPTH = 32
MAX_DOCUMENT_NODES = 32768
ROOT_ATTRIBUTES = {"viewBox", "width", "height", "font-family", "role", "aria-labelledby"}


# Purpose: Draw an exact observed range with endpoint caps, including a zero-width range.
# Inputs: Finite horizontal or vertical endpoints, positive cap extent, and series color.
# Outputs: Appends three SVG lines without widening the measured interval; rejects invalid geometry.
def append_range_whisker(
    parent: ET.Element,
    start: tuple[float, float],
    end: tuple[float, float],
    color: str,
    cap: float = 4,
    axis: str = "x",
) -> None:
    if not all(math.isfinite(value) for value in (*start, *end, cap)) or cap <= 0:
        raise ValueError("range whisker geometry must be finite with positive caps")
    if axis not in ("x", "y") or (start[1] != end[1] if axis == "x" else start[0] != end[0]):
        raise ValueError("range whisker must follow one plot axis")
    horizontal = axis == "x"
    dx, dy = (0, cap) if horizontal else (cap, 0)
    for (x1, y1), (x2, y2) in (
        (start, end),
        ((start[0] - dx, start[1] - dy), (start[0] + dx, start[1] + dy)),
        ((end[0] - dx, end[1] - dy), (end[0] + dx, end[1] + dy)),
    ):
        ET.SubElement(
            parent,
            f"{{{SVG}}}line",
            {
                "x1": f"{x1:.3f}",
                "x2": f"{x2:.3f}",
                "y1": f"{y1:.3f}",
                "y2": f"{y2:.3f}",
                "stroke": color,
                "stroke-width": "2",
            },
        )


# Purpose: Enforce structural resource limits before allocating each XML element.
# Inputs: XMLParser callbacks for one bounded document.
# Outputs: Builds an ElementTree or rejects declarations, processing instructions and oversized trees.
class BoundedDocumentBuilder(ET.TreeBuilder):
    # Purpose: Initialize independent counters for one document.
    # Inputs: None. Outputs: Empty builder with zero depth and node count.
    def __init__(self) -> None:
        super().__init__()
        self.depth = 0
        self.nodes = 0

    # Purpose: Admit one element before constructing it.
    # Inputs: Expanded tag and attribute mapping. Outputs: Element or a resource-limit failure.
    def start(self, tag: str, attrs: dict[str, str]) -> ET.Element:
        self.depth += 1
        self.nodes += 1
        if self.depth > MAX_DOCUMENT_DEPTH or self.nodes > MAX_DOCUMENT_NODES:
            raise ValueError("XML document exceeds its structural bounds")
        return super().start(tag, attrs)

    # Purpose: Close one admitted element and maintain the live-depth counter.
    # Inputs: Expanded closing tag. Outputs: Completed element.
    def end(self, tag: str) -> ET.Element:
        result = super().end(tag)
        self.depth -= 1
        return result

    # Purpose: Reject a DTD independently of the lexical admission guard.
    # Inputs: Document type identifiers. Outputs: Always raises before processing declarations.
    def doctype(self, name: str, pubid: str | None, system: str | None) -> None:
        raise ValueError("XML document type declarations are forbidden")

    # Purpose: Reject processing instructions instead of silently discarding them.
    # Inputs: Instruction target and data. Outputs: Always raises.
    def pi(self, target: str, text: str) -> None:
        raise ValueError("XML processing instructions are forbidden")


# Purpose: Parse only bounded UTF-8 XML without DTDs, entities or unbounded tree construction.
# Inputs: Complete bytes or text from a generated or reviewed chart.
# Outputs: Returns the document root; raises ValueError before declaration processing or on invalid input.
def parse_bounded_document(payload: bytes | str) -> ET.Element:
    if not isinstance(payload, (bytes, str)) or len(payload) > MAX_DOCUMENT_BYTES:
        raise ValueError("XML document exceeds its byte bound")
    try:
        text = payload.decode("utf-8", errors="strict") if isinstance(payload, bytes) else payload
        if len(text.encode("utf-8")) > MAX_DOCUMENT_BYTES or "\x00" in text or "<!" in text:
            raise ValueError("XML document contains a declaration or exceeds its byte bound")
        return ET.fromstring(text, parser=ET.XMLParser(target=BoundedDocumentBuilder()))
    except (UnicodeError, ET.ParseError) as error:
        raise ValueError("malformed UTF-8 XML document") from error


# Purpose: Read and parse one chart without first loading an unbounded file into memory.
# Inputs: Chart path. Outputs: Document root or an admission/parse failure.
def read_bounded_document(path: Path) -> ET.Element:
    with path.open("rb") as stream:
        return parse_bounded_document(stream.read(MAX_DOCUMENT_BYTES + 1))


# Purpose: Load only the bounded, passive document skeleton owned by the repository.
# Inputs: Path to the checked-in SVG template; alternate paths are used by negative tests.
# Outputs: Returns a namespaced SVG root; raises ValueError on malformed or active markup.
def load_template(path: Path = TEMPLATE_PATH) -> ET.Element:
    with path.open("rb") as stream:
        payload = stream.read(MAX_TEMPLATE_BYTES + 1)
    if len(payload) > MAX_TEMPLATE_BYTES:
        raise ValueError("SVG template exceeds its bound")
    root = parse_bounded_document(payload)
    if not isinstance(root.tag, str) or not root.tag.startswith("{") or not root.tag.endswith("}svg"):
        raise ValueError("SVG template requires a namespaced document root")
    namespace = root.tag[:-3]
    expected = (
        ("title", {"id": "title"}),
        ("desc", {"id": "desc"}),
        ("rect", {"width": "", "height": "", "fill": "#ffffff"}),
    )
    if root.attrib or root.text or root.tail or len(root) != len(expected):
        raise ValueError("SVG template has unexpected document content")
    for element, (tag, attributes) in zip(root, expected, strict=True):
        if (
            element.tag != namespace + tag
            or element.attrib != attributes
            or len(element)
            or element.text
            or element.tail
        ):
            raise ValueError("SVG template has unexpected or active elements")
    return root


_TEMPLATE = load_template()
SVG = _TEMPLATE.tag[1:-4]
ET.register_namespace("", SVG)


# Purpose: Create an independent accessible chart document using the passive shared template.
# Inputs: Six viewport/accessibility attributes and plain title/description text.
# Outputs: Returns a fresh root with escaped text and matching background geometry; rejects unsafe attributes.
def new_document(attributes: dict[str, str], title: str, description: str) -> ET.Element:
    if (
        set(attributes) != ROOT_ATTRIBUTES
        or any(not isinstance(value, str) or len(value) > 128 for value in attributes.values())
        or attributes["role"] != "img"
        or attributes["aria-labelledby"] not in ("title desc", "title description")
    ):
        raise ValueError("invalid SVG document attributes")
    for field in ("width", "height"):
        value = attributes[field]
        if not value.isascii() or not value.isdigit() or not 1 <= int(value) <= 65535:
            raise ValueError("invalid SVG viewport dimension")
    if attributes["viewBox"] != f"0 0 {attributes['width']} {attributes['height']}":
        raise ValueError("SVG viewport and background geometry disagree")
    if not isinstance(title, str) or not isinstance(description, str) or len(title) > 4096 or len(description) > 8192:
        raise ValueError("SVG accessibility text exceeds its bound")
    root = copy.deepcopy(_TEMPLATE)
    root.attrib.update(attributes)
    root[0].text = title
    root[1].set("id", attributes["aria-labelledby"].split()[1])
    root[1].text = description
    root[2].set("width", attributes["width"])
    root[2].set("height", attributes["height"])
    return root
