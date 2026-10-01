"""Compare scanner-reported paths with Git's complete tracked-file inventory."""

from __future__ import annotations

import argparse
import json
import subprocess
from collections import Counter
from pathlib import Path, PurePosixPath

MAX_REPORT_BYTES = 64 * 1024 * 1024
SOURCE_SUFFIXES = frozenset(
    {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".py", ".js", ".cjs", ".yml", ".yaml"}
)


# Purpose: Keep reported locations relative, unambiguous, and safe to publish.
# Inputs: One scanner or Git path; no filesystem content is opened.
# Outputs: Canonical repository-relative spelling; malformed paths raise ValueError.
def relative_path(value: object) -> str:
    if not isinstance(value, str) or not value or any(ord(char) < 32 for char in value):
        raise ValueError("scan path is missing or contains control characters")
    value = value.replace("\\", "/")
    while value.startswith("./"):
        value = value[2:]
    parts = value.split("/")
    if not value or value.startswith("/") or ":" in value or any(part in {"", ".", ".."} for part in parts):
        raise ValueError("scan path is not repository relative")
    return value


# Purpose: Inventory every tracked path, including tests, vendor files, and skills.
# Inputs: Repository root; Git must be available and return its complete NUL-delimited index.
# Outputs: Sorted unique paths; Git, encoding, size, and duplicate failures are fatal.
def tracked_paths(root: Path) -> list[str]:
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "--cached", "-z"],
        check=True,
        capture_output=True,
        timeout=30,
    )
    if len(result.stdout) > 8 * 1024 * 1024 or (result.stdout and not result.stdout.endswith(b"\0")):
        raise ValueError("Git inventory is oversized or incomplete")
    paths = [relative_path(value) for value in result.stdout.decode("utf-8").split("\0") if value]
    if not paths or len(paths) != len(set(paths)):
        raise ValueError("Git inventory is empty or contains duplicate index entries")
    return sorted(paths)


# Purpose: Read bounded structured evidence, never execute or interpret source snippets.
# Inputs: UTF-8 Semgrep JSON output from the same scan that produced the uploaded SARIF.
# Outputs: A JSON object; missing, truncated, oversized, or malformed reports fail closed.
def read_report(path: Path) -> dict:
    with path.open("rb") as stream:
        payload = stream.read(MAX_REPORT_BYTES + 1)
    if len(payload) > MAX_REPORT_BYTES:
        raise ValueError("Semgrep coverage report exceeds its size budget")
    report = json.loads(payload)
    if not isinstance(report, dict):
        raise ValueError("Semgrep coverage report is not an object")
    return report


# Purpose: Separate inventory, scanner admission, diagnostics, and actual coverage gaps.
# Inputs: Complete tracked paths and structured Semgrep output; all paths are relative.
# Outputs: Metadata-only evidence without source snippets, matches, or diagnostic messages.
def coverage_report(tracked: list[str], document: dict) -> dict:
    paths = document.get("paths")
    if not isinstance(paths, dict):
        raise ValueError("Semgrep output lacks its paths object")
    scanned = paths.get("scanned")
    errors = document.get("errors")
    version = document.get("version")
    if not isinstance(scanned, list) or not isinstance(errors, list) or not isinstance(version, str) or not version:
        raise ValueError("Semgrep output lacks required paths, errors, or version evidence")
    normalized = [relative_path(value) for value in scanned]
    if len(normalized) != len(set(normalized)):
        raise ValueError("Semgrep reported duplicate scanned paths")
    tracked_set = set(tracked)
    admitted = set(normalized) & tracked_set
    missing = sorted(tracked_set - admitted)
    diagnostic_counts: Counter[str] = Counter()
    diagnostic_paths: Counter[str] = Counter()
    unlocated = 0
    for error in errors:
        if not isinstance(error, dict):
            raise ValueError("Semgrep diagnostic metadata is malformed")
        kind = error.get("type")
        # Semgrep serializes some variants as [tag, payload], notably PartialParsing.
        if isinstance(kind, list) and len(kind) == 2:
            kind = kind[0]
        if not isinstance(kind, str) or not kind:
            raise ValueError("Semgrep diagnostic type is missing or malformed")
        diagnostic_counts[kind] += 1
        path = error.get("path")
        if path is not None:
            diagnostic_paths[relative_path(path)] += 1
        else:
            unlocated += 1
    source_gaps = [path for path in missing if PurePosixPath(path).suffix.lower() in SOURCE_SUFFIXES]
    return {
        "schema_version": 1,
        "scanner": "Semgrep",
        "scanner_version": version,
        "coverage_meaning": "Admission is not successful parsing, rule coverage, or proof of safety.",
        "tracked_files": sorted(tracked_set),
        "scanned_tracked_files": sorted(admitted),
        "not_scanned_tracked_files": missing,
        "unscanned_source_files": source_gaps,
        "untracked_scan_path_count": len(set(normalized) - tracked_set),
        "diagnostic_count": len(errors),
        "diagnostics_by_type": dict(sorted(diagnostic_counts.items())),
        "diagnostics_by_path": dict(sorted(diagnostic_paths.items())),
        "unlocated_diagnostic_count": unlocated,
    }


# Purpose: Publish auditable metadata and reject omitted supported-language source paths.
# Inputs: Repository root and the scanner's same-run JSON report via command-line options.
# Outputs: JSON on stdout; nonzero status for missing source, invalid evidence, or Git failure.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    report = coverage_report(tracked_paths(args.root), read_report(args.report))
    print(json.dumps(report, indent=2, ensure_ascii=True))
    return 1 if report["unscanned_source_files"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
