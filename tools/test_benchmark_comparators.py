"""Offline tests for comparator release, permission, binary, and timing gates."""

from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from datetime import UTC, datetime
from pathlib import Path
from unittest.mock import patch
from urllib.request import Request

from tools import benchmark_comparators as comparators
from tools import run_archive_comparison as comparison


class ComparatorPreflightTests(unittest.TestCase):
    # Purpose: Reject redirects before issuing requests to a changed origin or scheme.
    # Inputs: Offline redirect targets; no network request or browser action occurs.
    # Outputs: All redirect responses fail, including an HTTPS-to-HTTP downgrade.
    def test_release_redirects_are_not_followed(self) -> None:
        handler = comparators.NoReleaseRedirects()
        request = Request(comparators.RELEASE_SOURCES["Zstd"])
        for target in ("http://unreviewed.invalid/", "https://unreviewed.invalid/", request.full_url + "?redirect"):
            with self.subTest(target=target), self.assertRaisesRegex(ValueError, "redirected"):
                handler.redirect_request(request, None, 302, "Found", {}, target)

    # Purpose: Separate stable releases from malformed or prerelease API results.
    # Inputs: Offline GitHub, Bandizip, and WinRAR release-page examples.
    # Outputs: Exact stable labels pass; missing, ambiguous, or prerelease labels fail.
    def test_release_parsing(self) -> None:
        stable = {"tag_name": "v1.5.7", "draft": False, "prerelease": False}
        self.assertEqual(comparators.parse_release("Zstd", json.dumps(stable)), "1.5.7")
        self.assertEqual(comparators.parse_release("Bandizip", "<h2>v7.46</h2><h2>v7.45</h2>"), "7.46")
        page = "<a>WinRAR x64 7.30 beta 1</a><a>WinRAR x64 7.23</a>"
        self.assertEqual(comparators.parse_release("WinRAR", page), "7.23")
        for source in ("[]", "null", '{"tag_name": null}', json.dumps({**stable, "prerelease": True})):
            with self.subTest(source=source), self.assertRaises(ValueError):
                comparators.parse_release("Zstd", source)
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            comparators.parse_release("WinRAR", page + "<a>WinRAR x64 7.22</a>")

    # Purpose: Reject stale actual version banners even when a newer version appears elsewhere.
    # Inputs: Mocked official versions, current/stale banners, and a Bandizip build revision.
    # Outputs: Current stable versions pass; stale and beta banners fail.
    def test_installed_version_identity(self) -> None:
        stable = json.dumps({"tag_name": "v1.5.7", "draft": False, "prerelease": False})
        with patch.object(comparators, "fetch_release_source", return_value=stable):
            self.assertEqual(comparators.require_current_release("Zstd", "zstd v1.5.7")["version"], "1.5.7")
            for banner in ("zstd v1.5.6; latest release is 1.5.7", "zstd v1.5.7 beta", "zstd v1.5.7.1"):
                with self.subTest(banner=banner), self.assertRaisesRegex(ValueError, "not the current stable"):
                    comparators.require_current_release("Zstd", banner)
        with patch.object(comparators, "fetch_release_source", return_value="<h2>v7.46</h2>"):
            self.assertEqual(comparators.require_current_release("Bandizip", "7.46.0.1")["version"], "7.46")

    # Purpose: Keep reviewed actions, exact versions, editions, and codec modules separate.
    # Inputs: The reviewed catalog with a fixed review-date clock; no commercial tool is executed.
    # Outputs: Eligible scopes pass and every unsupported action or scope fails closed.
    def test_permission_scopes(self) -> None:
        with patch.object(comparators, "datetime", wraps=datetime) as clock:
            clock.now.return_value = datetime(2026, 10, 1, tzinfo=UTC)
            comparators.require_permission("7-Zip", "execute", version="26.03")
            comparators.require_permission("Bandizip", "publish_results", version="7.46", edition="Standard")
            comparators.require_permission("lzbench", "execute", version="2.4", modules=("lz4", "zstd"))
            comparators.require_permission("HIGGS", "execute")
            comparators.require_permission("PeaZip", "execute", version="11.3.0")
            for subject in ("WinZip", "WinRAR", "Silesia", "NanaZip", "Unknown"):
                with self.subTest(subject=subject), self.assertRaises(ValueError):
                    comparators.require_permission(subject, "execute")
            for kwargs in ({"version": "7.46"}, {"version": "7.46", "edition": "Professional"}):
                with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                    comparators.require_permission("Bandizip", "execute", **kwargs)
            for modules in ((), ("all",), ("unreviewed-codec",)):
                with self.subTest(modules=modules), self.assertRaises(ValueError):
                    comparators.require_permission("lzbench", "execute", version="2.4", modules=modules)
            with self.assertRaises(ValueError):
                comparators.require_permission("7-Zip", "execute", version="26.04")
            with self.assertRaises(ValueError):
                comparators.require_permission("7-Zip", "redistribute", version="26.03")
            clock.now.return_value = datetime(2026, 11, 2, tzinfo=UTC)
            with self.assertRaisesRegex(ValueError, "expired"):
                comparators.require_permission("7-Zip", "execute", version="26.03")

    # Purpose: Require independently pinned executable bytes rather than trusting a product name.
    # Inputs: A temporary catalog and synthetic binary fixture with a known hash.
    # Outputs: Exact bytes pass; modified bytes and unknown executable names fail.
    def test_binary_pin(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary = root / "fixture.exe"
            binary.write_bytes(b"owned fixture; never executed")
            expected = hashlib.sha256(binary.read_bytes()).hexdigest()
            manifest = root / "permissions.json"
            manifest.write_text(json.dumps({"subjects": {"Fixture": {"binaries": {binary.name: expected}}}}))
            with patch.object(comparators, "PERMISSIONS_PATH", manifest):
                self.assertEqual(comparators.require_binary_identity("Fixture", binary)[binary.name], expected)
                binary.write_bytes(b"modified")
                with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                    comparators.require_binary_identity("Fixture", binary)
                with self.assertRaisesRegex(ValueError, "no reviewed"):
                    comparators.require_binary_identity("Fixture", root / "unreviewed.exe")

    # Purpose: Prevent corpus holds from reaching even a product version subprocess.
    # Inputs: Actual held corpus decision and mocked subprocess execution.
    # Outputs: Preflight reports the hold with zero subprocess calls.
    def test_held_corpus_stops_all_execution(self) -> None:
        with (
            patch.object(comparison.subprocess, "run") as process,
            patch.object(comparators, "datetime", wraps=datetime) as clock,
        ):
            clock.now.return_value = datetime(2026, 10, 1, tzinfo=UTC)
            with self.assertRaisesRegex(ValueError, "Silesia execute is hold"):
                comparison.tool_preflight({"SuperZip": Path("not-executed.exe")})
            process.assert_not_called()

    # Purpose: Keep short diagnostics out of sustained headline evidence without dropping raw samples.
    # Inputs: Valid sustained, short, undersampled, and malformed timing arrays.
    # Outputs: Eligibility is explicit; malformed samples fail instead of becoming zero.
    def test_timing_eligibility(self) -> None:
        result = {"tool": "Fixture", "compress_seconds": [2.0] * 5, "extract_seconds": [2.0] * 5}
        self.assertTrue(comparators.timing_eligibility([result])["headline_eligible"])
        for samples in ([0.1] * 5, [2.0] * 4, [1.0] * 5):
            record = {**result, "compress_seconds": samples}
            self.assertFalse(comparators.timing_eligibility([record])["headline_eligible"])
            self.assertEqual(record["compress_seconds"], samples)
        for samples in ([], [True] * 5, [float("nan")] * 5, [-1] * 5, "wrong type"):
            with self.subTest(samples=samples), self.assertRaises(ValueError):
                comparators.timing_eligibility([{**result, "compress_seconds": samples}])
        with self.assertRaises(ValueError):
            comparators.timing_eligibility([])


if __name__ == "__main__":
    unittest.main()
