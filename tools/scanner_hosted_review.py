"""Apply individually approved hosted dispositions without hiding raw findings."""

from __future__ import annotations

import csv
import hashlib
import io
import os
import re
import subprocess
from collections import Counter
from pathlib import Path

from tools.native_build_provenance import INPUT_FILES, INPUT_ROOTS, canonical, capture_inputs, input_path

POLICY = ".github/scanner-hosted-reviews.csv"
EVIDENCE = "docs/security-remaining-finding-review-2026-10-04.md"
CONFIGURATION = (".github/codeql/codeql-config.yml", ".github/workflows/security-code-scanning.yml")
FIELDS = (
    "alert",
    "tool",
    "version",
    "category",
    "path",
    "rule",
    "input_sha256",
    "startLine",
    "startColumn",
    "endLine",
    "endColumn",
    "context_sha256",
    "evidence",
    "disposition",
)
COUNTS = {
    "cpp/invalid-pointer-deref": 6,
    "cpp/suspicious-pointer-scaling": 20,
    "cpp/stack-address-escape": 16,
    "cpp/loop-variable-changed": 4,
    "cpp/missing-header-guard": 5,
    "cpp/fixme-comment": 1,
    "cpp/unused-static-variable": 1,
    "DS161085": 1,
}
SOURCE = re.compile(
    r"(?:build/zstd-v1\.5\.7-source/zstd-1\.5\.7/lib/(?:common/(?:fse|mem|xxhash)\.h"
    r"|compress/(?:fse_compress|zstd_compress_sequences|zstd_lazy|zstd_opt)\.c"
    r"|compress/zstd_compress_internal\.h|decompress/huf_decompress\.c"
    r"|dictBuilder/(?:cover|fastcover|divsufsort)\.c|legacy/zstd_v0[567]\.c|z(?:std|dict)\.h)"
    r"|build/tests/zstd_legacy_fault_sources/zstd_v07\.c|tests/zstd/fault_allocator\.c"
    r"|out/tools/cmake-4\.4\.3/cmake-4\.4\.3-windows-x86_64/share/cmake-4\.4/Modules/CMakeCompilerABI\.h)"
)
MAX_POLICY = 64 * 1024
DERIVED_ROOT = "build/zstd-v1.5.7-source/zstd-1.5.7/lib"
APPROVAL = ".github/scanner-hosted-approval.csv"


# Purpose: Seal every approved incident field independently from a renewable caller-context receipt.
# Inputs: Ordered individual records. Outputs: Canonical digest; changing any decision identity changes this seal.
def approval_digest(rows: list[dict[str, str]]) -> str:
    identities = [{key: value for key, value in row.items() if key != "context_sha256"} for row in rows]
    return hashlib.sha256(canonical(identities)).hexdigest()


# Purpose: Read the public integrity pin as bounded provenance metadata rather than a source-code credential literal.
# Inputs: Checkout approval record. Outputs: Canonical incident digest; malformed/redirected metadata fails closed.
def read_approval_digest(root: Path) -> str:
    with input_path(root, APPROVAL).open("rb") as stream:
        payload = stream.read(1025)
    if len(payload) > 1024:
        raise ValueError("Hosted approval record exceeds its byte budget")
    reader = csv.DictReader(io.StringIO(payload.decode("utf-8")))
    rows = list(reader)
    if tuple(reader.fieldnames or ()) != ("scope", "records_sha256", "evidence") or len(rows) != 1:
        raise ValueError("Hosted approval record has an unknown schema or inventory")
    row = rows[0]
    if (
        set(row) != set(reader.fieldnames)
        or row["scope"] != "hosted-individual-identities-v1"
        or row["evidence"] != EVIDENCE
    ):
        raise ValueError("Hosted approval record differs from its individual review")
    if re.fullmatch(r"[0-9a-f]{64}", row["records_sha256"] or "") is None:
        raise ValueError("Hosted approval record has an invalid incident digest")
    return row["records_sha256"]


# Purpose: Hash a bounded regular reviewed source without accepting redirects or concurrent mutation.
# Inputs: Checkout and validated relative filename. Outputs: LF-normalized digest or fail-closed error.
def source_digest(root: Path, name: str) -> str:
    path = input_path(root, name)
    before = path.stat()
    if before.st_size > 2 * 1024**2:
        raise ValueError("Hosted reviewed source exceeds its byte budget")
    payload = path.read_bytes()
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError("Hosted reviewed source changed while hashing")
    return hashlib.sha256(payload.replace(b"\r\n", b"\n")).hexdigest()


# Purpose: Include unreported generated callers so edited cached sources cannot inherit an approved sink verdict.
# Inputs: Canonical generated Zstandard library tree. Outputs: Bounded complete source map; links fail closed.
def derived_sources(root: Path) -> dict[str, str]:
    input_path(root, DERIVED_ROOT + "/common/mem.h")
    sources = {}
    for directory, children, files in os.walk(root / DERIVED_ROOT, followlinks=False):
        for name in children:
            path = Path(directory) / name
            if path.is_symlink() or path.is_junction():
                raise ValueError("Derived review context crosses a redirected directory")
        for name in files:
            if Path(name).suffix.lower() not in (".c", ".h", ".cpp", ".inc", ".s", ".asm"):
                continue
            if len(sources) >= 512:
                raise ValueError("Derived review context exceeds its source inventory budget")
            relative = (Path(directory) / name).relative_to(root).as_posix()
            sources[relative] = source_digest(root, relative)
    return sources


