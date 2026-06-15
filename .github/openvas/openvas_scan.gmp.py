#!/usr/bin/env python3
"""Run a bounded Greenbone/OpenVAS scan through gvm-script."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any


DEFAULT_SCAN_CONFIG_ID = "daba56c8-73ec-11df-a475-002264764cea"
DEFAULT_SCANNER_ID = "08b69003-5fc2-4037-a479-93b440211c73"
TARGET_RE = re.compile(r"^[A-Za-z0-9._:/,-]+$")


def parse_bool(value: str) -> bool:
    """Purpose: Parse a workflow boolean string.
    Inputs: `value` is a string supplied by the GitHub workflow or default.
    Outputs: True for common affirmative values, otherwise False.
    """
    return value.strip().lower() in {"1", "true", "yes", "y", "on"}


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Purpose: Parse OpenVAS scan script arguments supplied by gvm-script.
    Inputs: `argv` contains script path plus target, output path, IDs, and timeouts.
    Outputs: argparse.Namespace with validated command options.
    """
    parser = argparse.ArgumentParser(description="Run a Greenbone/OpenVAS scan.")
    parser.add_argument("target", help="Authorized scan target host, IP, or CIDR.")
    parser.add_argument("output_dir", help="Directory for XML and JSON reports.")
    parser.add_argument(
        "scan_config_id",
        nargs="?",
        default=DEFAULT_SCAN_CONFIG_ID,
        help="Greenbone scan config UUID.",
    )
    parser.add_argument(
        "scanner_id",
        nargs="?",
        default=DEFAULT_SCANNER_ID,
        help="Greenbone scanner UUID.",
    )
    parser.add_argument(
        "port_list_id",
        nargs="?",
        default="",
        help="Optional Greenbone port-list UUID.",
    )
    parser.add_argument(
        "max_minutes",
        nargs="?",
        type=int,
        default=180,
        help="Maximum minutes to wait for scan completion.",
    )
    parser.add_argument(
        "delete_task",
        nargs="?",
        default="true",
        help="Delete the temporary task after collecting the report.",
    )
    parsed = parser.parse_args(argv[1:])
    if not TARGET_RE.fullmatch(parsed.target):
        raise SystemExit("Target contains characters outside the allowlist.")
    if parsed.max_minutes < 1 or parsed.max_minutes > 720:
        raise SystemExit("max_minutes must be between 1 and 720.")
    return parsed


def xml_text(element: Any, path: str, default: str = "") -> str:
    """Purpose: Return text from a child XML path.
    Inputs: `element` is an lxml/ElementTree-compatible node, `path` is an XPath-like child path,
    and `default` is returned when the child is missing.
    Outputs: Extracted text with whitespace trimmed, or `default`.
    """
    found = element.find(path)
    if found is None or found.text is None:
        return default
    return found.text.strip()


def serialize_xml(element: Any, output_path: Path) -> None:
    """Purpose: Persist a GMP XML response to disk.
    Inputs: `element` is an lxml/ElementTree-compatible node and `output_path` is a report path.
    Outputs: Writes UTF-8 XML to `output_path`.
    """
    try:
        payload = ET.tostring(element, encoding="utf-8")
    except TypeError:
        payload = ET.tostring(ET.fromstring(element), encoding="utf-8")
    output_path.write_bytes(payload)


def create_target(gmp: Any, name: str, target: str, port_list_id: str) -> str:
    """Purpose: Create a temporary OpenVAS target.
    Inputs: `gmp` is the authenticated GMP session, `name` is unique display text, `target`
    is the authorized host/IP/CIDR, and `port_list_id` optionally constrains ports.
    Outputs: Greenbone target UUID.
    """
    kwargs: dict[str, Any] = {"name": name, "hosts": [target]}
    if port_list_id:
        kwargs["port_list_id"] = port_list_id
    response = gmp.create_target(**kwargs)
    target_id = response.get("id")
    if not target_id:
        raise RuntimeError("Greenbone did not return a target id.")
    return str(target_id)


def create_task(
    gmp: Any,
    name: str,
    target_id: str,
    scan_config_id: str,
    scanner_id: str,
) -> str:
    """Purpose: Create a temporary OpenVAS scan task.
    Inputs: `gmp` is the authenticated GMP session, `name` is unique display text,
    `target_id` identifies the target, and config/scanner IDs select scan behavior.
    Outputs: Greenbone task UUID.
    """
    response = gmp.create_task(
        name=name,
        config_id=scan_config_id,
        target_id=target_id,
        scanner_id=scanner_id,
    )
    task_id = response.get("id")
    if not task_id:
        raise RuntimeError("Greenbone did not return a task id.")
    return str(task_id)


def start_task(gmp: Any, task_id: str) -> str:
    """Purpose: Start an OpenVAS scan task.
    Inputs: `gmp` is the authenticated GMP session and `task_id` identifies the task.
    Outputs: Greenbone report UUID allocated for the running scan.
    """
    response = gmp.start_task(task_id)
    report_id = xml_text(response, "report_id")
    if not report_id and len(response):
        report_id = (response[0].text or "").strip()
    if not report_id:
        raise RuntimeError("Greenbone did not return a report id.")
    return report_id


