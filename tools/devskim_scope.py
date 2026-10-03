"""Keep passive benchmark results separate from executable scanner inputs."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from tools.devskim_provenance import unique_fields
from tools.devskim_report import reject_constant

REPORT_DIRECTORY = Path("docs/benchmarks/data")
MAX_BYTES = 64 * 1024 * 1024
MAX_ENTRIES = 4096
REPORT_KINDS = {"suzip_ram", "archive_application_comparison", "format_size_matrix"}


# Purpose: Enforce the excluded directory's passive report role before any code-analysis exclusion applies.
# Inputs: Checkout or frozen scan root; only bounded regular JSON report files are admitted.
# Outputs: Report paths; code, test files, acquisition configuration, redirects and malformed records raise.
def validate_report_directory(root: Path) -> list[str]:
    directory = root / REPORT_DIRECTORY
    current = root
    for part in REPORT_DIRECTORY.parts:
        current /= part
        if current.is_symlink() or current.is_junction():
            raise ValueError("Benchmark report directory crosses a reparse point")
    if not directory.exists():
        return []
    pending, paths, total, entries = [directory], [], 0, 0
    while pending:
        for path in pending.pop().iterdir():
            entries += 1
            if entries > MAX_ENTRIES or path.is_symlink() or path.is_junction():
                raise ValueError("Benchmark report directory exceeds admission or contains a redirect")
            if path.is_dir():
                pending.append(path)
                continue
            if not path.is_file() or path.suffix != ".json":
                raise ValueError(
                    "Benchmark report directory contains a non-report file; code and tests must stay scanned"
                )
            with path.open("rb") as stream:
                payload = stream.read(MAX_BYTES - total + 1)
            total += len(payload)
            if total > MAX_BYTES:
                raise ValueError("Benchmark report directory exceeds its byte budget")
            report = json.loads(payload, object_pairs_hook=unique_fields, parse_constant=reject_constant)
            if not isinstance(report, dict) or type(report.get("schema_version")) is not int:
                raise ValueError("Benchmark report has no recognized schema")
            regular = (
                isinstance(report.get("benchmark_kind"), str)
                and report["benchmark_kind"] in REPORT_KINDS
                and isinstance(report.get("runs", report.get("cases")), list)
            )
            diagnostic = report.get("record_kind") == "cooperative_crc_engineering_diagnostic" and all(
                isinstance(report.get(key), list) for key in ("candidate_sweeps", "paired_studies")
            )
            if report["schema_version"] not in (1, 3) or not (regular or diagnostic):
                raise ValueError(
                    "Benchmark report role is unrecognized; configuration and test fixtures must stay scanned"
                )
            if any(key in report for key in ("download_url", "source_url", "member", "windows")):
                raise ValueError("Corpus acquisition configuration must stay outside the report directory")
            paths.append(path.relative_to(root).as_posix())
    return sorted(paths)


# Purpose: Produce one shared DevSkim scope for local and hosted scanning without date-specific paths.
# Inputs: Existing scan root whose passive report role has been validated.
# Outputs: Scanner ignore globs; all source, tests, manifests and configuration remain in scope.
def ignore_globs(root: Path) -> list[str]:
    validate_report_directory(root)
    # The pinned Glob 1.x matcher has no general literal escape. Its single-character
    # wildcard cannot cross separators. Only the already selected scan-root prefix
    # uses placeholders; fixed depth and the exact report suffix preserve scope.
    prefix = "".join("?" if character in "*?![]{,}" else character for character in root.absolute().as_posix())
    return ["**/.git/**", prefix + "/" + REPORT_DIRECTORY.as_posix() + "/**"]


# Purpose: Serialize the shared scope without CLI comma splitting or checkout-path glob injection.
# Inputs: Validated scan root and a new owned output path. Outputs: New JSON options file; collisions fail.
def write_options(root: Path, output: Path) -> None:
    options = {"Globs": ignore_globs(root)}
    with output.open("x", encoding="utf-8") as stream:
        json.dump(options, stream)


# Purpose: Validate the report boundary before hosted analysis starts.
# Inputs: Explicit checkout root and new options path. Outputs: Scanner options file, or a failed process.
def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    write_options(args.root, args.output)


if __name__ == "__main__":
    main()
