"""Instantiate the shared, passive SVG document used by benchmark renderers."""

from __future__ import annotations

import copy
import xml.etree.ElementTree as ET
from pathlib import Path

TEMPLATE_PATH = Path(__file__).resolve().parents[1] / "resources/benchmarks/chart-template.svg"
MAX_TEMPLATE_BYTES = 1024
ROOT_ATTRIBUTES = {"viewBox", "width", "height", "font-family", "role", "aria-labelledby"}


# Purpose: Load only the bounded, passive document skeleton owned by the repository.
# Inputs: Path to the checked-in SVG template; alternate paths are used by negative tests.
# Outputs: Returns a namespaced SVG root; raises ValueError on malformed or active markup.
def load_template(path: Path = TEMPLATE_PATH) -> ET.Element:
    with path.open("rb") as stream:
        payload = stream.read(MAX_TEMPLATE_BYTES + 1)
    if len(payload) > MAX_TEMPLATE_BYTES or b"<!" in payload:
        raise ValueError("SVG template exceeds its bound or contains a declaration")
    try:
        root = ET.fromstring(payload)
    except ET.ParseError as error:
        raise ValueError("malformed SVG template") from error
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
