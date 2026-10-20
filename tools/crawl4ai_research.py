"""Bounded local research crawling; remote content is untrusted source material.

This product includes software developed by UncleCode (https://x.com/unclecode)
as part of the Crawl4AI project (https://github.com/unclecode/crawl4ai).
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import importlib.metadata
import json
import time
from datetime import UTC, datetime
from pathlib import Path
from urllib.parse import urlsplit

from tools.crawl4ai_tool import ATTRIBUTION, ROOT, VERSION, text_identity

MAX_MARKDOWN_BYTES = 2 * 1024 * 1024
MAX_CAPTURE_BYTES = 8 * 1024 * 1024
CAPTURE_FIELDS = frozenset(
    {
        "url",
        "html",
        "success",
        "status_code",
        "redirected_url",
        "redirected_status_code",
        "response_headers",
        "error_message",
        "metadata",
        "crawl_stats",
    }
)
SITES = ROOT / "tools/crawl4ai_sites.json"


def crawler():
    """Purpose: Construct the source-admitted upstream crawler. Inputs: Installed package. Outputs: Crawler owner."""
    if importlib.metadata.version("crawl4ai") != VERSION:
        raise ValueError("Crawler version does not match the reviewed integration")
    from crawl4ai import AsyncWebCrawler

    return AsyncWebCrawler()


def inspect_result(result, marker: str = "") -> dict:
    """Purpose: Classify native access/content evidence. Inputs: Result and marker. Outputs: Honest acceptance/cause."""
    markdown = str(result.markdown.raw_markdown) if result.markdown else ""
    content = markdown.encode("utf-8")
    status = int(result.status_code or 0)
    final_status = int(getattr(result, "redirected_status_code", None) or status)
    headers = {key.casefold(): str(value) for key, value in (result.response_headers or {}).items()}
    error = str(result.error_message or "")
    expected = not marker or marker.casefold() in markdown.casefold()
    # Use the provider's access diagnosis and documented response headers, not
    # words in an article that might itself explain anti-bot mechanisms.
    outcomes = (
        (headers.get("x-robots-status") == "Blocked by robots.txt", "robots_denied"),
        (error == "Access denied by robots.txt", "robots_denied"),
        (error.startswith("Blocked by anti-bot protection:"), "access_challenge"),
        (headers.get("cf-mitigated", "").casefold() == "challenge", "access_challenge"),
        (final_status in (401, 403), "access_denied"),
        (final_status == 429, "rate_limited"),
        (bool(final_status) and not 200 <= final_status < 300, "http_error"),
        (not result.success, "native_failure"),
        (not 200 <= final_status < 300, "http_error"),
        (len(content) > MAX_MARKDOWN_BYTES, "content_too_large"),
        (len(content) < 200, "insufficient_content"),
        (not expected, "unexpected_content"),
    )
    category = next((name for failed, name in outcomes if failed), "")
    return {
        "success": not category,
        "native_success": bool(result.success),
        "http_status": status,
        "final_http_status": final_status,
        "markdown_bytes": len(content),
        "markdown_sha256": hashlib.sha256(content).hexdigest(),
        "expected_content": expected,
        "failure_category": category,
        "error": error[:500],
    }


def retain_capture(path: Path, url: str, result, row: dict) -> dict:
    """Purpose: Retain inspectable native evidence. Inputs: New file/result/diagnosis. Outputs: File identity."""
    capture = {
        "schema": 1,
        "requested_url": url,
        "provider_version": importlib.metadata.version("crawl4ai"),
        "diagnosis": row,
        # The public serializer adds Markdown. Limit other fields to the
        # research contract, excluding deprecated properties and binary artifacts.
        "native_result": result.model_dump(mode="json", include=CAPTURE_FIELDS) if result is not None else None,
    }
    content = (json.dumps(capture, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    if len(content) > MAX_CAPTURE_BYTES:
        raise ValueError("Native research capture exceeds the evidence byte budget")
    with path.open("xb") as output:
        output.write(content)
    return {"capture": path.name, "capture_sha256": hashlib.sha256(content).hexdigest()}


async def fetch(owner, url: str, marker: str = "", capture: Path | None = None) -> dict:
    """Purpose: Crawl with finite lifetime. Inputs: Owner/URL/marker/new capture. Outputs: Diagnosis and evidence."""
    from crawl4ai import CacheMode, CrawlerRunConfig

    started = time.monotonic()
    row = {"url": url, "success": False}
    result = None
    try:
        config = CrawlerRunConfig(cache_mode=CacheMode.BYPASS, check_robots_txt=True)
        result = await asyncio.wait_for(owner.arun(url=url, config=config), timeout=45)
        row.update(inspect_result(result, marker))
    except TimeoutError as error:
        row.update(failure_category="timeout", error=str(error)[:500] or "Page exceeded the 45-second deadline")
    except (ValueError, OSError) as error:
        row.update(failure_category="request_error", error=f"{type(error).__name__}: {error}"[:500])
    row["seconds"] = round(time.monotonic() - started, 3)
    if capture is not None:
        row.update(retain_capture(capture, url, result, row))
    return row


def source_identity() -> dict[str, str]:
    """Purpose: Bind qualification to integration bytes. Inputs: Repository sources. Outputs: Exact source digests."""
    from tools import crawl4ai_source_build, nltk_distribution

    paths = (
        "tools/crawl4ai_tool.py",
        "tools/crawl4ai_research.py",
        "tools/requirements/crawl4ai.txt",
        "tools/crawl4ai_sites.json",
        "tools/nltk_distribution.py",
        "tools/python_wheel.py",
        "tools/test_nltk_model_security.py",
        "tools/requirements/python-build.txt",
        "tools/crawl4ai_source_build.py",
        "tools/crawl4ai_download_opener.py",
        "tools/test_crawl4ai_downloads.py",
    )
    identities = {path: text_identity(ROOT / path) for path in paths}
    record = nltk_distribution.SOURCE_ROOT / "distribution.json"
    identities[record.relative_to(ROOT).as_posix()] = text_identity(record)
    identities["nltk_upstream_wheel"] = nltk_distribution.digest(nltk_distribution.wheel_path())
    identities["crawl4ai_source_build"] = crawl4ai_source_build.identity()
    return identities


async def execute(args) -> int:
    """Purpose: Run the requested local lane. Inputs: Parsed CLI. Outputs: Exit status and bounded evidence."""
    if args.command == "doctor":
        from crawl4ai import CacheMode, CrawlerRunConfig

        async with crawler() as owner:
            result = await asyncio.wait_for(
                owner.arun(
                    url=("raw:<html><body><h1>FlitzZip crawler contract</h1><p>Local extraction.</p></body></html>"),
                    config=CrawlerRunConfig(cache_mode=CacheMode.DISABLED, word_count_threshold=0),
                ),
                timeout=45,
            )
        passed = result.success and "FlitzZip crawler contract" in str(result.markdown)
        print(
            json.dumps({"doctor_passed": bool(passed), "version": VERSION, "browser_configuration": "upstream-default"})
        )
        return 0 if passed else 1
    sites = json.loads(SITES.read_text(encoding="utf-8"))["sites"]
    if args.receipt.exists():
        raise FileExistsError("Research receipt already exists; select a new evidence destination")
    captures = args.receipt.with_suffix(args.receipt.suffix + ".captures")
    captures.mkdir(parents=True, exist_ok=False)
    rows = []
    before = source_identity()
    async with crawler() as owner:
        for index, site in enumerate(sites):
            row = await fetch(owner, site["url"], site["marker"], captures / f"{index:04d}.json")
            rows.append(row)
            print(json.dumps(row), flush=True)
    successful_domains = {urlsplit(row["url"]).hostname for row in rows if row["success"]}
    qualified = len(successful_domains) >= 20 and before == source_identity()
    receipt = {
        "schema": 2,
        "completed_utc": datetime.now(UTC).isoformat(),
        "version": VERSION,
        "browser_configuration": "upstream-default",
        "robots_checked": True,
        "custom_browser_overrides": False,
        "source_identity": before,
        "capture_directory": captures.name,
        "attempted": len(rows),
        "successful_domains": len(successful_domains),
        "qualified": qualified,
        "results": rows,
    }
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    with args.receipt.open("x", encoding="utf-8") as output:
        json.dump(receipt, output, indent=2)
        output.write("\n")
    print(json.dumps({key: receipt[key] for key in ("attempted", "successful_domains", "qualified")}), flush=True)
    return 0 if qualified else 1


def main() -> int:
    """Purpose: Parse explicit crawler lanes. Inputs: CLI. Outputs: Selected lane status."""
    parser = argparse.ArgumentParser(description=__doc__, epilog=ATTRIBUTION)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("doctor")
    qualify = commands.add_parser("qualify")
    qualify.add_argument("--receipt", type=Path, required=True, help="New local JSON evidence file; never overwrite")
    return asyncio.run(execute(parser.parse_args()))


if __name__ == "__main__":
    raise SystemExit(main())
