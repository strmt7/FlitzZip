"""Independent artifact and measurement-boundary regressions for native coverage."""

import copy
import hashlib
import json
import os
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest.mock import patch
from urllib.parse import urlsplit
from urllib.request import Request

from tools import native_coverage as collector
from tools.native_coverage_report import normalize_report, read_report, source_path


class CoverageTests(unittest.TestCase):
    # Purpose: Own small source and binary fixtures. Inputs: None. Outputs: Cleanup-bound test directory.
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        (self.root / "src").mkdir()
        (self.root / "src/example.cpp").write_text("first\nsecond\nthird\n", encoding="utf-8")
        self.raw = self.root / "raw.xml"
        self.output = self.root / "native.xml"
        self.xml = '<coverage><sources><source>src</source></sources><packages><package name="native"><classes>'
        self.xml += '<class name="example" filename="example.cpp"><lines><line number="1" hits="2"/>'
        self.xml += '<line number="2" hits="0"/></lines></class></classes></package></packages></coverage>'

    # Purpose: Prove normalization retains executed and unexecuted lines rather than manufacturing coverage.
    # Inputs: Hand-authored source roots and line evidence.
    # Outputs: Portable paths, unchanged hits and independent totals.
    def test_report_preserves_hits_and_uncovered_lines(self):
        self.raw.write_text(self.xml, encoding="utf-8")
        summary = normalize_report(self.raw, self.output, self.root)
        self.assertEqual(summary, {"files": ["src/example.cpp"], "lines_valid": 2, "lines_covered": 1})
        tree = read_report(self.output)
        self.assertEqual(tree.find("./sources/source").text, ".")
        self.assertEqual(tree.find(".//class").get("filename"), "src/example.cpp")
        self.assertEqual(
            [line.attrib for line in tree.findall(".//line")],
            [{"number": "1", "hits": "2"}, {"number": "2", "hits": "0"}],
        )

    # Purpose: Reject empty, foreign, malformed and stale-source measurements before publication.
    # Inputs: Independent invalid records. Outputs: Explicit failures with no published report.
    def test_invalid_evidence_never_publishes(self):
        fixtures = [
            self.xml.replace('hits="2"', 'hits="0"'),
            self.xml.replace('hits="2"', 'hits="-1"'),
            self.xml.replace('number="1"', 'number="4"'),
            self.xml.replace('number="1"', 'number="0"'),
            self.xml.replace('filename="example.cpp"', 'filename="missing.cpp"'),
            "<coverage><packages/></coverage>",
            "<invalid/>",
            '<!DOCTYPE coverage [<!ENTITY example "expanded">]>' + self.xml,
        ]
        for value in fixtures:
            with self.subTest(value=value):
                self.raw.write_text(value, encoding="utf-8")
                with self.assertRaises(ValueError):
                    normalize_report(self.raw, self.output, self.root)
                self.assertFalse(self.output.exists())

    # Purpose: Keep source identity inside first-party source even when XML supplies outside roots.
    # Inputs: Traversal, absolute foreign sources and conflicting source roots. Outputs: Refusal, never guessed mapping.
    def test_foreign_and_ambiguous_sources(self):
        (self.root / "foreign.cpp").write_text("outside\n", encoding="utf-8")
        for name, roots in [("../foreign.cpp", ["src"]), (str(self.root / "foreign.cpp"), ["src"])]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                source_path(name, roots, self.root)
        (self.root / "src/second").mkdir()
        (self.root / "src/second/example.cpp").write_text("other\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            source_path("example.cpp", ["src", "src/second"], self.root)

    # Purpose: Resolve the collector's actual Windows drive-root Cobertura representation.
    # Inputs: Owned source expressed relative to its drive. Outputs: Exact mapping without using the drive's cwd.
    @unittest.skipUnless(os.name == "nt", "The native collector and Windows drive paths require Windows")
    def test_collector_drive_root_is_absolute(self):
        source = self.root / "src/example.cpp"
        name = source.relative_to(source.anchor).as_posix()
        self.assertEqual(source_path(name, [source.drive], self.root), "src/example.cpp")
        with self.assertRaises(ValueError):
            source_path("unrelated-repository/src/example.cpp", [source.drive], self.root)

    # Purpose: Merge repeated header line evidence without counting duplicate declarations as extra lines.
    # Inputs: Two measured classes for one source. Outputs: Union of real line hits and untouched measured classes.
    def test_duplicate_source_lines_are_counted_once(self):
        element = ET.fromstring(self.xml)
        classes = element.find("./packages/package/classes")
        repeated = copy.deepcopy(classes[0])
        repeated.set("name", "another module")
        repeated.find("./lines/line[@number='2']").set("hits", "1")
        classes.append(repeated)
        self.raw.write_bytes(ET.tostring(element))
        summary = normalize_report(self.raw, self.output, self.root)
        self.assertEqual((summary["lines_valid"], summary["lines_covered"]), (2, 2))
        self.assertEqual(len(read_report(self.output).findall(".//class")), 2)

    # Purpose: Reject entity expansion independently of XML encoding and use the same reader for all consumers.
    # Inputs: Entity-bearing UTF-8/16/32 and ordinary UTF-16 documents. Outputs: Refusal before parsing expansion.
    def test_xml_reader_rejects_encoded_entities(self):
        entity = '<!DOCTYPE coverage [<!ENTITY repeated "payload">]><coverage>&repeated;</coverage>'
        for encoding in ("utf-8", "utf-16", "utf-16-le", "utf-32", "utf-32-le"):
            with self.subTest(encoding=encoding):
                self.raw.write_bytes(entity.encode(encoding))
                with self.assertRaises(ValueError):
                    read_report(self.raw)
        self.raw.write_bytes(self.xml.encode("utf-16-le"))
        with self.assertRaises(ValueError):
            read_report(self.raw)

    # Purpose: Exercise the normalization boundary with an entity disguised by UTF-16 byte layout.
    # Inputs: Otherwise valid source/hit evidence with an entity-valued hit count. Outputs: No report publication.
    def test_normalizer_rejects_encoded_entity_attribute(self):
        xml = '<!DOCTYPE coverage [<!ENTITY observed "2">]>' + self.xml.replace('hits="2"', 'hits="&observed;"')
        self.raw.write_bytes(xml.encode("utf-16-le"))
        with self.assertRaises(ValueError):
            normalize_report(self.raw, self.output, self.root)
        self.assertFalse(self.output.exists())

    # Purpose: Prove a trusted initial download cannot redirect to an unauthorized transport or host.
    # Inputs: Independent scheme, authority and credential mutations. Outputs: Every hostile hop fails before access.
    def test_installer_redirects_remain_on_official_https(self):
        initial = "https://github.com/OpenCppCoverage/OpenCppCoverage/releases/download/release-0.9.9.0/installer.exe"
        valid = "https://release-assets.githubusercontent.com/release-asset?signature=public-fixture"
        handler = collector.ReleaseRedirects()
        request = Request(initial)
        self.assertEqual(handler.redirect_request(request, None, 302, "Found", {}, valid).full_url, valid)
        parsed = urlsplit(valid)
        mutations = [
            parsed._replace(scheme="file"),
            parsed._replace(scheme="http"),
            parsed._replace(netloc="github.com.example.invalid"),
            parsed._replace(netloc="192.0.2.1"),  # RFC 5737 documentation address; IP hosts are never admitted.
            parsed._replace(netloc="user@github.com"),
            parsed._replace(netloc="github.com:444"),
            parsed._replace(fragment="unexpected"),
        ]
        for changed in mutations:
            with self.subTest(url=changed.geturl()), self.assertRaises(ValueError):
                handler.redirect_request(request, None, 302, "Found", {}, changed.geturl())

    # Purpose: Reject changed collector code and unexpected runtime libraries.
    # Inputs: A complete binary fixture and separate mutations. Outputs: Identity-bound acceptance or refusal.
    def test_collector_inventory_and_hashes(self):
        directory = self.root / "collector"
        directory.mkdir()
        files = {"OpenCppCoverage.exe": b"collector fixture", "runtime.dll": b"runtime fixture"}
        hashes = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
        for name, data in files.items():
            (directory / name).write_bytes(data)
        self.assertEqual(collector.verify_collector(directory, hashes), directory / "OpenCppCoverage.exe")
        (directory / "runtime.dll").write_bytes(b"changed")
        with self.assertRaises(ValueError):
            collector.verify_collector(directory, hashes)
        (directory / "runtime.dll").write_bytes(files["runtime.dll"])
        (directory / "unexpected.dll").write_bytes(b"extra")
        with self.assertRaises(ValueError):
            collector.verify_collector(directory, hashes)
        with self.assertRaises(ValueError):
            collector.owned_path(self.root, self.root / "../outside")

    # Purpose: Preserve collector failures, deadlines and excess output as failed qualification.
    # Inputs: Controlled outcomes from the real shared process boundary. Outputs: Retained diagnostics and exception.
    def test_child_failures_remain_failures(self):
        outcome = {"exit_code": 0, "timed_out": False, "output_limit_exceeded": False}
        for field, value in [("exit_code", 9), ("timed_out", True), ("output_limit_exceeded", True)]:
            failure = {**outcome, field: value}
            with self.subTest(field=field), patch.object(collector, "process_owner") as owner:
                owner.return_value.run_bounded_command.return_value = failure
                with self.assertRaises(RuntimeError):
                    collector.run_checked(["owned-test-command"], 2, self.root / "process.json")
                self.assertEqual(json.loads((self.root / "process.json").read_text()), failure)


if __name__ == "__main__":
    unittest.main()
