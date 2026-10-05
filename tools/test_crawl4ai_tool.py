"""Offline contracts for portable provisioning and honest Crawl4AI evidence."""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from tools import crawl4ai_research as research
from tools import crawl4ai_tool as tool


class CrawlerContracts(unittest.TestCase):
    """Test repository integration without installing packages or making network requests."""

    def test_git_text_identity_ignores_only_line_endings(self):
        """Purpose: Reuse checkout text. Inputs: Line endings/content changes. Outputs: Identity assertions."""
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "lock.txt"
            path.write_bytes(b"package==1.0\n    --hash=sha256:example\n")
            identity = tool.text_identity(path)
            path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
            self.assertEqual(tool.text_identity(path), identity)
            path.write_bytes(path.read_bytes().replace(b"1.0", b"1.1"))
            self.assertNotEqual(tool.text_identity(path), identity)

    def test_cache_and_interpreter_paths(self):
        """Purpose: Check host-independent ownership. Inputs: Temporary paths/platforms. Outputs: Path assertions."""
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp).resolve()
            self.assertEqual(tool.cache_home(str(path)), path)
            with patch.object(tool.os, "name", "nt"):
                directory, python, identity = tool.environment_paths(path)
                self.assertEqual(python, directory / "Scripts/python.exe")
            with patch.object(tool.os, "name", "posix"):
                directory, python, other = tool.environment_paths(path)
                self.assertEqual(python, directory / "bin/python")
            self.assertEqual(identity, other)
            for platform, expected in (("linux", "superzip/crawl4ai"), ("darwin", "Library/Caches/SuperZip/crawl4ai")):
                with (
                    patch.object(tool.sys, "platform", platform),
                    patch.dict(os.environ, {"USERPROFILE": str(Path.home())}, clear=True),
                ):
                    self.assertTrue(tool.cache_home().as_posix().endswith(expected))
        with self.assertRaises(ValueError):
            tool.cache_home("relative-path")
        with self.assertRaises(ValueError):
            tool.cache_home(str(tool.ROOT))

    def test_process_scoped_environment(self):
        """Purpose: Keep settings child-scoped. Inputs: Existing environment. Outputs: Original remains intact."""
        before = dict(os.environ)
        env = tool.tool_environment(Path.home() / "crawler-test")
        self.assertEqual(dict(os.environ), before)
        self.assertEqual(env["PYTHONNOUSERSITE"], "1")
        self.assertTrue(env["PLAYWRIGHT_BROWSERS_PATH"].endswith("browsers"))
        self.assertEqual(env["LITELLM_TELEMETRY"], "False")

    def test_hidden_child_preserves_output_and_failure(self):
        """Purpose: Preserve child results. Inputs: Real CPU-only children. Outputs: Exact stdout/exit assertions."""
        with tempfile.TemporaryDirectory() as temp:
            output_path = Path(temp) / "stdout.txt"
            with output_path.open("w", encoding="utf-8") as output, patch.object(tool.sys, "stdout", output):
                code = tool.run_owned([sys.executable, "-c", "print('retained output')"], dict(os.environ), 10)
            self.assertEqual(code, 0)
            self.assertEqual(output_path.read_text(encoding="utf-8").strip(), "retained output")
            with self.assertRaises(subprocess.CalledProcessError):
                tool.run([sys.executable, "-c", "raise SystemExit(7)"], dict(os.environ), 10)

    def test_owned_timeout_and_exact_dependency_probe(self):
        """Purpose: Bound lifetime and verify pins. Inputs: CPU child and metadata. Outputs: Deadline/version checks."""
        with self.assertRaises(subprocess.TimeoutExpired):
            tool.run_owned([sys.executable, "-c", "import time; time.sleep(30)"], dict(os.environ), 0.2)
        with patch.object(tool, "run") as child:
            tool.verify_versions(Path(sys.executable), dict(os.environ))
            argv = child.call_args.args[0]
            pins = json.loads(argv[3])
            self.assertEqual(pins["crawl4ai"], tool.VERSION)
            self.assertIn("importlib.metadata.version", argv[2])
            self.assertIn("crawl4ai/superzip_download.py", json.loads(argv[4]))
            self.assertIn("verify_installed_runtime(sys.argv[3])", argv[2])
            self.assertRegex(argv[5], r"^[a-f0-9]{64}$")

    def test_script_entrypoint_imports_its_checkout_from_another_directory(self):
        """Purpose: Preserve direct script use. Inputs: External cwd/script path. Outputs: Local helper imports."""
        with tempfile.TemporaryDirectory() as temp:
            code = (
                "import os,runpy,sys;from pathlib import Path;os.chdir(sys.argv[2]);"
                "scope=runpy.run_path(sys.argv[1],run_name='entrypoint_contract');"
                "scope['environment_paths'](Path(sys.argv[2]))"
            )
            result = subprocess.run(
                [sys.executable, "-B", "-c", code, str(Path(tool.__file__).resolve()), temp],
                cwd=temp,
                capture_output=True,
                text=True,
                timeout=20,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_provision_resolves_repaired_wheel_normally_before_admission(self):
        """Purpose: Keep dependency/security gates mandatory. Inputs: Mocked setup. Outputs: Exact install order."""
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            python = directory / "python"
            python.touch()
            wheel = directory / "reviewed/nltk-security.whl"
            with (
                patch("tools.nltk_security_build.ensure_wheel", return_value=wheel),
                patch("tools.crawl4ai_source_build.ensure_wheel", return_value=wheel),
                patch.object(tool, "run") as run,
            ):
                tool.provision(directory, directory, python, "fixture-identity")
            commands = [call.args[0] for call in run.call_args_list]
            install = next(command for command in commands if "install" in command and "pip" in command)
            self.assertIn("--require-hashes", install)
            self.assertIn("--only-binary=:all:", install)
            self.assertIn(str(wheel.parent), install)
            self.assertNotIn("--no-deps", install)
            check_index = next(i for i, command in enumerate(commands) if command[-2:] == ["pip", "check"])
            model_index = next(
                i for i, command in enumerate(commands) if command[-1] == "tools.test_nltk_model_security"
            )
            download_index = next(
                i for i, command in enumerate(commands) if command[-1] == "tools.test_crawl4ai_downloads"
            )
            browser_index = next(i for i, command in enumerate(commands) if "playwright" in command)
            self.assertLess(check_index, model_index)
            self.assertLess(model_index, download_index)
            self.assertLess(download_index, browser_index)
            self.assertTrue((directory / "superzip-install.json").exists())

    def test_fresh_install_never_admits_a_failed_dependency_or_api_gate(self):
        """Purpose: Enforce setup admission. Inputs: Failed fresh dependency/API gates. Outputs: No receipt."""
        for failed in ("install", "check", "tools.test_nltk_model_security", "tools.test_crawl4ai_downloads"):
            with self.subTest(gate=failed), tempfile.TemporaryDirectory() as owned:
                directory = Path(owned)
                python = directory / "python"
                python.touch()
                commands = []

                def admit(command, *args, _calls=commands, _gate=failed, **kwargs):
                    """Purpose: Fail one actual setup command. Inputs: Production argv. Outputs: Recorded call/error."""
                    _calls.append(command)
                    if _gate in command:
                        raise subprocess.CalledProcessError(1, command)

                with (
                    patch("tools.nltk_security_build.ensure_wheel", return_value=directory / "nltk.whl"),
                    patch("tools.crawl4ai_source_build.ensure_wheel", return_value=directory / "crawl4ai.whl"),
                    patch.object(tool, "run", side_effect=admit),
                    self.assertRaises(subprocess.CalledProcessError),
                ):
                    tool.provision(directory, directory, python, "fresh-admission")
                self.assertFalse((directory / "superzip-install.json").exists())
                self.assertFalse(any("playwright" in command for command in commands))

    def test_changed_security_contract_reuses_dependencies_and_failed_gate_is_not_cached(self):
        """Purpose: Reuse qualified installs. Inputs: Stale gate/owned receipt. Outputs: Only affected test runs."""
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            python = directory / "python"
            python.touch()
            receipt = directory / "superzip-install.json"
            original = {"lock_sha256": "fixture", "platform": tool.platform.system(), "model_contract_sha256": "old"}
            tool.write_install_receipt(receipt, original)
            with (
                patch.object(tool, "verify_versions"),
                patch("tools.nltk_security_build.ensure_wheel") as builder,
                patch.object(tool, "run", side_effect=ValueError("security contract failed")),
                self.assertRaisesRegex(ValueError, "security contract failed"),
            ):
                tool.provision(directory, directory, python, "fixture")
            self.assertEqual(json.loads(receipt.read_text()), original)
            builder.assert_not_called()
            with patch.object(tool, "verify_versions"), patch.object(tool, "run") as run:
                tool.provision(directory, directory, python, "fixture")
                self.assertEqual(run.call_count, 2)
                self.assertEqual(run.call_args_list[0].args[0][-1], "tools.test_nltk_model_security")
                self.assertEqual(run.call_args_list[1].args[0][-1], "tools.test_crawl4ai_downloads")
                tool.provision(directory, directory, python, "fixture")
                self.assertEqual(run.call_count, 2)

    def test_lock_is_complete_hashed_and_latest_reviewed(self):
        """Purpose: Require immutable wheel selection. Inputs: Checked-in lock. Outputs: Every entry pinned/hashed."""
        text = tool.LOCK.read_text(encoding="utf-8")
        entries = re.findall(r"^([\w-]+)==([^\s]+) \\\n((?:    --hash=sha256:[a-f0-9]{64}(?: \\)?\n)+)", text, re.M)
        self.assertGreater(len(entries), 50)
        self.assertEqual(len(entries), len(re.findall(r"^[\w-]+==", text, re.M)))
        versions = {name: version for name, version, _ in entries}
        self.assertEqual(versions["crawl4ai"], tool.VERSION)
        self.assertNotIn("torch", versions)
        self.assertNotIn("sentence-transformers", versions)
        for name, version, hashes in entries:
            self.assertTrue(name and version and hashes)

    def test_documented_upstream_configuration(self):
        """Purpose: Reject custom engine patches. Inputs: Integration source. Outputs: Uses public upstream API."""
        source = (tool.ROOT / "tools/crawl4ai_research.py").read_text(encoding="utf-8")
        self.assertIn("return AsyncWebCrawler()", source)
        self.assertIn("CrawlerRunConfig(cache_mode=CacheMode.BYPASS, check_robots_txt=True)", source)
        for private in ("_build_browser_args", "ResearchBrowserManager", "ResearchRobots", "set_hook", "monkeypatch"):
            self.assertNotIn(private, source)
        with (
            patch.object(research.importlib.metadata, "version", return_value="different"),
            self.assertRaisesRegex(ValueError, "version"),
        ):
            research.crawler()

    def test_automatic_install_and_upstream_cli_passthrough(self):
        """Purpose: Use the official CLI transparently. Inputs: Mock setup/argv. Outputs: Exact forwarding/status."""
        python = Path(sys.executable)
        for arguments in (("https://docs.python.org/", "-o", "markdown", "-c", "check_robots_txt=true"), ("--help",)):
            with (
                patch.object(tool.sys, "argv", ["crawler", "crawl", *arguments]),
                patch.object(tool, "install", return_value=python) as setup,
                patch.object(tool, "run_owned", return_value=7) as launch,
            ):
                self.assertEqual(tool.main(), 7)
                setup.assert_called_once()
                self.assertEqual(launch.call_args.args[0], [str(python), "-m", "crawl4ai.cli", "crawl", *arguments])

    def test_installation_reuse_and_failure_receipt(self):
        """Purpose: Avoid repeated downloads and false setup success. Inputs: Cache fixtures. Outputs: Pin checks."""
        with tempfile.TemporaryDirectory() as temp:
            home = Path(temp)
            directory, python, identity = tool.environment_paths(home)
            python.parent.mkdir(parents=True)
            python.touch()
            receipt = directory / "superzip-install.json"
            receipt.write_text(
                json.dumps(
                    {
                        "lock_sha256": identity,
                        "platform": tool.platform.system(),
                        "model_contract_sha256": tool.text_identity(tool.ROOT / "tools/test_nltk_model_security.py"),
                        "download_contract_sha256": tool.text_identity(tool.ROOT / "tools/test_crawl4ai_downloads.py"),
                    }
                )
            )
            with patch.object(tool, "verify_versions") as verify, patch.object(tool, "run") as launch:
                self.assertEqual(tool.install(home), python)
                verify.assert_called_once()
                launch.assert_not_called()
            receipt.unlink()
            with (
                patch.object(tool, "verify_versions"),
                patch("tools.nltk_security_build.ensure_wheel", return_value=home / "fixture.whl"),
                patch("tools.crawl4ai_source_build.ensure_wheel", return_value=home / "fixture.whl"),
                patch.object(tool, "tool_environment", return_value={"PYTHONUTF8": "1"}),
                patch.object(
                    tool, "run", side_effect=[None, None, None, subprocess.CalledProcessError(7, ["browser"])]
                ),
                self.assertRaises(subprocess.CalledProcessError),
            ):
                tool.install(home)
            self.assertFalse(receipt.exists())

    def test_shared_install_lock_and_quiet_setup(self):
        """Purpose: Bound setup races and hide progress. Inputs: Cache/child doubles. Outputs: Lock/output checks."""
        with (
            tempfile.TemporaryDirectory() as temp,
            tool.installation_lock(Path(temp)),
            self.assertRaises(TimeoutError),
            tool.installation_lock(Path(temp), timeout=0.1),
        ):
            self.fail("Overlapping cache setup was admitted")
        with patch.object(tool, "run_owned", return_value=0) as launch:
            tool.run([sys.executable, "-c", "pass"], dict(os.environ))
            self.assertTrue(launch.call_args.kwargs["quiet"])
        with tempfile.TemporaryDirectory() as temp:
            output_path = Path(temp) / "stdout.txt"
            with output_path.open("w", encoding="utf-8") as output, patch.object(tool.sys, "stdout", output):
                code = tool.run_owned(
                    [sys.executable, "-c", "print('setup progress')"], dict(os.environ), 10, quiet=True
                )
            self.assertEqual(code, 0)
            self.assertEqual(output_path.read_text(encoding="utf-8"), "")

    def test_setup_diagnostics_and_output_limit(self):
        """Purpose: Retain errors without unbounded logs. Inputs: Real setup children. Outputs: Error/budget checks."""
        with tempfile.TemporaryDirectory() as temp:
            error_path = Path(temp) / "stderr.txt"
            with error_path.open("w", encoding="utf-8") as output, patch.object(tool.sys, "stderr", output):
                self.assertEqual(
                    tool.run_owned(
                        [sys.executable, "-c", "print('setup failed'); raise SystemExit(7)"],
                        dict(os.environ),
                        10,
                        quiet=True,
                    ),
                    7,
                )
            self.assertIn("setup failed", error_path.read_text(encoding="utf-8"))
            with (
                error_path.open("w", encoding="utf-8") as output,
                patch.object(tool.sys, "stderr", output),
                self.assertRaisesRegex(ValueError, "output budget"),
            ):
                tool.run_owned(
                    [sys.executable, "-c", "print('x' * (5 * 1024 * 1024))"], dict(os.environ), 10, quiet=True
                )
            self.assertLessEqual(error_path.stat().st_size, 8200)

    def test_result_rejects_challenges_empty_and_error_pages(self):
        """Purpose: Prevent false crawling success. Inputs: Result doubles. Outputs: Content/status validation."""
        result = SimpleNamespace(
            success=True,
            status_code=200,
            markdown=SimpleNamespace(raw_markdown="Python documentation " * 30),
            error_message="",
        )
        row = research.inspect_result(result, "Python")
        self.assertTrue(row["success"])
        self.assertEqual(len(row["markdown_sha256"]), 64)
        self.assertFalse(research.inspect_result(result, "missing marker")["success"])
        result.status_code = 403
        self.assertFalse(research.inspect_result(result)["success"])
        result.status_code = 200
        result.markdown.raw_markdown = "Verify you are human " * 20
        self.assertFalse(research.inspect_result(result)["success"])
        result.markdown.raw_markdown = ""
        self.assertFalse(research.inspect_result(result)["success"])
        result.markdown.raw_markdown = "x" * (research.MAX_MARKDOWN_BYTES + 1)
        self.assertFalse(research.inspect_result(result)["success"])

    def test_qualification_diversity_and_license_notice(self):
        """Purpose: Preserve sites and attribution. Inputs: Tracked manifest/notices. Outputs: Assertions."""
        from urllib.parse import urlsplit

        sites = json.loads(research.SITES.read_text(encoding="utf-8"))["sites"]
        domains = {urlsplit(site["url"]).hostname for site in sites}
        self.assertGreaterEqual(len(domains), 30)
        self.assertEqual(len(domains), len(sites))
        self.assertTrue(all(site["marker"] for site in sites))
        self.assertTrue(all(urlsplit(site["url"]).scheme == "https" for site in sites))
        license_text = (tool.ROOT / "docs/licenses/Crawl4AI-0.9.4.txt").read_text(encoding="utf-8")
        self.assertIn(tool.ATTRIBUTION, license_text)


if __name__ == "__main__":
    unittest.main()
