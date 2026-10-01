"""Offline regressions for lossless, excerpt-free DevSkim SARIF publication."""

from __future__ import annotations

import copy
import io
import json
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from unittest.mock import patch

from tools import devskim_report as publication


# Purpose: Reproduce the pinned writer's empty rendered excerpt with identifiable finding metadata.
# Inputs: None; synthetic report is never submitted to GitHub code scanning.
# Outputs: New SARIF object with one finding, region, severity, message, and fingerprint.
def fixture() -> dict:
    return {
        "version": "2.1.0",
        "runs": [
            {
                "tool": {"driver": {"name": "devskim"}},
                "results": [
                    {
                        "ruleId": "test-rule",
                        "level": "warning",
                        "message": {"text": "Test finding"},
                        "partialFingerprints": {"identity": "fixture"},
                        "locations": [
                            {
                                "physicalLocation": {
                                    "artifactLocation": {"uri": "sample.c"},
                                    "region": {
                                        "startLine": 3,
                                        "startColumn": 2,
                                        "endColumn": 8,
                                        "snippet": {"text": "", "rendered": {"markdown": "``"}},
                                    },
                                }
                            }
                        ],
                    }
                ],
            }
        ],
        "properties": {"physicalLocation": {"region": {"snippet": "unrelated metadata"}}},
    }


