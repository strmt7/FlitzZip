"""Offline tests for comparator release, permission, binary, and timing gates."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from datetime import UTC, datetime
from pathlib import Path
from unittest.mock import patch
from urllib.parse import urlsplit
from urllib.request import Request

from tools import benchmark_comparators as comparators
from tools import neutron_corpus_benchmark as corpus
from tools import run_archive_comparison as comparison


class ComparatorPreflightTests(unittest.TestCase):
    # Purpose: Refuse ambiguous or nonfinite comparator metadata across release and permission consumers.
    # Inputs: Malformed scalar, duplicate and nonfinite objects; no external discovery occurs.
    # Outputs: All readers reject conflicting evidence and retain explicit caller-visible errors.
    def test_metadata_is_an_unambiguous_finite_object(self) -> None:
        for payload in (
            "[]",
            "null",
            '{"x":NaN}',
            '{"x":Infinity}',
            '{"x":1,"x":2}',
            '{"nested":{"allowed":false,"allowed":true}}',
        ):
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                comparators.parse_metadata(payload)
        stable = '{"tag_name":"v1.5.6","tag_name":"v1.5.7","draft":false,"prerelease":false}'
        with self.assertRaisesRegex(ValueError, "duplicate"):
            comparators.parse_release("Zstd", stable)
        original = json.loads(comparators.PERMISSIONS_PATH.read_text())
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "permissions.json"
            for schema in (True, "1", None, 0, 2):
                path.write_text(json.dumps({**original, "schema_version": schema}))
                with (
                    self.subTest(schema=schema),
                    patch.object(comparators, "PERMISSIONS_PATH", path),
                    self.assertRaisesRegex(ValueError, "permission schema"),
                ):
                    comparators.require_permission("HIGGS", "execute")
            path.write_text('{"subjects":{"Fixture":{"binaries":{},"binaries":{}}}}')
            with patch.object(comparators, "PERMISSIONS_PATH", path), self.assertRaisesRegex(ValueError, "duplicate"):
                comparators.require_binary_identity("Fixture", Path(temporary) / "fixture.exe")

    # Purpose: Exercise the command handler with official-shaped release metadata and deduplicated explicit tools.
    # Inputs: Controlled discovery observations and a fixed UTC clock.
    # Outputs: Exact stable versions and one fetch per tool.
    def test_command_line_reports_deduplicated_stable_releases(self) -> None:
        output = io.StringIO()
        stable = json.dumps({"tag_name": "v1.5.7", "draft": False, "prerelease": False})
        with (
            patch.object(comparators.sys, "argv", ["versions", "--tool", "Zstd", "--tool", "Zstd"]),
            patch.object(comparators, "fetch_release_source", return_value=stable) as fetch,
            patch.object(comparators, "datetime", wraps=datetime) as clock,
            contextlib.redirect_stdout(output),
        ):
            clock.now.return_value = datetime(2026, 10, 20, tzinfo=UTC)
            self.assertEqual(comparators.main(), 0)
        fetch.assert_called_once_with("Zstd")
        self.assertEqual(
            json.loads(output.getvalue()),
            {
                "checked_at_utc": "2026-10-20T00:00:00+00:00",
                "channel": "stable",
                "tools": {"Zstd": {"version": "1.5.7", "source_url": comparators.RELEASE_SOURCES["Zstd"]}},
            },
        )

    # Purpose: Preserve a discovery failure at the actual process entry point rather than publishing partial versions.
    # Inputs: A controlled OS error at urllib's network boundary in a separate interpreter.
    # Outputs: Nonzero status, no JSON success and actionable stderr; no connection is attempted.
    def test_command_line_transport_error_returns_failure(self) -> None:
        program = (
            "import runpy,sys\nfrom unittest.mock import patch\n"
            "sys.argv=['comparators','--tool','Zstd']\n"
            "with patch('urllib.request.OpenerDirector.open',side_effect=OSError('owned transport failure')):\n"
            " runpy.run_module('tools.benchmark_comparators',run_name='__main__')\n"
        )
        process = subprocess.run(
            [sys.executable, "-B", "-c", program],
            cwd=Path(comparators.__file__).resolve().parents[1],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        self.assertEqual(process.returncode, 1)
        self.assertEqual(process.stdout, "")
        self.assertIn("comparator preflight failed: owned transport failure", process.stderr)

    # Purpose: Reject redirects before issuing requests to a changed origin or scheme.
    # Inputs: Offline redirect targets; no network request or browser action occurs.
    # Outputs: All redirect responses fail, including an HTTPS-to-HTTP downgrade.
    def test_release_redirects_are_not_followed(self) -> None:
        handler = comparators.NoReleaseRedirects()
        request = Request(comparators.RELEASE_SOURCES["Zstd"])
        origin = urlsplit(request.full_url)
        targets = (
            origin._replace(scheme="http").geturl(),
            origin._replace(netloc="unreviewed.invalid").geturl(),
            origin._replace(query="redirect").geturl(),
        )
        self.assertEqual(urlsplit(targets[0]).scheme, "http")
        self.assertEqual(urlsplit(targets[0]).netloc, origin.netloc)
        self.assertEqual(urlsplit(targets[1]).scheme, origin.scheme)
        for target in targets:
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
        for tool, source in (
            ("Bandizip", "no release label"),
            ("Zstd", json.dumps({**stable, "tag_name": "v1.5.7-rc1"})),
        ):
            with self.subTest(tool=tool), self.assertRaisesRegex(ValueError, "could not be parsed"):
                comparators.parse_release(tool, source)

    # Purpose: Bound successful and oversized official responses at the native transport boundary.
    # Inputs: Independent in-memory HTTP body streams; no browser or network request runs.
    # Outputs: Exact text or explicit refusal, fixed official URI/deadline, and closed response ownership.
    def test_release_transport_enforces_complete_response_bound_and_closes_stream(self) -> None:
        for size in (comparators.MAX_RESPONSE_BYTES, comparators.MAX_RESPONSE_BYTES + 1):
            payload = b" " * size
            response = io.BytesIO(payload)
            with self.subTest(size=size), patch.object(comparators, "build_opener") as opener:
                opener.return_value.open.return_value = response
                if size > comparators.MAX_RESPONSE_BYTES:
                    with self.assertRaisesRegex(ValueError, "exceeds the bound"):
                        comparators.fetch_release_source("Zstd")
                else:
                    self.assertEqual(comparators.fetch_release_source("Zstd"), payload.decode())
                request = opener.return_value.open.call_args.args[0]
                self.assertEqual(request.full_url, comparators.RELEASE_SOURCES["Zstd"])
                self.assertEqual(opener.return_value.open.call_args.kwargs, {"timeout": 20})
                self.assertTrue(response.closed)
        response = io.BytesIO(b"\xff")
        with patch.object(comparators, "build_opener") as opener:
            opener.return_value.open.return_value = response
            with self.assertRaises(UnicodeDecodeError):
                comparators.fetch_release_source("Zstd")
        self.assertTrue(response.closed)

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
        with self.assertRaisesRegex(ValueError, "unsupported permission action"):
            comparators.require_permission("HIGGS", "unreviewed action")
        with patch.object(comparators, "datetime", wraps=datetime) as clock:
            clock.now.return_value = datetime(2026, 10, 25, tzinfo=UTC)
            comparators.require_permission("7-Zip", "execute", version="26.05")
            comparators.require_permission("Bandizip", "publish_results", version="7.46", edition="Standard")
            comparators.require_permission("lzbench", "execute", version="2.4", modules=("lz4", "zstd"))
            comparators.require_permission("HIGGS", "execute")
            comparators.require_permission("HouseholdPower", "execute")
            comparators.require_permission("HouseholdPower", "publish_results")
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
                comparators.require_permission("7-Zip", "execute", version="26.03")
            with self.assertRaises(ValueError):
                comparators.require_permission("7-Zip", "redistribute", version="26.05")
            clock.now.return_value = datetime(2026, 11, 2, tzinfo=UTC)
            comparators.require_permission("7-Zip", "execute", version="26.05")
            with self.assertRaisesRegex(ValueError, "expired"):
                comparators.require_permission("Bandizip", "publish_results", version="7.46", edition="Standard")
            clock.now.return_value = datetime(2026, 11, 26, tzinfo=UTC)
            with self.assertRaisesRegex(ValueError, "expired"):
                comparators.require_permission("7-Zip", "execute", version="26.05")

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
                binary.unlink()
                with self.assertRaisesRegex(ValueError, "missing or linked"):
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
                comparison.tool_preflight({"FlitzZip": Path("not-executed.exe")})
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


@unittest.skipUnless(os.name == "nt", "The native benchmark installer uses Windows PowerShell")
class HyperfineBootstrapTests(unittest.TestCase):
    def setUp(self) -> None:
        """Purpose: Isolate provisioning from checkout state.
        Inputs: Exact installer bytes. Outputs: Owned fresh fixture.
        """
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.fixture_root = Path(temporary.name)
        tools_root = self.fixture_root / "tools"
        tools_root.mkdir()
        self.installer = tools_root / "bootstrap_comparison_tools.ps1"
        self.installer.write_bytes((corpus.ROOT / "tools/bootstrap_comparison_tools.ps1").read_bytes())
        self.output_root = self.fixture_root / "out"
        self.output_root.mkdir()

    def test_fresh_checkout_creates_owned_output_before_offline_admission(self) -> None:
        """Purpose: Qualify first use.
        Inputs: Absent output directory and no asset. Outputs: Owned setup, then refusal.
        """
        self.output_root.rmdir()
        self.assertFalse(self.output_root.exists())
        with self.assertRaisesRegex(ValueError, "Offline comparison asset is missing"):
            corpus.run_process(
                [
                    "powershell",
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(self.installer),
                    "-Tool",
                    "Hyperfine",
                    "-ToolRoot",
                    str(self.output_root / "tools"),
                    "-Offline",
                ],
                None,
                20,
            )
        self.assertTrue((self.output_root / "tools/downloads").is_dir())
        self.assertFalse(list(self.output_root.rglob("*.exe")))

    def test_offline_bootstrap_rejects_missing_and_changed_artifacts(self) -> None:
        """Purpose: Exercise installer admission. Inputs: Owned inert fixtures. Outputs: Refusal before execution."""
        output_root = self.output_root
        directory = "hyperfine-v1.20.0-x86_64-pc-windows-msvc"
        for role in ("missing", "changed_archive", "changed_binary"):
            with self.subTest(role=role), tempfile.TemporaryDirectory(dir=output_root) as temporary:
                root = Path(temporary)
                self.assertEqual(root.resolve().parent, output_root.resolve())
                if role == "changed_archive":
                    (root / "downloads").mkdir()
                    (root / "downloads" / f"{directory}.zip").write_bytes(b"inert changed archive fixture")
                elif role == "changed_binary":
                    (root / directory).mkdir()
                    (root / directory / "hyperfine.exe").write_bytes(b"inert changed executable fixture")
                expected = "Offline comparison asset is missing" if role == "missing" else "SHA-256 mismatch"
                with self.assertRaisesRegex(ValueError, expected):
                    corpus.run_process(
                        [
                            "powershell",
                            "-NoProfile",
                            "-ExecutionPolicy",
                            "Bypass",
                            "-File",
                            str(self.installer),
                            "-Tool",
                            "Hyperfine",
                            "-ToolRoot",
                            str(root),
                            "-Offline",
                        ],
                        None,
                        20,
                    )

    def test_bootstrap_refuses_an_external_installation_root(self) -> None:
        """Purpose: Preserve owned scope. Inputs: The checkout root. Outputs: Refusal without installation."""
        with self.assertRaisesRegex(ValueError, "Comparison tools must remain inside"):
            corpus.run_process(
                [
                    "powershell",
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(self.installer),
                    "-Tool",
                    "Hyperfine",
                    "-ToolRoot",
                    str(self.fixture_root),
                    "-Offline",
                ],
                None,
                20,
            )

    def test_bootstrap_refuses_linked_ancestors_before_writing(self) -> None:
        """Purpose: Guard path escapes. Inputs: An owned junction. Outputs: No nested installation is created."""
        output_root = self.output_root
        with tempfile.TemporaryDirectory(dir=output_root) as temporary:
            root = Path(temporary)
            self.assertEqual(root.resolve().parent, output_root.resolve())
            target = root / "target"
            target.mkdir()
            link = root / "linked"
            setup = root / "junction.ps1"
            setup.write_text(
                "param([string]$Link,[string]$Target)\n"
                "New-Item -ItemType Junction -Path $Link -Target $Target | Out-Null\n",
                encoding="utf-8",
            )
            corpus.run_process(
                [
                    "powershell",
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(setup),
                    "-Link",
                    str(link),
                    "-Target",
                    str(target),
                ],
                None,
                20,
            )
            try:
                with self.assertRaisesRegex(ValueError, "linked comparison tool ancestor"):
                    corpus.run_process(
                        [
                            "powershell",
                            "-NoProfile",
                            "-ExecutionPolicy",
                            "Bypass",
                            "-File",
                            str(self.installer),
                            "-Tool",
                            "Hyperfine",
                            "-ToolRoot",
                            str(link / "nested"),
                            "-Offline",
                        ],
                        None,
                        20,
                    )
                self.assertFalse((target / "nested").exists())
            finally:
                link.rmdir()


if __name__ == "__main__":
    unittest.main()
