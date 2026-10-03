"""Retain scanner findings while recognizing exact, provenance-reviewed metadata snapshots."""

from __future__ import annotations

import csv
import hashlib
import io
import json
import re
from pathlib import Path

POLICY = Path(".github/scanner-metadata-reviews.csv")
FIELDS = ("path", "rule", "input_sha256", "public_sha256", "evidence")
MAX_POLICY_BYTES = 64 * 1024
MAX_REVIEWS = 256
METADATA_PATH = re.compile(r"(?:\.github/gitleaks\.toml|docs/benchmarks/corpora/[^/]+\.json)")


# Purpose: Freeze bounded review policy bytes without following filesystem redirects.
# Inputs: Checkout root. Outputs: Validated CSV bytes; reparse points, missing or malformed policy fail.
def read_policy(root: Path) -> bytes:
    path = root
    for part in POLICY.parts:
        path /= part
        if path.is_symlink() or path.is_junction():
            raise ValueError("Scanner metadata review policy crosses a reparse point")
    with path.open("rb") as stream:
        payload = stream.read(MAX_POLICY_BYTES + 1)
    read_reviews(payload)
    return payload


# Purpose: Admit only exact reviewed public-checksum records, never source/test code or general suppressions.
# Inputs: Bounded version-controlled CSV bytes. Outputs: Validated records; unknown fields, roles and values fail.
def read_reviews(payload: bytes) -> list[dict[str, str]]:
    if len(payload) > MAX_POLICY_BYTES:
        raise ValueError("Scanner metadata review policy exceeds its byte budget")
    reader = csv.DictReader(io.StringIO(payload.decode("utf-8")))
    if tuple(reader.fieldnames or ()) != FIELDS:
        raise ValueError("Scanner metadata review fields differ from the explicit policy contract")
    reviews, seen = [], set()
    for row in reader:
        if len(reviews) >= MAX_REVIEWS or set(row) != set(FIELDS) or any(not value for value in row.values()):
            raise ValueError("Scanner metadata review row is malformed or exceeds admission")
        if METADATA_PATH.fullmatch(row["path"]) is None or row["rule"] != "DS173237":
            raise ValueError("Only public-checksum metadata can be reviewed; code and tests remain blocking")
        if any(re.fullmatch(r"[0-9a-f]{64}", row[key]) is None for key in ("input_sha256", "public_sha256")):
            raise ValueError("Scanner metadata review has an invalid digest")
        if row["evidence"] != "docs/security-code-scanning.md#finding-triage":
            raise ValueError("Scanner metadata review lacks the canonical provenance evidence")
        identity = (row["path"], row["rule"], row["public_sha256"])
        if identity in seen:
            raise ValueError("Scanner metadata review identity is duplicated")
        seen.add(identity)
        reviews.append(row)
    return reviews


# Purpose: Distinguish exact reviewed metadata from unresolved findings without mutating or filtering the raw SARIF.
# Inputs: Frozen input root, complete raw report and frozen review policy.
# Outputs: Reviewed identities and unresolved count.
def review_findings(root: Path, report: Path, policy: bytes) -> dict:
    reviews = read_reviews(policy)
    data = json.loads(report.read_text(encoding="utf-8-sig"))
    reviewed, unresolved = [], 0
    for run_index, run in enumerate(data["runs"]):
        for index, finding in enumerate(run["results"]):
            locations = finding.get("locations", [])
            physical = locations[0].get("physicalLocation", {}) if len(locations) == 1 else {}
            name = physical.get("artifactLocation", {}).get("uri", "").replace("\\", "/")
            candidates = [row for row in reviews if row["path"] == name and row["rule"] == finding.get("ruleId")]
            accepted = False
            if candidates:
                payload = (root / name).read_bytes()
                digest = hashlib.sha256(payload.replace(b"\r\n", b"\n")).hexdigest()
                region = physical.get("region", {})
                offset, length = region.get("charOffset"), region.get("charLength")
                if type(offset) is int and offset >= 0 and type(length) is int and length == 66:
                    units = payload.decode("utf-8").encode("utf-16-le")
                    value = units[offset * 2 : (offset + length) * 2].decode("utf-16-le")
                    accepted = any(
                        digest == row["input_sha256"] and value == '"' + row["public_sha256"] + '"'
                        for row in candidates
                    )
            if accepted:
                reviewed.append(
                    {
                        "path": name,
                        "rule": finding["ruleId"],
                        "run_index": run_index,
                        "result_index": index,
                        "input_sha256": digest,
                        "evidence": candidates[0]["evidence"],
                    }
                )
            else:
                unresolved += 1
    return {"reviewed_metadata": reviewed, "unresolved_count": unresolved}