# Purpose: Bind dispositions to native callers/recipes, generated callers and complete analysis configuration.
# Inputs: Checkout, including uncommitted/untracked native changes. Outputs: Deterministic context identity.
def context_digest(root: Path) -> str:
    value = {
        "native_inputs_sha256": capture_inputs(root)["inputs_sha256"],
        "derived_sources": derived_sources(root),
        "analysis_configuration": {name: source_digest(root, name) for name in CONFIGURATION},
    }
    return hashlib.sha256(canonical(value)).hexdigest()


# Purpose: Load only the complete, individually approved 54-record ledger; no family or path exemptions.
# Inputs: Checkout policy file. Outputs: Validated records; unknown schema, expansion or malformed identity fails.
def read_hosted_policy(root: Path) -> list[dict[str, str]]:
    path = input_path(root, POLICY)
    with path.open("rb") as stream:
        payload = stream.read(MAX_POLICY + 1)
    if len(payload) > MAX_POLICY:
        raise ValueError("Hosted review ledger exceeds its byte budget")
    reader = csv.DictReader(io.StringIO(payload.decode("utf-8")))
    if tuple(reader.fieldnames or ()) != FIELDS:
        raise ValueError("Hosted review ledger has an unknown schema")
    rows, seen, contexts = [], set(), set()
    for row in reader:
        if len(rows) >= 54 or set(row) != set(FIELDS) or any(not value for value in row.values()):
            raise ValueError("Hosted review exceeds the individually approved records")
        if row["rule"] not in COUNTS or SOURCE.fullmatch(row["path"]) is None or row["evidence"] != EVIDENCE:
            raise ValueError("Hosted review rule, source or evidence exceeds approval")
        codeql = row["rule"].startswith("cpp/")
        producer = ("CodeQL", "2.27.1", "/language:c-cpp") if codeql else ("devskim", "1.0.100+ea92e6f3cc", "devskim")
        if tuple(row[key] for key in ("tool", "version", "category")) != producer:
            raise ValueError("Hosted review has an unapproved producer or analysis category")
        if not codeql and row["path"] != "tests/zstd/fault_allocator.c":
            raise ValueError("Hosted allocation review is limited to the approved serial fixture")
        if row["disposition"] not in ("false-positive", "justified-quality-observation"):
            raise ValueError("Hosted review requires an explicit approved decision")
        if any(re.fullmatch(r"[0-9a-f]{64}", row[key]) is None for key in ("input_sha256", "context_sha256")):
            raise ValueError("Hosted review requires exact source and caller-context digests")
        for key in ("alert", *FIELDS[7:11]):
            if re.fullmatch(r"[1-9][0-9]{0,6}", row[key]) is None:
                raise ValueError("Hosted review has an invalid alert or region identity")
        if (int(row["endLine"]), int(row["endColumn"])) < (int(row["startLine"]), int(row["startColumn"])):
            raise ValueError("Hosted review has a reversed region")
        if row["alert"] in seen:
            raise ValueError("Hosted review duplicates an alert identity")
        seen.add(row["alert"])
        contexts.add(row["context_sha256"])
        rows.append(row)
    if Counter(row["rule"] for row in rows) != COUNTS or len(contexts) != 1:
        raise ValueError("Hosted review is not the complete approved per-rule inventory and context")
    if approval_digest(rows) != read_approval_digest(root):
        raise ValueError("Hosted review differs from the maintainer-approved individual identities")
    return rows


# Purpose: Require the analyzed commit to contain the same native and analysis inputs as the reviewed checkout.
# Inputs: Full SHA and fixed input projection. Outputs: True for identical Git inputs; tool failures raise.
def committed_context_matches(root: Path, commit: str) -> bool:
    result = subprocess.run(
        [
            "git",
            "-c",
            "core.fsmonitor=false",
            "diff",
            "--quiet",
            commit,
            "--",
            *INPUT_ROOTS,
            *INPUT_FILES,
            *CONFIGURATION,
        ],
        cwd=root,
        timeout=30,
    )
    if result.returncode not in (0, 1):
        raise ValueError("Published hosted-review context cannot be verified")
    return result.returncode == 0


# Purpose: Admit exact approved hosted operations only while their source, caller and analysis context remain valid.
# Inputs: Complete raw alerts, full analysis commit and approved ledger. Outputs: IDs only; never mutates reports.
def review_hosted_alerts(root: Path, alerts: list[dict], commit: str, rows: list[dict[str, str]]) -> list[int]:
    if re.fullmatch(r"[0-9a-f]{40}", commit) is None or len(alerts) > 20000:
        raise ValueError("Hosted review requires a full analysis commit and bounded inventory")
    if context_digest(root) != rows[0]["context_sha256"] or not committed_context_matches(root, commit):
        return []
    reviews = {int(row["alert"]): row for row in rows}
    digests, accepted = {}, []
    for alert in alerts:
        number = alert.get("number")
        row = reviews.get(number) if type(number) is int else None
        if row is None or alert.get("state") != "open":
            continue
        tool, instance = alert.get("tool", {}), alert.get("most_recent_instance", {})
        location = instance.get("location", {})
        if (tool.get("name"), tool.get("version"), instance.get("category")) != tuple(
            row[key] for key in ("tool", "version", "category")
        ):
            continue
        if instance.get("commit_sha") != commit or alert.get("rule", {}).get("id") != row["rule"]:
            continue
        if location.get("path") != row["path"]:
            continue
        region_keys = ("start_line", "start_column", "end_line", "end_column")
        if any(
            type(location.get(key)) is not int or location[key] != int(row[field])
            for key, field in zip(region_keys, FIELDS[7:11], strict=True)
        ):
            continue
        if row["path"] not in digests:
            digests[row["path"]] = source_digest(root, row["path"])
        if digests[row["path"]] == row["input_sha256"]:
            accepted.append(number)
    if len(set(accepted)) != len(accepted):
        raise ValueError("Hosted review contains duplicate alert identities")
    return accepted
