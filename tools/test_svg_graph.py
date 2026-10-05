"""Contracts for the passive shared SVG document and its renderer callers."""

from __future__ import annotations

import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

from tools.render_svg_graph import (
    MAX_DOCUMENT_BYTES,
    MAX_DOCUMENT_DEPTH,
    MAX_DOCUMENT_NODES,
    MAX_TEMPLATE_BYTES,
    SVG,
    TEMPLATE_PATH,
    BoundedDocumentBuilder,
    load_template,
    new_document,
    parse_bounded_document,
    read_bounded_document,
)


# Purpose: Verify document isolation, XML escaping and rejection of active or malformed templates.
# Inputs: The checked-in document and bounded temporary negative fixtures.
# Outputs: Fails on active markup, resource-bound bypass or cross-render mutation.
class SvgDocumentTests(unittest.TestCase):
    # Purpose: Reject hostile declarations and alternate encodings before the XML parser can expand entities.
    # Inputs: Entity/DTD fixtures, processing instructions, malformed encodings and oversized documents.
    # Outputs: Each hostile input fails while escaped plain text and UTF-8 documents remain readable.
    def test_document_admission_rejects_entities_encodings_and_oversize(self) -> None:
        declaration = '<!DOCTYPE svg [<!ENTITY x "expanded">]><svg>&x;</svg>'
        for payload in (
            declaration,
            declaration.encode("utf-16"),
            declaration.encode("utf-32"),
            b'<!DOCTYPE svg SYSTEM "local-file"><svg/>',
            '<!DOCTYPE svg [<!ENTITY % p SYSTEM "remote-resource">%p;]><svg/>',
            b"<svg>\xff</svg>",
            "<svg>\x00</svg>",
            "<svg>\ud800</svg>",
            "<?instruction data?><svg/>",
            "<svg>",
            b" " * (MAX_DOCUMENT_BYTES + 1),
            "é" * (MAX_DOCUMENT_BYTES // 2 + 1),
            None,
        ):
            with self.subTest(kind=type(payload).__name__), self.assertRaises(ValueError):
                parse_bounded_document(payload)
        self.assertEqual(parse_bounded_document("<svg>&lt;!DOCTYPE &amp; label</svg>").text, "<!DOCTYPE & label")
        self.assertEqual(parse_bounded_document(b'<?xml version="1.0" encoding="utf-8"?><svg/>').tag, "svg")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "oversized.svg"
            path.write_bytes(b" " * (MAX_DOCUMENT_BYTES + 1))
            with self.assertRaises(ValueError):
                read_bounded_document(path)

    # Purpose: Check exact tree limits and independent DTD callback protection.
    # Inputs: Documents at and beyond both structural limits and an intentionally unguarded test parser.
    # Outputs: Boundary documents pass; excess nodes/depth and DTDs fail before allocation or expansion.
    def test_document_structural_limits_and_independent_dtd_guard(self) -> None:
        for depth, accepted in ((MAX_DOCUMENT_DEPTH, True), (MAX_DOCUMENT_DEPTH + 1, False)):
            payload = "<g>" * depth + "</g>" * depth
            if accepted:
                self.assertEqual(len(list(parse_bounded_document(payload).iter())), depth)
            else:
                with self.assertRaises(ValueError):
                    parse_bounded_document(payload)
        for nodes, accepted in ((MAX_DOCUMENT_NODES, True), (MAX_DOCUMENT_NODES + 1, False)):
            payload = "<svg>" + "<g/>" * (nodes - 1) + "</svg>"
            if accepted:
                self.assertEqual(len(list(parse_bounded_document(payload).iter())), nodes)
            else:
                with self.assertRaises(ValueError):
                    parse_bounded_document(payload)
        parser = ET.XMLParser(target=BoundedDocumentBuilder())
        with self.assertRaisesRegex(ValueError, "document type"):
            parser.feed('<!DOCTYPE svg [<!ENTITY x "expanded">]><svg>&x;</svg>')

    # Purpose: Supply the actual six renderer attributes in their stable serialization order.
    # Inputs: None.
    # Outputs: Returns a new valid viewport/accessibility mapping.
    def attributes(self) -> dict[str, str]:
        return {
            "width": "1200",
            "height": "300",
            "viewBox": "0 0 1200 300",
            "role": "img",
            "aria-labelledby": "title description",
            "font-family": "Segoe UI, Arial, sans-serif",
        }

    # Purpose: Ensure independently rendered charts cannot share mutations or inject XML through labels.
    # Inputs: Markup-like plain labels and two independent documents.
    # Outputs: Requires escaped round-tripping and matching accessible/background geometry.
    def test_independent_documents_escape_text_and_preserve_geometry(self) -> None:
        first = new_document(self.attributes(), "<title> & label", '<desc attribute="value">')
        first[2].set("fill", "#000000")
        second = new_document(self.attributes(), "another", "description")
        self.assertEqual(second[2].attrib, {"width": "1200", "height": "300", "fill": "#ffffff"})
        self.assertEqual(second[1].attrib, {"id": "description"})
        reread = parse_bounded_document(ET.tostring(first))
        self.assertEqual(reread[0].text, "<title> & label")
        self.assertEqual(reread[1].text, '<desc attribute="value">')
        self.assertEqual(len(reread), 3)
        published = TEMPLATE_PATH.with_name("beta-native-cpu-hip.svg")
        self.assertEqual(reread.tag, read_bounded_document(published).tag)
        self.assertEqual(reread.tag, f"{{{SVG}}}svg")

    # Purpose: Reject active attributes, incoherent viewports and inaccessible or oversized text.
    # Inputs: One individually mutated document contract per negative case.
    # Outputs: Every invalid case raises ValueError before constructing a chart.
    def test_invalid_document_contracts(self) -> None:
        for field, value in (
            ("onload", "active"),
            ("role", "button"),
            ("aria-labelledby", "missing"),
            ("viewBox", "0 0 1 1"),
            ("width", "65536"),
            ("height", "-1"),
            ("width", "１２"),
            ("height", "0"),
            ("font-family", 7),
        ):
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                attributes = self.attributes()
                attributes[field] = value
                new_document(attributes, "title", "description")
        for title, description in (("x" * 4097, "desc"), ("title", "x" * 8193), (None, "desc")):
            with self.subTest(title=title), self.assertRaises(ValueError):
                new_document(self.attributes(), title, description)

    # Purpose: Exercise the actual template loader against active markup and parser/resource failures.
    # Inputs: Mutations of the exact checked-in template bytes, each written into an owned temporary directory.
    # Outputs: All malformed documents fail; the original skeleton remains usable.
    def test_template_rejects_active_malformed_and_unbounded_markup(self) -> None:
        payload = TEMPLATE_PATH.read_bytes()
        negatives = (
            payload.replace(b"<title", b'<title onload="active"'),
            payload.replace(b"<rect", b"<script"),
            payload.replace(b'<title id="title"/>', b'<title id="title"><g/></title>'),
            payload.replace(b"<svg ", b'<svg role="img" '),
            payload.replace(b"<desc", b"<other:desc"),
            payload.replace(b"<rect", b"text<rect"),
            b"<!DOCTYPE svg>" + payload,
            b"<svg>",
            b"<svg/>",
            payload + b" " * MAX_TEMPLATE_BYTES,
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "template.svg"
            for index, negative in enumerate(negatives):
                with self.subTest(index=index), self.assertRaises(ValueError):
                    path.write_bytes(negative)
                    load_template(path)
            path.write_bytes(payload)
            self.assertEqual(len(load_template(path)), 3)


if __name__ == "__main__":
    unittest.main()
