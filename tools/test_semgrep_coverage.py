"""Offline checks for complete, honest, metadata-only scanner coverage reports."""

from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import semgrep_coverage as coverage


class CoverageTests(unittest.TestCase):
    # Purpose: Canonicalize scanner paths without accepting traversal or host identities.
    # Inputs: Equivalent relative spellings and unsafe absolute/control-character paths.
    # Outputs: Valid paths agree; all unsafe spellings are rejected.
    def test_relative_paths(self) -> None:
        self.assertEqual(coverage.relative_path("./src\\core\\codec.cpp"), "src/core/codec.cpp")
        for value in (None, "", "./", "/source.cpp", "C:/source.cpp", "../a.py", "src/../a.py", "a\nb", "a//b"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                coverage.relative_path(value)

    # Purpose: Preserve complete inventory while exposing missing native and helper source.
    # Inputs: Vendor/HIP/test/skill paths and a scanner admission subset.
    # Outputs: Missing source fails its gate; docs and binary provenance remain visible too.
    def test_coverage_is_not_inventory_or_admission(self) -> None:
        tracked = ["src/hip/codec.hip.cpp", "tests/check.py", ".agents/skills/example/SKILL.md", "vendor/archive.zip"]
        document = {"version": "fixture", "paths": {"scanned": ["./tests/check.py"]}, "errors": []}
        report = coverage.coverage_report(tracked, document)
        self.assertEqual(report["tracked_files"], sorted(tracked))
        self.assertEqual(report["unscanned_source_files"], ["src/hip/codec.hip.cpp"])
        self.assertIn("vendor/archive.zip", report["not_scanned_tracked_files"])
        self.assertIn(".agents/skills/example/SKILL.md", report["not_scanned_tracked_files"])
        self.assertIn("not successful parsing", report["coverage_meaning"])

    # Purpose: Retain diagnostic counts and locations without leaking source or message content.
    # Inputs: An admitted source with an error carrying a deliberately private fixture message.
    # Outputs: Metadata reports the diagnostic; it never claims successful analysis or publishes text.
    def test_diagnostics_are_visible_without_source_snippets(self) -> None:
        document = {
            "version": "fixture",
            "paths": {"scanned": ["src/a.cpp", "generated/a.py"]},
            "errors": [
                {"type": "PartialParsing", "path": "./src/a.cpp", "message": "private fixture text"},
                {"type": "Timeout"},
                {"type": ["PartialParsing", [{"path": "src/a.cpp"}]], "path": "src/a.cpp"},
            ],
            "results": [{"extra": {"lines": "private fixture text"}}],
        }
        report = coverage.coverage_report(["src/a.cpp"], document)
        self.assertEqual(report["diagnostic_count"], 3)
        self.assertEqual(report["diagnostics_by_type"], {"PartialParsing": 2, "Timeout": 1})
        self.assertEqual(report["diagnostics_by_path"], {"src/a.cpp": 2})
        self.assertEqual(report["unlocated_diagnostic_count"], 1)
        self.assertEqual(report["untracked_scan_path_count"], 1)
        self.assertNotIn("private fixture text", json.dumps(report))

    # Purpose: Fail closed when scanner evidence is malformed or silently incomplete.
    # Inputs: Missing fields, wrong types, duplicate paths, and invalid diagnostic locations.
    # Outputs: Each corrupted record is rejected rather than reported as clean coverage.
    def test_invalid_evidence(self) -> None:
        valid = {"version": "fixture", "paths": {"scanned": ["a.py"]}, "errors": []}
        variants = [
            {},
            {**valid, "paths": None},
            {**valid, "paths": []},
            {**valid, "version": None},
            {**valid, "errors": None},
            {**valid, "errors": [{}]},
            {**valid, "errors": [{"type": []}]},
            {**valid, "errors": [{"type": ["PartialParsing"]}]},
            {**valid, "errors": [{"type": [None, []]}]},
            {**valid, "paths": {"scanned": ["a.py", "./a.py"]}},
            {**valid, "errors": [{"type": "Parsing", "path": "/host/a.py"}]},
        ]
        for document in variants:
            with self.subTest(document=document), self.assertRaises(ValueError):
                coverage.coverage_report(["a.py"], document)

    # Purpose: Bound the structured report and reject non-object or truncated JSON input.
    # Inputs: Temporary JSON fixture files under a deliberately small size budget.
    # Outputs: A valid object loads; malformed, oversized, and array reports fail.
    def test_bounded_json(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "report.json"
            for payload in (b"[]", b"{", b"x" * 101):
                path.write_bytes(payload)
                with patch.object(coverage, "MAX_REPORT_BYTES", 100), self.assertRaises(ValueError):
                    coverage.read_report(path)
            path.write_text("{}", encoding="utf-8")
            self.assertEqual(coverage.read_report(path), {})

    # Purpose: Include deep source, ignored-but-tracked files, vendor data, and skills from real Git.
    # Inputs: A temporary initialized repository with explicitly staged fixture files.
    # Outputs: Every tracked path appears without extension, depth, or root exclusions.
    def test_complete_git_inventory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subprocess.run(["git", "init", "--quiet", str(root)], check=True, capture_output=True)
            names = ["out/tracked.py", "vendor/pinned.zip", ".agents/skills/example/SKILL.md", "a/b/c/d/e/f/g/code.cpp"]
            for name in names:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture", encoding="utf-8")
            subprocess.run(["git", "-C", str(root), "add", "--force", "."], check=True, capture_output=True)
            self.assertEqual(coverage.tracked_paths(root), sorted(names))

    # Purpose: Prove the CLI distinguishes admitted files from an omitted source regression.
    # Inputs: Mocked complete inventory and reports with/without the expected source.
    # Outputs: Missing source returns one; complete admission returns zero, both with evidence.
    def test_cli_exit_status(self) -> None:
        for scanned, expected in (([], 1), (["src/a.cpp"], 0)):
            document = {"version": "fixture", "paths": {"scanned": scanned}, "errors": []}
            with (
                patch("sys.argv", ["coverage", "--report", "fixture.json"]),
                patch.object(coverage, "tracked_paths", return_value=["src/a.cpp"]),
                patch.object(coverage, "read_report", return_value=document),
                patch("builtins.print") as output,
            ):
                self.assertEqual(coverage.main(), expected)
                missing = json.loads(output.call_args.args[0])["unscanned_source_files"]
                self.assertEqual(missing, [] if scanned else ["src/a.cpp"])


if __name__ == "__main__":
    unittest.main()