class DevSkimReportTests(unittest.TestCase):
    # Purpose: Prove exact finding and metadata parity, extension preservation, and idempotence.
    # Inputs: Two independently identified findings from the synthetic pinned-writer report.
    # Outputs: Assertions reject filtering or unrelated metadata changes.
    def test_preserves_all_finding_metadata(self) -> None:
        report = fixture()
        second = copy.deepcopy(report["runs"][0]["results"][0])
        second["partialFingerprints"]["identity"] = "second"
        report["runs"][0]["results"].append(second)
        expected = copy.deepcopy(report)
        for finding in expected["runs"][0]["results"]:
            del finding["locations"][0]["physicalLocation"]["region"]["snippet"]
        self.assertEqual(publication.prepare_report(report), 2)
        self.assertEqual(report, expected)
        self.assertEqual(publication.prepare_report(report), 0)

    # Purpose: Refuse unexpected content instead of silently stripping real excerpts.
    # Inputs: Malformed, binary, and nonempty snippet variants.
    # Outputs: Every variant fails publication before any output is created.
    def test_rejects_nonempty_and_unknown_snippets(self) -> None:
        cases = [
            None,
            [],
            {"text": None},
            {"text": "source"},
            {"binary": "encoded"},
            {"rendered": None},
            {"rendered": {"text": "source"}},
            {"rendered": {"markdown": "`source`"}},
            {"rendered": {"extra": ""}},
        ]
        for snippet in cases:
            with self.subTest(snippet=snippet), self.assertRaises(ValueError):
                report = fixture()
                report["runs"][0]["results"][0]["locations"][0]["physicalLocation"]["region"]["snippet"] = snippet
                publication.prepare_report(report)

    # Purpose: Cover optional empty forms in both SARIF physical-region fields.
    # Inputs: Empty snippets and the no-results scanner success case.
    # Outputs: Empty optional fields are removed while zero findings remain valid.
    def test_empty_variants_and_context_region(self) -> None:
        for snippet in ({}, {"text": ""}, {"rendered": {"text": "", "markdown": "``"}}):
            report = fixture()
            location = report["runs"][0]["results"][0]["locations"][0]["physicalLocation"]
            location["region"]["snippet"] = snippet
            location["contextRegion"] = {"startLine": 1, "snippet": copy.deepcopy(snippet)}
            self.assertEqual(publication.prepare_report(report), 2)
        report = fixture()
        report["runs"][0]["results"] = []
        self.assertEqual(publication.prepare_report(report), 0)

    # Purpose: Fail closed on producer drift, malformed root fields, and structure exhaustion.
    # Inputs: Invalid envelopes and reduced test-only resource budgets.
    # Outputs: ValueError for each incompatible or oversized structure.
    def test_report_shape_and_structure_budgets(self) -> None:
        cases = [
            [],
            {},
            {"version": "2.1.0", "runs": []},
            {"version": "2.1.0", "runs": [None]},
            {"version": "2.1.0", "runs": [{"tool": []}]},
            {"version": "2.1.0", "runs": [{"tool": {"driver": []}}]},
        ]
        bad = fixture()
        bad["runs"][0]["tool"]["driver"]["name"] = "another producer"
        cases.append(bad)
        bad = fixture()
        bad["runs"][0]["results"] = {}
        cases.append(bad)
        cases.append({"version": "2.1.0", "runs": fixture()["runs"] * 17})
        for report in cases:
            with self.subTest(report=report), self.assertRaises(ValueError):
                publication.prepare_report(report)
        for name in ("MAX_DEPTH", "MAX_NODES"):
            with patch.object(publication, name, 2), self.assertRaises(ValueError):
                publication.prepare_report(fixture())

    # Purpose: Verify bounded decoding, exclusive publication, and failed-link cleanup.
    # Inputs: Temporary report files with collisions, malformed JSON, and simulated I/O failure.
    # Outputs: Valid counts and complete bytes only; refused files remain unchanged.
    def test_file_publication_boundaries(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, destination = root / "raw.json", root / "published.json"
            source.write_text(json.dumps(fixture()), encoding="utf-8")
            self.assertEqual(publication.publish_report(source, destination), (1, 1))
            previous = destination.read_bytes()
            with self.assertRaises(FileExistsError):
                publication.publish_report(source, destination)
            self.assertEqual(destination.read_bytes(), previous)
            destination.unlink()
            with patch.object(publication.os, "link", side_effect=OSError), self.assertRaises(OSError):
                publication.publish_report(source, destination)
            self.assertFalse(destination.exists())
            self.assertEqual(sorted(path.name for path in root.iterdir()), ["raw.json"])
            for payload in ('{"version":"2.1.0","version":"2.1.0"}', "{", "NaN"):
                source.write_text(payload, encoding="utf-8")
                with self.assertRaises(ValueError):
                    publication.publish_report(source, destination)
                self.assertFalse(destination.exists())
            source.write_text(json.dumps(fixture()), encoding="utf-8")
            with patch.object(publication, "MAX_REPORT_BYTES", 2), self.assertRaises(ValueError):
                publication.publish_report(source, destination)

    # Purpose: Keep decoder failures from exposing private raw-report content in CI logs.
    # Inputs: Malformed private fixture and mocked CLI arguments.
    # Outputs: Fixed value-free error with nonzero status and no destination.
    def test_cli_errors_do_not_echo_content(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "raw.json"
            source.write_text('{"private fixture":', encoding="utf-8")
            destination = Path(directory) / "published.json"
            error = io.StringIO()
            with patch("sys.argv", ["devskim_report", str(source), str(destination)]), redirect_stderr(error):
                self.assertEqual(publication.main(), 2)
            self.assertNotIn("private fixture", error.getvalue())
            self.assertFalse(destination.exists())

    # Purpose: Prevent CI from bypassing the report boundary or restoring source excerpt collection.
    # Inputs: Canonical hosted security workflow source.
    # Outputs: Assertions enforce scan/publication/upload ordering and disabled suppression.
    def test_workflow_publishes_before_upload(self) -> None:
        root = Path(__file__).resolve().parents[1]
        workflow = (root / ".github/workflows/security-code-scanning.yml").read_text(encoding="utf-8")
        start = workflow.index("      - name: Run DevSkim")
        end = workflow.index("\n  osv-scanner:", start)
        steps = workflow[start:end]
        self.assertIn("--disable-supression --skip-excerpts", steps)
        self.assertIn('-O "$RUNNER_TEMP/devskim-raw.sarif"', steps)
        self.assertLess(steps.index("analyze -I"), steps.index("python -m tools.devskim_report"))
        self.assertLess(steps.index("python -m tools.devskim_report"), steps.index("Upload DevSkim SARIF"))


if __name__ == "__main__":
    unittest.main()
