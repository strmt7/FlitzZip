#!/usr/bin/env python3
"""Fail closed unless an authorized Greenbone scan completed without findings."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any


def validate_summary(summary: Any) -> dict[str, int]:
    """Purpose: Validate completion and every blocking severity count.
    Inputs: `summary` is untrusted JSON from the scanner report.
    Outputs: Returns validated counts or raises ValueError for incomplete, malformed, or positive results.
    """
    if not isinstance(summary, dict):
        raise ValueError("Greenbone summary must be an object.")
    task_state = summary.get("task_status")
    if not isinstance(task_state, dict) or task_state.get("status") != "Done":
        raise ValueError("Greenbone scan did not complete as Done.")
    raw_counts = summary.get("severity_counts")
    if not isinstance(raw_counts, dict):
        raise ValueError("Greenbone severity counts are missing.")
    counts = {}
    for severity in ("critical", "high", "medium", "low"):
        count = raw_counts.get(severity)
        if type(count) is not int or count < 0:
            raise ValueError(f"Greenbone {severity} count is missing or invalid.")
        counts[severity] = count
    if sum(counts.values()) > 0:
        raise ValueError(f"Greenbone/OpenVAS found blocking vulnerabilities: {counts}")
    return counts


def main(argv: list[str]) -> int:
    """Purpose: Enforce the live-scan gate for one summary file.
    Inputs: `argv` contains the script name and exact summary path.
    Outputs: Returns zero only for a completed, zero-finding scan; otherwise prints an error and returns one.
    """
    if len(argv) != 2:
        print("::error::Expected one Greenbone summary path.", file=sys.stderr)
        return 1
    try:
        summary = json.loads(Path(argv[1]).read_text(encoding="utf-8"))
        validate_summary(summary)
    except (OSError, ValueError) as exc:
        print(f"::error::{exc}", file=sys.stderr)
        return 1
    print("Greenbone/OpenVAS completed with zero critical, high, medium, or low vulnerabilities.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
