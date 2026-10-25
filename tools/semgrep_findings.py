"""Fail on every Semgrep finding except an exact, current individual review."""

from __future__ import annotations

import argparse
import hashlib
import json
from collections import Counter
from pathlib import Path

from tools.scanner_hosted_review import (
    CURRENT_POLICY,
    committed_review_blob,
    current_context_matches,
    read_current_reviews,
    source_digest,
)
from tools.semgrep_coverage import read_report, relative_path


# Purpose: Decode one native Semgrep JSON finding without accepting an ambiguous location.
# Inputs: One result object. Outputs: Rule, contained path and exact one-based range.
def json_finding_key(result: object) -> tuple[str, str, int, int, int, int]:
    if not isinstance(result, dict) or not isinstance(result.get("check_id"), str) or not result["check_id"]:
        raise ValueError("Semgrep JSON finding lacks a rule")
    start, end = result.get("start"), result.get("end")
    if not isinstance(start, dict) or not isinstance(end, dict):
        raise ValueError("Semgrep JSON finding lacks a complete range")
    coordinates = tuple(point.get(field) for point in (start, end) for field in ("line", "col"))
    if any(type(value) is not int or value < 1 for value in coordinates) or coordinates[:2] > coordinates[2:]:
        raise ValueError("Semgrep JSON finding has an invalid range")
    return result["check_id"], relative_path(result.get("path")), *coordinates


# Purpose: Decode the corresponding SARIF result that GitHub publishes.
# Inputs: One Semgrep SARIF result. Outputs: Rule, contained path and exact one-based range.
def sarif_finding_key(result: object) -> tuple[str, str, int, int, int, int]:
    if not isinstance(result, dict) or not isinstance(result.get("ruleId"), str) or not result["ruleId"]:
        raise ValueError("Semgrep SARIF finding lacks a rule")
    locations = result.get("locations")
    if not isinstance(locations, list) or len(locations) != 1 or not isinstance(locations[0], dict):
        raise ValueError("Semgrep SARIF finding lacks one location")
    physical = locations[0].get("physicalLocation")
    if not isinstance(physical, dict):
        raise ValueError("Semgrep SARIF finding lacks a physical location")
    artifact, region = physical.get("artifactLocation"), physical.get("region")
    if not isinstance(artifact, dict) or artifact.get("uriBaseId") != "%SRCROOT%" or not isinstance(region, dict):
        raise ValueError("Semgrep SARIF finding lacks a repository-relative range")
    coordinates = tuple(region.get(field) for field in ("startLine", "startColumn", "endLine", "endColumn"))
    if any(type(value) is not int or value < 1 for value in coordinates) or coordinates[:2] > coordinates[2:]:
        raise ValueError("Semgrep SARIF finding has an invalid range")
    return result["ruleId"], relative_path(artifact.get("uri")), *coordinates


# Purpose: Admit only committed, individually reviewed findings from a complete same-run scan.
# Inputs: Exact checkout/commit/repository, JSON and SARIF documents, scanner exit status.
# Outputs: Reviewed alert IDs; any new finding, report divergence or stale source raises ValueError.
def admit_scan(root: Path, commit: str, repository: str, report: dict, sarif: dict, scanner_exit: int) -> list[int]:
    results = report.get("results")
    runs = sarif.get("runs")
    version = report.get("version")
    if (
        not isinstance(repository, str)
        or not repository
        or not isinstance(version, str)
        or not version
        or not isinstance(results, list)
        or len(results) > 10000
        or sarif.get("version") != "2.1.0"
        or not isinstance(runs, list)
        or len(runs) != 1
        or not isinstance(runs[0], dict)
    ):
        raise ValueError("Semgrep scan identity or bounded result inventory is missing")
    driver = runs[0].get("tool", {}).get("driver", {})
    published = runs[0].get("results")
    if (
        not isinstance(driver, dict)
        or (driver.get("name"), driver.get("semanticVersion")) != ("Semgrep OSS", version)
        or not isinstance(published, list)
        or len(published) != len(results)
    ):
        raise ValueError("Semgrep JSON and SARIF scanner identities or counts differ")
    json_keys = [json_finding_key(item) for item in results]
    sarif_keys = [sarif_finding_key(item) for item in published]
    if Counter(json_keys) != Counter(sarif_keys) or len(set(sarif_keys)) != len(sarif_keys):
        raise ValueError("Semgrep JSON and SARIF findings differ or duplicate a location")
    if type(scanner_exit) is not int or scanner_exit != (1 if sarif_keys else 0):
        raise ValueError("Semgrep scanner exit does not match its complete findings")

    policy = committed_review_blob(root, commit, CURRENT_POLICY)
    if source_digest(root, CURRENT_POLICY) != hashlib.sha256(policy.replace(b"\r\n", b"\n")).hexdigest():
        raise ValueError("Semgrep review ledger differs from the audited commit")
    rows = [
        row
        for row in read_current_reviews(policy)
        if (row["repository"], row["tool"], row["version"], row["category"])
        == (repository, "Semgrep OSS", version, "semgrep")
    ]
    cache: dict[str, bytes] = {}

    # Purpose: Read each committed context blob once, preserving Git's regular-file check.
    # Inputs: Validated ledger path. Outputs: Immutable bytes from the scanned revision.
    def blob(name: str) -> bytes:
        if name not in cache:
            cache[name] = committed_review_blob(root, commit, name)
        return cache[name]

    admitted = []
    for rule, path, start_line, start_column, end_line, end_column in sarif_keys:
        matching = [
            row
            for row in rows
            if (
                row["rule"],
                row["path"],
                *(int(row[key]) for key in ("start_line", "start_column", "end_line", "end_column")),
            )
            == (rule, path, start_line, start_column, end_line, end_column)
        ]
        if len(matching) != 1:
            raise ValueError(f"Semgrep finding has no unique exact review: {rule} at {path}:{start_line}")
        row = matching[0]
        if not current_context_matches(row, blob) or any(
            source_digest(root, name) != hashlib.sha256(blob(name).replace(b"\r\n", b"\n")).hexdigest()
            for name in row["context_paths"].split("|")
        ):
            raise ValueError(f"Semgrep review has stale source or scanner context: {row['alert']}")
        admitted.append(int(row["alert"]))
    return sorted(admitted)


# Purpose: Run the exact-source gate over the two raw outputs of one Semgrep process.
# Inputs: Paths to bounded reports, full commit/repository and preserved scanner exit.
# Outputs: Metadata-only receipt or a nonzero status with no raw source excerpt.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--json-report", type=Path, required=True)
    parser.add_argument("--sarif-report", type=Path, required=True)
    parser.add_argument("--scanner-exit", type=int, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--repository", required=True)
    args = parser.parse_args()
    report, sarif = read_report(args.json_report), read_report(args.sarif_report)
    admitted = admit_scan(args.root, args.commit, args.repository, report, sarif, args.scanner_exit)
    print(json.dumps({"scanner": "Semgrep OSS", "findings": len(report["results"]), "reviewed_alerts": admitted}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