def wait_for_task(gmp: Any, task_id: str, max_minutes: int) -> dict[str, Any]:
    """Purpose: Poll a scan task until it reaches a terminal state or times out.
    Inputs: `gmp` is the authenticated GMP session, `task_id` identifies the task, and
    `max_minutes` bounds CI runtime.
    Outputs: Dictionary with status, progress, and poll count.
    """
    deadline = time.monotonic() + max_minutes * 60
    polls = 0
    while True:
        polls += 1
        response = gmp.get_task(task_id=task_id)
        task = response.find("task")
        if task is None:
            raise RuntimeError("Greenbone task response did not include a task node.")
        status = xml_text(task, "status", "Unknown")
        progress = xml_text(task, "progress", "0")
        if status in {"Done", "Stopped", "Interrupted"}:
            return {"status": status, "progress": progress, "polls": polls}
        if time.monotonic() >= deadline:
            raise TimeoutError(f"OpenVAS scan timed out after {max_minutes} minutes.")
        time.sleep(30)


def cleanup_greenbone_resources(
    gmp: Any,
    task_id: str | None,
    target_id: str | None,
    delete_resources: bool,
    task_finished: bool,
) -> list[str]:
    """Purpose: Remove temporary Greenbone resources created for one CI scan.
    Inputs: `gmp` is the authenticated GMP session, `task_id` and `target_id` are
    optional created resource IDs, `delete_resources` reflects workflow policy, and
    `task_finished` tells whether stopping the task can be skipped.
    Outputs: List of cleanup failure messages; successful cleanup returns an empty list.
    """
    if not delete_resources:
        return []

    errors: list[str] = []
    if task_id and not task_finished:
        try:
            gmp.stop_task(task_id=task_id)
        except Exception as exc:  # pragma: no cover - depends on remote scanner state
            errors.append(f"Unable to stop temporary task {task_id}: {exc}")
    if task_id:
        try:
            gmp.delete_task(task_id=task_id, ultimate=True)
        except Exception as exc:  # pragma: no cover - depends on remote scanner state
            errors.append(f"Unable to delete temporary task {task_id}: {exc}")
    if target_id:
        try:
            gmp.delete_target(target_id=target_id, ultimate=True)
        except Exception as exc:  # pragma: no cover - depends on remote scanner state
            errors.append(f"Unable to delete temporary target {target_id}: {exc}")
    return errors


def summarize_report(report_xml_path: Path, target: str, task_state: dict[str, Any]) -> dict[str, Any]:
    """Purpose: Build a compact JSON summary from a Greenbone XML report.
    Inputs: `report_xml_path` points to the downloaded XML report, `target` names the
    authorized scan target, and `task_state` contains the terminal task status.
    Outputs: Summary dictionary with severity bucket counts.
    """
    root = ET.fromstring(report_xml_path.read_bytes())
    counts = {"critical": 0, "high": 0, "medium": 0, "low": 0, "log": 0}
    for result in root.findall(".//result"):
        try:
            severity = float(xml_text(result, "severity", "0") or "0")
        except ValueError:
            severity = 0.0
        if severity >= 9.0:
            counts["critical"] += 1
        elif severity >= 7.0:
            counts["high"] += 1
        elif severity >= 4.0:
            counts["medium"] += 1
        elif severity > 0.0:
            counts["low"] += 1
        else:
            counts["log"] += 1
    return {
        "tool": "Greenbone/OpenVAS",
        "target": target,
        "generated_at_utc": dt.datetime.now(dt.UTC).isoformat(),
        "task_status": task_state,
        "severity_counts": counts,
    }


def main(gmp: Any, args: Any) -> int:
    """Purpose: gvm-script entrypoint for CI OpenVAS scanning.
    Inputs: `gmp` is the authenticated GMP session and `args.argv` is provided by gvm-script.
    Outputs: Process exit code; writes XML and JSON reports to the requested directory.
    """
    parsed = parse_args(args.argv)
    output_dir = Path(parsed.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    timestamp = dt.datetime.now(dt.UTC).strftime("%Y%m%dT%H%M%SZ")
    name = f"SuperZip CI {parsed.target} {timestamp}"

    target_id: str | None = None
    task_id: str | None = None
    task_finished = False
    scan_succeeded = False
    try:
        target_id = create_target(gmp, name, parsed.target, parsed.port_list_id)
        task_id = create_task(
            gmp,
            name,
            target_id,
            parsed.scan_config_id,
            parsed.scanner_id,
        )
        report_id = start_task(gmp, task_id)
        task_state = wait_for_task(gmp, task_id, parsed.max_minutes)
        task_finished = True
        report = gmp.get_report(report_id=report_id, details=True)
        xml_path = output_dir / "openvas-report.xml"
        serialize_xml(report, xml_path)
        summary = summarize_report(xml_path, parsed.target, task_state)
        summary["target_id"] = target_id
        summary["task_id"] = task_id
        summary["report_id"] = report_id
        (output_dir / "openvas-summary.json").write_text(
            json.dumps(summary, indent=2),
            encoding="utf-8",
        )
        scan_succeeded = True
        return 0
    finally:
        cleanup_errors = cleanup_greenbone_resources(
            gmp,
            task_id,
            target_id,
            parse_bool(parsed.delete_task),
            task_finished,
        )
        for error in cleanup_errors:
            print(f"::warning::{error}", file=sys.stderr)
        if scan_succeeded and cleanup_errors:
            raise RuntimeError("Greenbone cleanup failed after successful scan.")
