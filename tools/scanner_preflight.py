"""Scan changed publication inputs with the pinned detectors before committing or pushing."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import uuid
from pathlib import Path, PurePosixPath

from tools.devskim_scope import validate_report_directory, write_options
from tools.scanner_metadata_review import read_policy, review_findings

ROOT = Path(__file__).resolve().parents[1]
MAX_BYTES = 64 * 1024 * 1024
MAX_FILES = 4096


# Purpose: Select complete changed-file inputs without guessing paths or silently excluding new files.
# Inputs: Checkout and base ref. Outputs: Sorted existing tracked/untracked relative paths; Git failures raise.
def changed_paths(root: Path, base: str) -> list[str]:
    revision = (
        subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "--verify", "--end-of-options", base + "^{commit}"],
            timeout=30,
            stderr=subprocess.PIPE,
        )
        .decode("ascii")
        .strip()
    )
    paths = set()
    for args in (
        ["diff", "--name-only", "--diff-filter=ACMR", "-z", revision, "--"],
        ["ls-files", "--others", "--exclude-standard", "-z"],
    ):
        payload = subprocess.check_output(["git", "-C", str(root), *args], timeout=30)
        paths.update(name.decode("utf-8") for name in payload.split(b"\0") if name)
    if len(paths) > MAX_FILES:
        raise ValueError("Changed input count exceeds scanner admission; split or explicitly review the batch")
    return sorted(paths)


# Purpose: Freeze exact publication bytes inside a bounded owned directory, refusing redirected source paths.
# Inputs: Checkout-relative paths and a new snapshot directory.
# Outputs: Path-to-SHA-256 map; unsafe/oversize inputs fail.
def snapshot_inputs(root: Path, paths: list[str], destination: Path) -> dict[str, str]:
    hashes = {}
    total = 0
    for name in paths:
        relative = PurePosixPath(name)
        if "\\" in name or relative.is_absolute() or any(part in (".", "..") or ":" in part for part in relative.parts):
            raise ValueError("Invalid repository-relative scanner input")
        source = root
        for part in relative.parts:
            source /= part
            if source.is_symlink() or source.is_junction():
                raise ValueError("Scanner input crosses a reparse point")
        if not source.is_file():
            raise ValueError("Changed scanner input is not a regular file")
        with source.open("rb") as stream:
            payload = stream.read(MAX_BYTES - total + 1)
        total += len(payload)
        if total > MAX_BYTES:
            raise ValueError("Changed input bytes exceed scanner admission; split or explicitly review the batch")
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as stream:
            stream.write(payload)
        hashes[name] = hashlib.sha256(payload).hexdigest()
    return hashes


# Purpose: Bound report decoding and reject absent, ambiguous or malformed detector output.
# Inputs: Raw report path and detector identity. Outputs: Finding count; invalid reports fail admission.
def finding_count(path: Path, tool: str) -> int:
    with path.open("rb") as stream:
        payload = stream.read(MAX_BYTES + 1)
    if len(payload) > MAX_BYTES:
        raise ValueError("Scanner report exceeds the decoding budget")
    report = json.loads(payload, object_pairs_hook=unique_fields, parse_constant=reject_constant)
    if tool == "gitleaks" and isinstance(report, list):
        return len(report)
    if tool == "devskim" and isinstance(report, dict) and report.get("version") == "2.1.0":
        runs = report.get("runs")
        if not isinstance(runs, list) or not runs:
            raise ValueError("DevSkim report has no scanner runs")
        for run in runs:
            if run["tool"]["driver"]["name"] != "devskim" or not isinstance(run.get("results"), list):
                raise ValueError("DevSkim report has an unexpected producer or result shape")
        return sum(len(run["results"]) for run in runs)
    raise ValueError("Unexpected scanner report shape")


# Purpose: Execute actual detectors with process-tree, time, memory and output containment.
# Inputs: Explicit tool argv and scanner budget. Outputs: Bounded result; failed invocation never qualifies as clean.
def run_scanner(argv: list[str], timeout: int = 90, directory: Path | None = None) -> dict:
    spec = importlib.util.spec_from_file_location("superzip_mcp", ROOT / "mcp/superzip_mcp.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module.run_bounded_command(
        argv, timeout_seconds=timeout, max_output_bytes=2_000_000, working_directory=directory
    )


# Purpose: Refuse ambiguous report fields. Inputs: Decoded object pairs. Outputs: Unique field mapping or refusal.
def unique_fields(pairs: list[tuple]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Scanner report contains duplicate fields")
        result[key] = value
    return result


# Purpose: Reject non-JSON numeric constants. Inputs: Decoder token. Outputs: Always raises without exposing input.
def reject_constant(token: str) -> None:
    raise ValueError("Scanner report contains a non-JSON numeric constant")


# Purpose: Reject absent/mismatched detector versions before scanning publication bytes.
# Inputs: Explicit executable paths. Outputs: Version evidence or a value-free actionable failure.
def require_versions(gitleaks: str, devskim: str) -> dict:
    workflow = (ROOT / ".github/workflows/security-code-scanning.yml").read_text(encoding="utf-8")
    versions = set(re.findall(r"ghcr\.io/gitleaks/gitleaks:v([0-9.]+)@sha256:", workflow))
    if len(versions) != 1:
        raise ValueError("Gitleaks workflow version pin is ambiguous")
    expected = {
        "gitleaks": versions.pop(),
        "devskim": json.loads((ROOT / ".github/requirements/devskim-packaging.json").read_text(encoding="utf-8"))[
            "version"
        ],
    }
    observed = {}
    for name, executable, argument in (("gitleaks", gitleaks, "version"), ("devskim", devskim, "--version")):
        result = run_scanner([executable, argument], 15)
        output = result.get("stdout", "") + result.get("stderr", "")
        if result["exit_code"] != 0 or result["timed_out"] or result["output_limit_exceeded"]:
            raise ValueError("Pinned scanner version probe failed")
        if re.search(r"(?<![0-9.])" + re.escape(expected[name]) + r"(?![0-9.])", output) is None:
            raise ValueError("Scanner differs from the repository pin; provision the complete pinned tool")
        observed[name] = {
            "version": expected[name],
            "binary_sha256": hashlib.sha256(Path(executable).read_bytes()).hexdigest(),
        }
    return observed


# Purpose: Fail publication admission on detector findings, tool failures, or source changes during the check.
# Inputs: Base ref and pinned detector paths; no credentials are accepted or logged.
# Outputs: Private reports and exit status.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="HEAD")
    parser.add_argument("--gitleaks", default=os.environ.get("SUPERZIP_GITLEAKS") or shutil.which("gitleaks"))
    parser.add_argument("--devskim", default=os.environ.get("SUPERZIP_DEVSKIM") or shutil.which("devskim"))
    args = parser.parse_args()
    paths = changed_paths(ROOT, args.base)
    validate_report_directory(ROOT)
    if not paths:
        print("Scanner preflight: no changed publication inputs; no scan qualification claimed.")
        return 0
    if not args.gitleaks or not args.devskim:
        raise ValueError(
            "Pinned detectors unavailable: pass --gitleaks/--devskim or set process-scoped "
            "SUPERZIP_GITLEAKS/SUPERZIP_DEVSKIM after complete-tool provisioning"
        )
    args.gitleaks = str(Path(args.gitleaks).resolve())
    args.devskim = str(Path(args.devskim).resolve())
    tools = require_versions(args.gitleaks, args.devskim)
    directory = ROOT / "out/scanner-preflight" / uuid.uuid4().hex
    parent = ROOT
    for part in directory.relative_to(ROOT).parts:
        parent /= part
        if parent.is_symlink() or parent.is_junction():
            raise ValueError("Scanner output crosses a reparse point")
    directory.mkdir(parents=True)
    snapshot = directory / "inputs"
    hashes = snapshot_inputs(ROOT, paths, snapshot)
    review_policy = read_policy(ROOT)
    gitleaks_report, devskim_report = directory / "gitleaks.json", directory / "devskim.sarif"
    devskim_options = directory / "devskim-options.json"
    write_options(snapshot, devskim_options)
    commands = {
        "gitleaks": [
            args.gitleaks,
            "dir",
            ".",
            "--config",
            str(ROOT / ".github/gitleaks.toml"),
            "--redact",
            "--no-banner",
            "--no-color",
            "--timeout",
            "60",
            "--report-format",
            "json",
            "--report-path",
            str(gitleaks_report),
        ],
        "devskim": [
            args.devskim,
            "analyze",
            "-I",
            str(snapshot),
            "--base-path",
            str(snapshot),
            "-O",
            str(devskim_report),
            "-f",
            "sarif",
            "--skip-excerpts",
            "--disable-supression",
            "--disable-console",
            "--options-json",
            str(devskim_options),
        ],
    }
    status = {name: run_scanner(command, directory=snapshot) for name, command in commands.items()}
    counts = {
        "gitleaks": finding_count(gitleaks_report, "gitleaks"),
        "devskim": finding_count(devskim_report, "devskim"),
    }
    review = review_findings(snapshot, devskim_report, review_policy)
    unchanged = all(hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == digest for name, digest in hashes.items())
    unchanged = unchanged and changed_paths(ROOT, args.base) == paths
    unchanged = unchanged and read_policy(ROOT) == review_policy
    passed = (
        unchanged
        and counts["gitleaks"] == 0
        and review["unresolved_count"] == 0
        and all(
            result["exit_code"] == 0 and not result["timed_out"] and not result["output_limit_exceeded"]
            for result in status.values()
        )
    )
    receipt = {
        "passed": passed,
        "input_hashes": hashes,
        "tools": tools,
        "finding_counts": counts,
        "devskim_review": review,
        "review_policy_sha256": hashlib.sha256(review_policy).hexdigest(),
        "source_unchanged": unchanged,
        "devskim_passive_reports": validate_report_directory(snapshot),
        "results": status,
        "scope": "changed_files_only_not_full_repository_security_acceptance",
    }
    (directory / "receipt.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
    print(
        json.dumps(
            {
                "passed": passed,
                "files": len(paths),
                "finding_counts": counts,
                "reviewed_metadata": len(review["reviewed_metadata"]),
                "unresolved_devskim": review["unresolved_count"],
                "receipt": str(directory / "receipt.json"),
            }
        )
    )
    return 0 if passed else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.SubprocessError, KeyError) as error:
        print(f"Scanner preflight failed: {error}", file=sys.stderr)
        raise SystemExit(1) from None
