"""Exercise the installed source repair and real HTTP download consumer without public network access."""

from __future__ import annotations

import asyncio
import importlib.metadata
import ipaddress
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


class DownloadContracts(unittest.TestCase):
    """Verify the production package, including refusal before target truncation."""

    @classmethod
    def setUpClass(cls):
        """Purpose: Admit the actual installed source build. Inputs: Runtime metadata. Outputs: Production opener."""
        from tools.crawl4ai_tool import VERSION

        if importlib.metadata.version("crawl4ai") != VERSION:
            raise AssertionError("Download regression requires the exact source-admitted Crawl4AI runtime")
        from crawl4ai.async_crawler_strategy import _nofollow_opener, _safe_download_filepath

        cls.opener = staticmethod(_nofollow_opener)
        cls.destination = staticmethod(_safe_download_filepath)

    def test_real_creation_overwrite_and_descriptor_ownership(self):
        """Purpose: Preserve normal downloads. Inputs: Owned binary file. Outputs: Exact bytes and closed handle."""
        with tempfile.TemporaryDirectory() as owned:
            path = Path(owned) / "download.json"
            for contents in (b'{"long":123456789}', b'{"ok":true}'):
                with open(path, "wb", opener=self.opener) as stream:
                    self.assertFalse(os.get_inheritable(stream.fileno()))
                    stream.write(contents)
                self.assertEqual(path.read_bytes(), contents)
            with self.assertRaises(FileExistsError), open(path, "xb", opener=self.opener):
                pass
            self.assertEqual(path.read_bytes(), b'{"ok":true}')
            path.unlink()  # Windows denies deletion when an owned handle leaked.

    def test_installed_byte_admission_refuses_a_wrong_expected_repair(self):
        """Purpose: Keep cache reuse fail-closed. Inputs: Actual runtime/wrong digest. Outputs: Admission failure."""
        from tools import crawl4ai_source_build as build
        from tools import crawl4ai_tool as tool

        expected = build.runtime_hashes()
        expected["crawl4ai/superzip_download.py"] = build.hashlib.sha256(b"different repair").hexdigest()
        with tempfile.TemporaryFile(mode="w+b") as diagnostics:
            with (
                patch.object(build, "runtime_hashes", return_value=expected),
                patch.object(tool.sys, "stderr", diagnostics),
                self.assertRaises(subprocess.CalledProcessError),
            ):
                tool.verify_versions(Path(sys.executable), tool.tool_environment(tool.cache_home()))
            diagnostics.seek(0)
            self.assertIn(b"crawler repair bytes changed", diagnostics.read(8192))

    def test_reparse_target_is_not_truncated(self):
        """Purpose: Refuse links at the actual open. Inputs: Owned link/target. Outputs: Error and unchanged target."""
        with tempfile.TemporaryDirectory() as owned:
            root = Path(owned)
            target = root / "target"
            target.write_bytes(b"must survive")
            link = root / "link"
            try:
                link.symlink_to(target)
            except OSError as error:
                if os.name == "nt" and error.winerror == 1314:
                    self.skipTest("Host policy does not permit unprivileged symbolic-link creation")
                raise
            with self.assertRaises(OSError), open(link, "wb", opener=self.opener):
                pass
            self.assertEqual(target.read_bytes(), b"must survive")
            self.assertTrue(link.is_symlink())

    @unittest.skipUnless(os.name == "nt", "Windows native-handle boundary")
    def test_windows_hardlink_and_reserved_destinations_refused(self):
        """Purpose: Reject unsafe Windows aliases. Inputs: Hardlink/device/stream destinations. Outputs: No writes."""
        with tempfile.TemporaryDirectory() as owned:
            target = Path(owned) / "target"
            target.write_bytes(b"preserved")
            link = Path(owned) / "hardlink"
            os.link(target, link)
            names = (
                link.name,
                "CON",
                "con.json",
                "CONIN$",
                "NUL",
                "AUX.txt",
                "LPT1",
                "COM1",
                "target:stream",
                "trailing.",
            )
            for name in names:
                with (
                    self.subTest(destination=name),
                    self.assertRaises((OSError, ValueError)),
                    open(self.destination(owned, name), "wb", opener=self.opener),
                ):
                    pass
            self.assertEqual(target.read_bytes(), b"preserved")
            link.unlink()
            target.unlink()

    @unittest.skipUnless(os.name == "nt", "Windows reserved-name admission")
    def test_http_reserved_headers_refused_before_opening(self):
        """Purpose: Protect the complete download consumer. Inputs: Remote reserved names. Outputs: No file opens."""
        import aiofiles
        from aiohttp import web
        from aiohttp.test_utils import TestServer
        from crawl4ai import CrawlerRunConfig, HTTPCrawlerConfig
        from crawl4ai.async_crawler_strategy import AsyncHTTPCrawlerStrategy, HTTPCrawlerError

        async def respond(request):
            """Purpose: Supply a hostile download name. Inputs: Owned request. Outputs: Typed named response."""
            return web.Response(
                body=b"reserved filename must not be written",
                content_type="application/octet-stream",
                headers={"Content-Disposition": f'attachment; filename="{request.query["name"]}"'},
            )

        async def consume(root: str):
            """Purpose: Exercise admission before file I/O. Inputs: Owned root. Outputs: Verified refusals."""
            app = web.Application()
            app.router.add_get("/reserved", respond)
            async with TestServer(app) as server:
                self.assertTrue(ipaddress.ip_address(server.host).is_loopback)
                async with AsyncHTTPCrawlerStrategy(browser_config=HTTPCrawlerConfig(downloads_path=root)) as strategy:
                    # The real admission must reject before aiofiles or CPython can select WindowsConsoleIO.
                    with patch("crawl4ai.async_crawler_strategy.aiofiles.open", wraps=aiofiles.open) as opened:
                        for name in ("CON", "CON.txt", "CONIN$", "NUL", "AUX.txt", "target:stream", "trailing."):
                            url = server.make_url("/reserved").with_query(name=name)
                            with (
                                self.subTest(filename=name),
                                self.assertRaisesRegex(HTTPCrawlerError, "Reserved Windows download destination"),
                            ):
                                await asyncio.wait_for(
                                    strategy.crawl(str(url), CrawlerRunConfig(page_timeout=5000)), 10
                                )
                        opened.assert_not_called()
            self.assertEqual(list(Path(root).iterdir()), [])

        with tempfile.TemporaryDirectory() as owned:
            asyncio.run(consume(owned))

    def test_confined_http_consumer_preserves_complete_download_and_content(self):
        """Purpose: Exercise the failing HTTP path. Inputs: Owned JSON response. Outputs: Exact file/content."""
        from aiohttp import web
        from aiohttp.test_utils import TestServer
        from crawl4ai import CrawlerRunConfig, HTTPCrawlerConfig
        from crawl4ai.async_crawler_strategy import AsyncHTTPCrawlerStrategy

        payload = b'{"crawler_download_contract":"byte exact","value":123}'

        async def respond(request):
            """Purpose: Supply the owned JSON fixture. Inputs: TestServer request. Outputs: Exact typed response."""
            return web.Response(body=payload, content_type="application/json")

        async def consume(root: str):
            """Purpose: Exercise the upstream HTTP consumer. Inputs: Owned root. Outputs: Verified native result."""
            app = web.Application()
            app.router.add_get("/contract.json", respond)
            # Reuse aiohttp's owned loopback test fixture and its actual allocated URL.
            async with TestServer(app) as server:
                self.assertTrue(ipaddress.ip_address(server.host).is_loopback)
                async with AsyncHTTPCrawlerStrategy(browser_config=HTTPCrawlerConfig(downloads_path=root)) as strategy:
                    result = await asyncio.wait_for(
                        strategy.crawl(str(server.make_url("/contract.json")), CrawlerRunConfig(page_timeout=5000)), 10
                    )
                self.assertEqual(result.status_code, 200)
                self.assertEqual(result.html.encode(), payload)
                self.assertEqual(len(result.downloaded_files), 1)
                path = Path(result.downloaded_files[0])
                self.assertEqual(path.parent.resolve(), Path(root).resolve())
                self.assertEqual(path.read_bytes(), payload)

        with tempfile.TemporaryDirectory() as owned:
            asyncio.run(consume(owned))


if __name__ == "__main__":
    unittest.main()
