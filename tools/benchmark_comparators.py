"""Resolve official stable comparator releases and assess useful timing evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
from datetime import UTC, datetime
from html.parser import HTMLParser
from pathlib import Path
from urllib.request import HTTPRedirectHandler, Request, build_opener

RELEASE_SOURCES = {
    "7-Zip": "https://api.github.com/repos/ip7z/7zip/releases/latest",
    "Zstd": "https://api.github.com/repos/facebook/zstd/releases/latest",
    "Bandizip": "https://www.bandisoft.com/bandizip/history/",
    "WinRAR": "https://www.rarlab.com/download.htm",
    "PeaZip": "https://api.github.com/repos/peazip/PeaZip/releases/latest",
    "NanaZip": "https://api.github.com/repos/M2Team/NanaZip/releases/latest",
    "lzbench": "https://api.github.com/repos/inikep/lzbench/releases/latest",
    "Hyperfine": "https://api.github.com/repos/sharkdp/hyperfine/releases/latest",
}
MAX_RESPONSE_BYTES = 2 * 1024 * 1024
MIN_SAMPLE_SECONDS = 1.0
MIN_SERIES_SECONDS = 10.0
PERMISSIONS_PATH = Path(__file__).with_name("benchmark_permissions.json")


# Purpose: Reject release-endpoint redirects before any second request can occur.
# Inputs: Standard urllib redirect callback arguments from the fixed official endpoint.
# Outputs: Raises ValueError; neither another origin nor a protocol downgrade is contacted.
class NoReleaseRedirects(HTTPRedirectHandler):
    # Purpose: Stop redirects rather than checking the origin only after following one.
    # Inputs: Original request, response metadata and redirect target supplied by urllib.
    # Outputs: Raises ValueError before a redirected request is constructed.
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise ValueError("official release endpoint redirected; review the fixed catalog URL")


# Purpose: Parse official release HTML as text without executing page content.
# Inputs: HTMLParser callbacks for a release-history or download page.
# Outputs: Plain text and anchor labels used only for version discovery.
class ReleasePage(HTMLParser):
    # Purpose: Initialize an isolated release-page parser.
    # Inputs: None; no network or filesystem operations occur here.
    # Outputs: Empty text and anchor collections.
    def __init__(self) -> None:
        super().__init__()
        self.text: list[str] = []
        self.links: list[str] = []
        self.in_anchor = False

    # Purpose: Track whether release text belongs to a hyperlink.
    # Inputs: Parsed tag and attributes; attributes are not executed.
    # Outputs: Updates the anchor-state flag.
    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag == "a":
            self.in_anchor = True

    # Purpose: Finish the current hyperlink label.
    # Inputs: Parsed closing tag.
    # Outputs: Clears the anchor-state flag at a closing anchor.
    def handle_endtag(self, tag: str) -> None:
        if tag == "a":
            self.in_anchor = False

    # Purpose: Collect release labels for bounded version matching.
    # Inputs: Parsed text fragments.
    # Outputs: Appends plain text and hyperlink text.
    def handle_data(self, data: str) -> None:
        self.text.append(data)
        if self.in_anchor:
            self.links.append(data.strip())


# Purpose: Fetch a fixed official endpoint without credentials or unbounded output.
# Inputs: Catalog tool name; arbitrary caller-supplied URLs are not accepted.
# Outputs: UTF-8 source text; network, status, redirect, and size failures stop preflight.
def fetch_release_source(tool: str) -> str:
    uri = RELEASE_SOURCES[tool]
    request = Request(uri, headers={"User-Agent": "SuperZip-benchmark-preflight"})
    with build_opener(NoReleaseRedirects()).open(request, timeout=20) as response:
        payload = response.read(MAX_RESPONSE_BYTES + 1)
    if len(payload) > MAX_RESPONSE_BYTES:
        raise ValueError(f"official release response exceeds the bound: {tool}")
    return payload.decode("utf-8")


# Purpose: Parse the current stable release without silently substituting a beta.
# Inputs: Catalog name and bounded official JSON or HTML source.
# Outputs: Numeric release label; changed or ambiguous source structure raises ValueError.
def parse_release(tool: str, source: str) -> str:
    if RELEASE_SOURCES[tool].startswith("https://api.github.com/"):
        release = json.loads(source)
        if not isinstance(release, dict) or not isinstance(release.get("tag_name"), str):
            raise ValueError(f"official release object is malformed: {tool}")
        version = release["tag_name"].removeprefix("v")
        if release.get("draft") is not False or release.get("prerelease") is not False:
            raise ValueError(f"official latest endpoint returned a non-stable release: {tool}")
    else:
        page = ReleasePage()
        page.feed(source)
        if tool == "Bandizip":
            match = re.search(r"\bv(\d+\.\d+)\b", " ".join(page.text))
        else:
            stable = [label for label in page.links if "WinRAR x64" in label and "beta" not in label.lower()]
            if len(stable) != 1:
                raise ValueError("official WinRAR stable release label is missing or ambiguous")
            match = re.search(r"\b(\d+\.\d+)\b", stable[0])
        version = match[1] if match else ""
    if not re.fullmatch(r"\d+(?:\.\d+){1,3}", version):
        raise ValueError(f"official stable version could not be parsed: {tool}")
    return version


# Purpose: Compare actual executable version text with a freshly fetched official release.
# Inputs: Catalog name, executable banner, and optional already-fetched official source text.
# Outputs: Dated release evidence; stale or unidentified executables raise ValueError.
def require_current_release(tool: str, banner: str, *, release_source: str | None = None) -> dict:
    latest = parse_release(tool, fetch_release_source(tool) if release_source is None else release_source)
    match = re.search(r"(?<![\d.])v?(\d+(?:\.\d+){1,3})(?![\d.])", banner)
    actual = match[1] if match else ""
    release_matches = actual == latest or (tool == "Bandizip" and actual.startswith(latest + "."))
    if not release_matches or re.search(r"\b(beta|alpha|preview|rc\d*)\b", banner, re.IGNORECASE):
        raise ValueError(f"{tool} executable is not the current stable {latest}; update from {RELEASE_SOURCES[tool]}")
    return {
        "version": latest,
        "channel": "stable",
        "checked_at_utc": datetime.now(UTC).isoformat(),
        "source_url": RELEASE_SOURCES[tool],
    }


# Purpose: Require a current, action-specific permission decision before running or publishing.
# Inputs: Catalog subject, action, exact release, and any edition or codec-module scope.
# Outputs: Returns dated permission evidence; unknown, stale, or ineligible scopes fail closed.
def require_permission(
    subject: str,
    action: str,
    *,
    version: str | None = None,
    edition: str | None = None,
    modules: tuple[str, ...] = (),
) -> dict:
    if action not in ("execute", "publish_results", "redistribute"):
        raise ValueError(f"unsupported permission action: {action}")
    catalog = json.loads(PERMISSIONS_PATH.read_text(encoding="utf-8"))
    if catalog.get("schema_version") != 1:
        raise ValueError("unsupported benchmark permission schema")
    reviewed = datetime.strptime(catalog["reviewed_on"], "%Y-%m-%d").date()
    age = (datetime.now(UTC).date() - reviewed).days
    validity = catalog["review_valid_days"]
    if type(validity) is not int or not 1 <= validity <= 30 or not 0 <= age <= validity:
        raise ValueError("benchmark permission review is expired or invalid; review the current terms")
    decision = catalog["subjects"].get(subject)
    if not isinstance(decision, dict):
        raise ValueError(f"unreviewed benchmark subject: {subject}")
    if decision.get(action) != "allowed":
        raise ValueError(f"{subject} {action} is {decision.get(action, 'unreviewed')}: {decision['basis']}")
    if "versions" in decision and version not in decision["versions"]:
        raise ValueError(f"{subject} {version} has not had its license scope reviewed")
    if edition != decision.get("edition"):
        raise ValueError(f"{subject} edition is outside the reviewed scope")
    allowed_modules = decision.get("modules", [])
    if (allowed_modules and not modules) or any(module not in allowed_modules for module in modules):
        raise ValueError(f"{subject} codec modules are outside the reviewed scope")
    return {
        "subject": subject,
        "action": action,
        "reviewed_on": catalog["reviewed_on"],
        "basis": decision["basis"],
        "evidence": decision["evidence"],
        "conditions": decision["conditions"],
    }


# Purpose: Verify official comparator bytes before executing even a version probe.
# Inputs: Reviewed catalog subject and path to the original pinned executable.
# Outputs: Exact SHA-256 evidence; missing, linked, renamed, or unreviewed binaries fail closed.
def require_binary_identity(tool: str, executable: Path) -> dict:
    catalog = json.loads(PERMISSIONS_PATH.read_text(encoding="utf-8"))
    pins = catalog["subjects"].get(tool, {}).get("binaries", {})
    if executable.name not in pins:
        raise ValueError(f"{tool} executable has no reviewed official binary pin")
    hashes = {}
    for filename, expected in pins.items():
        path = executable.parent / filename
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"{tool} pinned binary is missing or linked: {filename}")
        with path.open("rb") as stream:
            actual = hashlib.file_digest(stream, "sha256").hexdigest()
        if actual != expected:
            raise ValueError(f"{tool} official binary SHA-256 mismatch: {filename}")
        hashes[filename] = actual
    return hashes


# Purpose: Separate sustained operation evidence from short-file diagnostics without hiding samples.
# Inputs: Tool results containing compression and extraction sample lists.
# Outputs: Eligibility and explicit reasons; invalid samples raise ValueError.
def timing_eligibility(results: list[dict]) -> dict:
    if not results:
        raise ValueError("timing eligibility requires at least one tool result")
    reasons = []
    for result in results:
        for direction in ("compress_seconds", "extract_seconds"):
            samples = result[direction]
            if (
                not isinstance(samples, list)
                or not samples
                or any(type(value) not in (int, float) or not math.isfinite(value) or value <= 0 for value in samples)
            ):
                raise ValueError("timing eligibility requires finite positive samples")
            if len(samples) < 5 or min(samples) < MIN_SAMPLE_SECONDS or sum(samples) < MIN_SERIES_SECONDS:
                reasons.append(f"{result['tool']} {direction}: workload too short or fewer than five measured runs")
    return {
        "headline_eligible": not reasons,
        "minimum_sample_seconds": MIN_SAMPLE_SECONDS,
        "minimum_series_seconds": MIN_SERIES_SECONDS,
        "reasons": reasons,
    }


# Purpose: Inspect current official versions without installing software or benchmarking.
# Inputs: One or more catalog tool names on the command line.
# Outputs: Dated JSON release metadata on stdout; discovery failures return nonzero.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", action="append", choices=tuple(RELEASE_SOURCES), required=True)
    args = parser.parse_args()
    versions = {
        name: {"version": parse_release(name, fetch_release_source(name)), "source_url": RELEASE_SOURCES[name]}
        for name in dict.fromkeys(args.tool)
    }
    print(
        json.dumps({"checked_at_utc": datetime.now(UTC).isoformat(), "channel": "stable", "tools": versions}, indent=2)
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"comparator preflight failed: {error}", file=sys.stderr)
        sys.exit(1)
