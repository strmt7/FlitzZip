"""Regression tests for the offline Greenbone integration and live gate."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from types import SimpleNamespace


def load_script(name: str, filename: str):
    """Purpose: Import a repository script by path, including gvm's dotted script name.
    Inputs: `name` is the module name and `filename` is adjacent to this test.
    Outputs: Returns the loaded module or fails when its import contract is broken.
    """
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to import {filename}.")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SCAN = load_script("openvas_scan", "openvas_scan.gmp.py")
GATE = load_script("enforce_gate", "enforce_gate.py")


class FakeGmp:
    """Provide deterministic terminal task states without contacting a scanner."""

    def __init__(self, status: str = "Done") -> None:
        """Purpose: Prepare a fake task; inputs: terminal `status`; outputs: empty call history."""
        self.status = status
        self.calls: list[tuple] = []

    def create_target(self, **kwargs):
        """Purpose: Allocate a fake target; inputs: GMP fields; outputs: stable target ID."""
        self.calls.append(("create_target", kwargs))
        return {"id": "target-1"}

    def create_task(self, **kwargs):
        """Purpose: Allocate a fake task; inputs: GMP fields; outputs: stable task ID."""
        self.calls.append(("create_task", kwargs))
        return {"id": "task-1"}

    def start_task(self, task_id):
        """Purpose: Start the fake task; inputs: task ID; outputs: stable report ID."""
        self.calls.append(("start_task", task_id))
        return ET.fromstring("<start_task_response><report_id>report-1</report_id></start_task_response>")

    def get_task(self, task_id):
        """Purpose: Poll the fake task; inputs: task ID; outputs: configured terminal state."""
        self.calls.append(("get_task", task_id))
        return ET.fromstring(
            f"<get_tasks_response><task><status>{self.status}</status>"
            "<progress>100</progress></task></get_tasks_response>"
        )

    def get_report(self, report_id, details, ignore_pagination=False):
        """Purpose: Return a clean fake report; inputs: report request; outputs: XML report."""
        self.calls.append(("get_report", report_id, details, ignore_pagination))
        return ET.fromstring("<get_reports_response><report/></get_reports_response>")

    def stop_task(self, task_id):
        """Purpose: Record a stop request; inputs: task ID; outputs: recorded call."""
        self.calls.append(("stop_task", task_id))

    def delete_task(self, task_id, ultimate=False):
        """Purpose: Record task cleanup; inputs: task ID and mode; outputs: recorded call."""
        self.calls.append(("delete_task", task_id, ultimate))

    def delete_target(self, target_id, ultimate=False):
        """Purpose: Record target cleanup; inputs: target ID and mode; outputs: recorded call."""
        self.calls.append(("delete_target", target_id, ultimate))


class OpenvasScanTests(unittest.TestCase):
    """Exercise scan completion, cleanup, and the report gate independently."""

    def scan_args(self, output_dir: str) -> SimpleNamespace:
        """Purpose: Build valid gvm-script arguments; inputs: output directory; outputs: args object."""
        return SimpleNamespace(
            argv=[
                "openvas_scan.gmp.py",
                "scan-target.example",
                output_dir,
                SCAN.DEFAULT_SCAN_CONFIG_ID,
                SCAN.DEFAULT_SCANNER_ID,
                "",
                "5",
                "true",
            ]
        )

    def test_parser_rejects_invalid_target(self):
        """Purpose: Keep target authorization input bounded; inputs: bad name; outputs: parser rejection."""
        with self.assertRaises(SystemExit):
            SCAN.parse_args(["openvas_scan.gmp.py", "bad target with spaces", "reports/openvas"])

    def test_done_scan_writes_summary_and_cleans_up(self):
        """Purpose: Preserve complete-scan success; inputs: Done task; outputs: summary and resource cleanup."""
        fake = FakeGmp()
        with tempfile.TemporaryDirectory() as output_dir:
            self.assertEqual(SCAN.main(fake, self.scan_args(output_dir)), 0)
            summary = json.loads((Path(output_dir) / "openvas-summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["task_status"]["status"], "Done")
            self.assertEqual(
                GATE.validate_summary(summary),
                {
                    "critical": 0,
                    "high": 0,
                    "medium": 0,
                    "low": 0,
                },
            )
            sarif = json.loads((Path(output_dir) / "openvas-report.sarif").read_text(encoding="utf-8"))
            self.assertEqual(sarif["version"], "2.1.0")
            self.assertEqual(sarif["runs"][0]["results"], [])
        self.assertIn(("get_report", "report-1", True, True), fake.calls)
        self.assertIn(("delete_task", "task-1", True), fake.calls)
        self.assertIn(("delete_target", "target-1", True), fake.calls)

    def test_incomplete_terminal_states_never_fetch_reports(self):
        """Purpose: Reject partial scans; inputs: stopped/interrupted tasks; outputs: failure and cleanup."""
        for status in ("Stopped", "Interrupted"):
            with self.subTest(status=status), tempfile.TemporaryDirectory() as output_dir:
                fake = FakeGmp(status)
                with self.assertRaisesRegex(RuntimeError, "did not complete"):
                    SCAN.main(fake, self.scan_args(output_dir))
                self.assertFalse((Path(output_dir) / "openvas-summary.json").exists())
                self.assertFalse(any(call[0] == "get_report" for call in fake.calls))
                self.assertIn(("delete_task", "task-1", True), fake.calls)
                self.assertIn(("delete_target", "target-1", True), fake.calls)

    def test_sarif_preserves_all_results_and_summary_severity_boundaries(self):
        """Purpose: Preserve complete reports. Inputs: Repeated rules and severity boundaries.
        Outputs: Every result retained.
        """
        scores = [9, 7, 4, 0.1, 0, -1] * 20
        root = ET.Element("get_reports_response")
        results = ET.SubElement(ET.SubElement(root, "report"), "results")
        for index, score in enumerate(scores):
            result = ET.SubElement(results, "result", {"id": f"result-{index}"})
            ET.SubElement(result, "severity").text = str(score)
            ET.SubElement(result, "name").text = f"Finding {index}"
            ET.SubElement(result, "description").text = f"Complete evidence {index}"
            ET.SubElement(result, "host").text = "scan-target.example"
            ET.SubElement(result, "port").text = "443/tcp"
            ET.SubElement(result, "nvt", {"oid": f"nvt-{index % 2}"})
        with tempfile.TemporaryDirectory() as directory:
            xml_path, sarif_path = Path(directory) / "report.xml", Path(directory) / "report.sarif"
            SCAN.serialize_xml(root, xml_path)
            summary = SCAN.summarize_report(xml_path, "scan-target.example", {"status": "Done"}, sarif_path)
            run = json.loads(sarif_path.read_text(encoding="utf-8"))["runs"][0]
        self.assertEqual(summary["severity_counts"], {"critical": 20, "high": 20, "medium": 20, "low": 20, "log": 40})
        self.assertEqual(len(run["tool"]["driver"]["rules"]), 2)
        self.assertEqual(len(run["results"]), len(scores))
        for index, result in enumerate(run["results"]):
            self.assertEqual(
                result["properties"],
                {
                    "result_id": f"result-{index}",
                    "severity": scores[index],
                    "host": "scan-target.example",
                    "port": "443/tcp",
                },
            )
            self.assertEqual(result["ruleId"], f"nvt-{index % 2}")
            self.assertEqual(result["message"]["text"], f"Finding {index}\nComplete evidence {index}")
            self.assertEqual(
                result["level"], "error" if scores[index] >= 7 else "warning" if scores[index] >= 4 else "note"
            )
        with self.assertRaises(ValueError):
            GATE.validate_summary(summary)

    def test_invalid_severity_cannot_produce_a_qualified_summary_or_sarif(self):
        """Purpose: Fail closed on corrupted scores. Inputs: Missing, invalid and nonfinite severity.
        Outputs: No admitted report.
        """
        for score in (None, "", "invalid", "NaN", "Infinity", "-Infinity", "1e999"):
            with self.subTest(score=score), tempfile.TemporaryDirectory() as directory:
                xml_path, sarif_path = Path(directory) / "report.xml", Path(directory) / "report.sarif"
                root = ET.Element("report")
                result = ET.SubElement(root, "result")
                if score is not None:
                    ET.SubElement(result, "severity").text = score
                SCAN.serialize_xml(root, xml_path)
                with self.assertRaisesRegex(ValueError, "severity"):
                    SCAN.summarize_report(xml_path, "scan-target.example", {"status": "Done"}, sarif_path)
                self.assertFalse(sarif_path.exists())

    def test_empty_nvt_fields_keep_valid_sarif_rule_and_message(self):
        """Purpose: Retain generic scanner results. Inputs: Empty optional names and OID.
        Outputs: Nonempty SARIF identifiers.
        """
        with tempfile.TemporaryDirectory() as directory:
            xml_path, sarif_path = Path(directory) / "report.xml", Path(directory) / "report.sarif"
            xml_path.write_text(
                '<report><result><severity>0</severity><name/><nvt oid=""><name/></nvt></result></report>',
                encoding="utf-8",
            )
            SCAN.summarize_report(xml_path, "scan-target.example", {"status": "Done"}, sarif_path)
            run = json.loads(sarif_path.read_text(encoding="utf-8"))["runs"][0]
        self.assertEqual(
            run["tool"]["driver"]["rules"], [{"id": "greenbone/result", "shortDescription": {"text": "OpenVAS result"}}]
        )
        self.assertEqual(run["results"][0]["ruleId"], "greenbone/result")
        self.assertTrue(run["results"][0]["message"]["text"].strip())

    def test_gate_rejects_incomplete_and_malformed_summaries(self):
        """Purpose: Fail closed on partial or invalid reports; inputs: malformed data; outputs: ValueError."""
        clean = {
            "task_status": {"status": "Done"},
            "severity_counts": {
                "critical": 0,
                "high": 0,
                "medium": 0,
                "low": 0,
            },
        }
        for status in ("Stopped", "Interrupted", None):
            with self.subTest(status=status), self.assertRaises(ValueError):
                GATE.validate_summary({**clean, "task_status": {"status": status}})
        for counts in (
            None,
            {},
            {**clean["severity_counts"], "high": -1},
            {**clean["severity_counts"], "medium": "0"},
            {**clean["severity_counts"], "low": 1},
        ):
            with self.subTest(counts=counts), self.assertRaises(ValueError):
                GATE.validate_summary({**clean, "severity_counts": counts})

    def test_unfinished_cleanup_stops_and_deletes_resources(self):
        """Purpose: Preserve timeout cleanup; inputs: running task IDs; outputs: stop and delete calls."""
        fake = FakeGmp()
        self.assertEqual(SCAN.cleanup_greenbone_resources(fake, "task-2", "target-2", True, False), [])
        self.assertEqual(
            set(fake.calls),
            {
                ("stop_task", "task-2"),
                ("delete_task", "task-2", True),
                ("delete_target", "target-2", True),
            },
        )


if __name__ == "__main__":
    unittest.main()
