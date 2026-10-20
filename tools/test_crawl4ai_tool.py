"""Offline contracts for portable provisioning and honest Crawl4AI evidence."""

from __future__ import annotations

import asyncio
import hashlib
import json
import os
import py_compile
import re
import runpy
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock, patch

from tools import crawl4ai_research as research
from tools import crawl4ai_tool as tool


class CrawlerContracts(unittest.TestCase):
    """Test repository integration without installing packages or making network requests."""

    # Purpose: Prevent known vulnerable image decoders from returning during ordinary crawler lock refreshes.
    # Inputs: The active exact Pillow pin; the installed-consumer suite verifies actual decoding separately.
    # Outputs: Requires the fixed release floor without changing parent dependency constraints or scanner coverage.
    def test_pillow_decoder_advisory_floor(self):
        versions = re.findall(
            r"(?m)^pillow==([0-9]+)\.([0-9]+)\.([0-9]+)(?=\s|$)", tool.LOCK.read_text(encoding="utf-8")
        )
        self.assertEqual(len(versions), 1)
        self.assertGreaterEqual(tuple(map(int, versions[0])), (13, 0, 0))

    def test_lock_resolution_uses_an_empty_environment_and_preserves_failures(self):
        """Purpose: Detect masked dependency conflicts. Inputs: Resolver outcomes. Outputs: Isolation checks."""
        for failure in (False, True):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp:
                home = Path(temp)
                observed = []

                def resolve(command, env, _calls=observed, _home=home, _failure=failure):
                    """Purpose: Inspect the resolver boundary. Inputs: Actual argv/env. Outputs: Result or error."""
                    _calls.append(command)
                    self.assertTrue(Path(command[0]).parent.parent.is_relative_to(_home))
                    self.assertIn("--dry-run", command)
                    self.assertIn("--ignore-installed", command)
                    self.assertIn("--require-hashes", command)
                    self.assertIn("--only-binary=:all:", command)
                    self.assertNotIn("--no-deps", command)
                    self.assertEqual(command[-2:], ["-r", str(tool.LOCK)])
                    self.assertEqual(env["PYTHONNOUSERSITE"], "1")
                    if _failure:
                        raise subprocess.CalledProcessError(1, command)

                with (
                    patch("tools.nltk_distribution.wheel_path", return_value=home / "nltk/a.whl"),
                    patch("tools.crawl4ai_source_build.ensure_wheel", return_value=home / "crawler/b.whl"),
                    patch.object(tool.venv, "EnvBuilder") as builder,
                    patch.object(tool, "run", side_effect=resolve),
                    patch.object(tool, "install") as install,
                ):
                    if failure:
                        with self.assertRaises(subprocess.CalledProcessError):
                            tool.check_lock(home)
                    else:
                        tool.check_lock(home)
                    builder.assert_called_once_with(with_pip=True)
                    install.assert_not_called()
                self.assertEqual(len(observed), 1)
                self.assertEqual(list(home.iterdir()), [])

    def test_check_lock_cli_does_not_install_the_runtime_or_browser(self):
        """Purpose: Separate validation from setup. Inputs: Public command. Outputs: Only resolver invoked."""
        with (
            patch.object(sys, "argv", ["crawl4ai_tool.py", "check-lock"]),
            patch.object(tool, "check_lock") as resolve,
            patch.object(tool, "install") as install,
        ):
            self.assertEqual(tool.main(), 0)
            resolve.assert_called_once()
            install.assert_not_called()

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
            for platform, expected in (("linux", "flitzzip/crawl4ai"), ("darwin", "Library/Caches/FlitzZip/crawl4ai")):
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
        with (
            patch.object(tool, "run") as child,
            patch("tools.nltk_distribution.runtime_source_sha256", return_value=hashlib.sha256(b"fixture").hexdigest()),
        ):
            tool.verify_versions(Path(sys.executable), dict(os.environ))
            argv = child.call_args.args[0]
            pins = json.loads(argv[3])
            self.assertEqual(pins["crawl4ai"], tool.VERSION)
            self.assertIn("importlib.metadata.version", argv[2])
            self.assertIn("crawl4ai/flitzzip_download.py", json.loads(argv[4]))
            self.assertIn("verify_installed_runtime(sys.argv[3])", argv[2])
            self.assertRegex(argv[5], r"^[a-f0-9]{64}$")
            self.assertTrue(child.call_args.kwargs["source_only"])

    def test_source_only_runtime_ignores_unchecked_bytecode_and_preserves_cache(self):
        """Purpose: Refuse stale execution. Inputs: Sentinel cache. Outputs: Reviewed source and unchanged old cache."""
        from tools import nltk_distribution as build

        temporary_root = Path(tempfile.gettempdir()).resolve()
        with tempfile.TemporaryDirectory(dir=temporary_root) as temporary:
            root = Path(temporary)
            self.assertEqual(root.resolve().parent, temporary_root)
            package = root / "nltk"
            package.mkdir()
            source = package / "__init__.py"
            reviewed = b"SOURCE_SENTINEL = 11\n"
            source.write_bytes(reviewed)
            (package / "VERSION").write_text("inert source-guard fixture\n", encoding="utf-8")
            expected = build.installed_runtime_sha256(package)
            source.write_bytes(b"SOURCE_SENTINEL = 22\n")
            cached = Path(
                py_compile.compile(
                    str(source),
                    doraise=True,
                    invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH,
                )
            )
            original_cache = cached.read_bytes()
            source.write_bytes(reviewed)
            with patch("importlib.metadata.distribution", return_value=SimpleNamespace(locate_file=lambda _: package)):
                build.verify_installed_runtime(expected)
            script = (
                "import sys;sys.path.insert(0,sys.argv[1]);import nltk;"
                "raise SystemExit(0 if nltk.SOURCE_SENTINEL==11 else 5)"
            )
            command = [sys.executable, "-c", script, str(root)]
            self.assertEqual(tool.run_owned(command, dict(os.environ), 10), 5)
            self.assertEqual(tool.run_source_python(command, dict(os.environ), 10), 0)
            self.assertEqual(source.read_bytes(), reviewed)
            self.assertEqual(cached.read_bytes(), original_cache)

    def test_source_runtime_flags_are_scoped_and_temporary(self):
        """Purpose: Bind import lifetime. Inputs: Captured launcher. Outputs: Empty owned cache and cleanup."""
        environment = {"PYTHONDONTWRITEBYTECODE": "0", "PYTHONPYCACHEPREFIX": "caller-value"}
        observed = []

        def observe(command, env, timeout, **kwargs):
            """Purpose: Inspect actual policy. Inputs: Wrapped argv/environment. Outputs: Fixture exit status."""
            prefix = Path(env["PYTHONPYCACHEPREFIX"])
            self.assertTrue(prefix.is_dir())
            self.assertEqual(list(prefix.iterdir()), [])
            self.assertEqual(command[:3], [sys.executable, "-B", "-X"])
            self.assertEqual(command[3], f"pycache_prefix={prefix}")
            self.assertEqual(command[4:], ["-c", "pass"])
            self.assertEqual(env["PYTHONDONTWRITEBYTECODE"], "1")
            observed.append(prefix)
            return 7

        with patch.object(tool, "run_owned", side_effect=observe):
            self.assertEqual(tool.run_source_python([sys.executable, "-c", "pass"], environment, 10), 7)
        self.assertFalse(observed[0].exists())
        self.assertEqual(environment, {"PYTHONDONTWRITEBYTECODE": "0", "PYTHONPYCACHEPREFIX": "caller-value"})
        with patch.object(tool, "run_owned") as child, self.assertRaisesRegex(ValueError, "interpreter command"):
            tool.run_source_python([], environment)
        child.assert_not_called()

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

    def test_provision_resolves_upstream_wheel_normally_before_admission(self):
        """Purpose: Keep dependency/security gates mandatory. Inputs: Mocked setup. Outputs: Exact install order."""
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            python = directory / "python"
            python.touch()
            wheel = directory / "asset.whl"
            with (
                patch("tools.nltk_distribution.wheel_path", return_value=wheel),
                patch(
                    "tools.nltk_distribution.runtime_source_sha256",
                    return_value=hashlib.sha256(b"fixture").hexdigest(),
                ),
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
            self.assertTrue((directory / "flitzzip-install.json").exists())

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
                    patch("tools.crawl4ai_source_build.ensure_wheel", return_value=directory / "crawl4ai.whl"),
                    patch("tools.nltk_distribution.wheel_path", return_value=directory / "nltk.whl"),
                    patch(
                        "tools.nltk_distribution.runtime_source_sha256",
                        return_value=hashlib.sha256(b"fixture").hexdigest(),
                    ),
                    patch.object(tool, "run", side_effect=admit),
                    self.assertRaises(subprocess.CalledProcessError),
                ):
                    tool.provision(directory, directory, python, "fresh-admission")
                self.assertFalse((directory / "flitzzip-install.json").exists())
                self.assertFalse(any("playwright" in command for command in commands))

    def test_changed_security_contract_reuses_dependencies_and_failed_gate_is_not_cached(self):
        """Purpose: Reuse qualified installs. Inputs: Stale gate/owned receipt. Outputs: Only affected test runs."""
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            python = directory / "python"
            python.touch()
            receipt = directory / "flitzzip-install.json"
            original = {"lock_sha256": "fixture", "platform": tool.platform.system(), "model_contract_sha256": "old"}
            tool.write_install_receipt(receipt, original)
            with (
                patch.object(tool, "verify_versions"),
                patch("tools.nltk_distribution.wheel_path") as builder,
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
                patch.object(tool, "run_source_python", return_value=7) as launch,
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
            receipt = directory / "flitzzip-install.json"
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
                patch("tools.nltk_distribution.wheel_path", return_value=home / "fixture.whl"),
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

    # Purpose: Admit only bounded, typed, distinct public HTTPS research targets before the crawler starts.
    # Inputs: Owned 20-site manifest plus independent encoding, schema, shape, endpoint and marker mutations.
    # Outputs: Exact rows pass; ambiguous JSON and local, credentialed, numeric or stateful URLs fail closed.
    def test_research_site_manifest_admission(self):
        sites = [
            {"url": f"https://docs{index}.example.invalid/reference/", "marker": f"Marker {index}"}
            for index in range(20)
        ]
        document = {"schema": 1, "purpose": "Owned offline crawler admission fixture.", "sites": sites}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sites.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            self.assertEqual(research.read_sites(path), sites)
            for payload, message in (
                (b"", "1..65536 bytes"),
                (b"x" * (research.MAX_SITES_BYTES + 1), "1..65536 bytes"),
                (b"\xff", "UTF-8 JSON"),
                (b"{", "UTF-8 JSON"),
            ):
                path.write_bytes(payload)
                with self.subTest(payload=len(payload)), self.assertRaisesRegex(ValueError, message):
                    research.read_sites(path)
            mutations = (
                ("boolean schema", lambda row: row.__setitem__("schema", True), "fields or schema"),
                ("empty purpose", lambda row: row.__setitem__("purpose", ""), "purpose"),
                ("few sites", lambda row: row.__setitem__("sites", row["sites"][:19]), "20-100"),
                ("site fields", lambda row: row["sites"][0].__setitem__("extra", True), "fields"),
                ("blank marker", lambda row: row["sites"][0].__setitem__("marker", " "), "URL or marker"),
                (
                    "insecure scheme",
                    lambda row: row["sites"][0].__setitem__("url", "ftp://docs0.example.invalid/reference/"),
                    "public HTTPS origins",
                ),
                (
                    "credentials",
                    lambda row: row["sites"][0].__setitem__("url", "https://user@docs0.example.invalid/reference/"),
                    "public HTTPS origins",
                ),
                (
                    "query",
                    lambda row: row["sites"][0].__setitem__("url", "https://docs0.example.invalid/reference/?state=1"),
                    "public HTTPS origins",
                ),
                (
                    "numeric host",
                    lambda row: row["sites"][0].__setitem__("url", "https://192.0.2.1/reference/"),
                    "public HTTPS origins",
                ),
                (
                    "ambiguous port",
                    lambda row: row["sites"][0].__setitem__("url", "https://docs0.example.invalid:invalid/"),
                    "unambiguous public HTTPS URLs",
                ),
                (
                    "duplicate host",
                    lambda row: row["sites"][1].__setitem__("url", row["sites"][0]["url"] + "other"),
                    "public HTTPS origins",
                ),
            )
            for label, mutate, message in mutations:
                invalid = json.loads(json.dumps(document))
                mutate(invalid)
                path.write_text(json.dumps(invalid), encoding="utf-8")
                with self.subTest(mutation=label), self.assertRaisesRegex(ValueError, message):
                    research.read_sites(path)

    # Purpose: Preserve successful crawl diagnosis and convert owned request failures into retained evidence.
    # Inputs: In-memory CrawlResult/error doubles and the actual upstream configuration constructor boundary.
    # Outputs: Success uses content inspection; OSError becomes request_error without claiming access denial.
    def test_fetch_success_and_request_error(self):
        module = SimpleNamespace(CacheMode=SimpleNamespace(BYPASS="bypass"), CrawlerRunConfig=Mock())
        result = SimpleNamespace(
            success=True,
            status_code=200,
            redirected_status_code=None,
            response_headers={},
            error_message="",
            markdown=SimpleNamespace(raw_markdown="Expected documentation " * 20),
        )
        with patch.dict(sys.modules, {"crawl4ai": module}):
            row = asyncio.run(
                research.fetch(
                    SimpleNamespace(arun=AsyncMock(return_value=result)), "https://example.invalid", "Expected"
                )
            )
            self.assertTrue(row["success"])
            self.assertGreaterEqual(row["seconds"], 0)
            failure = asyncio.run(
                research.fetch(
                    SimpleNamespace(arun=AsyncMock(side_effect=OSError("owned request failure"))),
                    "https://example.invalid",
                )
            )
        self.assertEqual(failure["failure_category"], "request_error")
        self.assertIn("OSError: owned request failure", failure["error"])
        self.assertEqual(module.CrawlerRunConfig.call_count, 2)

    # Purpose: Bind crawler construction to the reviewed package and exercise the local raw-document doctor lane.
    # Inputs: Installed-version doubles, upstream constructor and successful/failed local extraction results.
    # Outputs: Version drift refuses construction; exact local marker controls the doctor's exit status.
    def test_crawler_constructor_and_doctor_lane(self):
        crawler_owner = Mock(return_value=SimpleNamespace())
        module = SimpleNamespace(
            AsyncWebCrawler=crawler_owner,
            CacheMode=SimpleNamespace(DISABLED="disabled"),
            CrawlerRunConfig=Mock(),
        )
        with (
            patch.dict(sys.modules, {"crawl4ai": module}),
            patch.object(research.importlib.metadata, "version", return_value="different"),
            self.assertRaisesRegex(ValueError, "reviewed integration"),
        ):
            research.crawler()
        with (
            patch.dict(sys.modules, {"crawl4ai": module}),
            patch.object(research.importlib.metadata, "version", return_value=tool.VERSION),
        ):
            self.assertIs(research.crawler(), crawler_owner.return_value)
        crawler_owner.assert_called_once_with()
        for markdown, expected in (("FlitzZip crawler contract", 0), ("wrong content", 1)):
            result = SimpleNamespace(success=True, markdown=markdown)
            context = AsyncMock()
            context.__aenter__.return_value = SimpleNamespace(arun=AsyncMock(return_value=result))
            with (
                self.subTest(markdown=markdown),
                patch.dict(sys.modules, {"crawl4ai": module}),
                patch.object(research, "crawler", return_value=context),
                patch("builtins.print") as output,
            ):
                self.assertEqual(asyncio.run(research.execute(SimpleNamespace(command="doctor"))), expected)
            emitted = json.loads(output.call_args.args[0])
            self.assertEqual(emitted["doctor_passed"], not expected)
            module.CrawlerRunConfig.assert_called_with(cache_mode="disabled", word_count_threshold=0)

    # Purpose: Bind qualification to every repository integration and admitted upstream artifact identity.
    # Inputs: Isolated repository paths and deterministic source, wheel and source-build digests.
    # Outputs: The complete expected identity inventory is returned without importing NLTK or Crawl4AI runtime code.
    def test_research_source_identity_inventory(self):
        from tools import crawl4ai_source_build, nltk_distribution

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            distribution = root / "third_party/nltk/distribution.json"
            wheel = root / "nltk.whl"

            def identity(path):
                """Purpose: Model source identity. Inputs: Owned path. Outputs: Deterministic SHA-256."""
                return hashlib.sha256(str(path).encode()).hexdigest()

            with (
                patch.object(research, "ROOT", root),
                patch.object(research, "text_identity", side_effect=identity),
                patch.object(nltk_distribution, "SOURCE_ROOT", distribution.parent),
                patch.object(nltk_distribution, "wheel_path", return_value=wheel),
                patch.object(nltk_distribution, "digest", return_value="a" * 64),
                patch.object(crawl4ai_source_build, "identity", return_value="b" * 64),
            ):
                observed = research.source_identity()
            self.assertEqual(observed["third_party/nltk/distribution.json"], identity(distribution))
            self.assertEqual(observed["nltk_upstream_wheel"], "a" * 64)
            self.assertEqual(observed["crawl4ai_source_build"], "b" * 64)
            self.assertIn("tools/crawl4ai_research.py", observed)

    # Purpose: Qualify only diverse successful domains with unchanged integration bytes and immutable evidence paths.
    # Inputs: Owned 20-domain manifest, in-memory crawler, exact capture writer and stable/drifting source identities.
    # Outputs: Stable evidence qualifies; source drift returns one; an existing receipt is never overwritten.
    def test_research_qualification_publication_and_source_drift(self):
        sites = [
            {"url": f"https://docs{index}.example.invalid/reference/", "marker": f"Marker {index}"}
            for index in range(20)
        ]
        context = AsyncMock()
        context.__aenter__.return_value = SimpleNamespace()

        async def retain(_owner, url, marker, capture):
            capture.write_text(json.dumps({"url": url, "marker": marker}), encoding="utf-8")
            return {"url": url, "success": True, "capture": capture.name}

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = root / "sites.json"
            manifest.write_text(
                json.dumps({"schema": 1, "purpose": "Owned qualification fixture.", "sites": sites}),
                encoding="utf-8",
            )
            for label, identities, expected in (
                ("stable", [{"source": "a"}, {"source": "a"}], 0),
                ("drift", [{"source": "a"}, {"source": "b"}], 1),
            ):
                receipt = root / f"{label}.json"
                args = SimpleNamespace(command="qualify", receipt=receipt)
                with (
                    patch.object(research, "SITES", manifest),
                    patch.object(research, "crawler", return_value=context),
                    patch.object(research, "fetch", AsyncMock(side_effect=retain)),
                    patch.object(research, "source_identity", side_effect=identities),
                    patch("builtins.print"),
                ):
                    self.assertEqual(asyncio.run(research.execute(args)), expected)
                record = json.loads(receipt.read_text(encoding="utf-8"))
                self.assertEqual(record["qualified"], not expected)
                self.assertEqual(record["successful_domains"], 20)
                self.assertEqual(len(list((root / f"{label}.json.captures").iterdir())), 20)
            with (
                patch.object(research, "SITES", manifest),
                patch.object(research, "crawler") as forbidden,
                self.assertRaisesRegex(FileExistsError, "already exists"),
            ):
                asyncio.run(research.execute(SimpleNamespace(command="qualify", receipt=root / "stable.json")))
            forbidden.assert_not_called()

    # Purpose: Exercise both parser routes and preserve the module entrypoint's exact exit status.
    # Inputs: Controlled asynchronous executor and a run-path invocation with an owned parsed command.
    # Outputs: Doctor/qualify arguments route correctly and __main__ raises the executor status unchanged.
    def test_research_cli_and_module_entrypoint(self):
        with (
            patch("sys.argv", ["crawl4ai_research", "doctor"]),
            patch.object(research, "execute", AsyncMock(return_value=0)) as execute,
        ):
            self.assertEqual(research.main(), 0)
        self.assertEqual(execute.await_args.args[0].command, "doctor")
        with tempfile.TemporaryDirectory() as directory:
            receipt = Path(directory) / "receipt.json"
            with (
                patch("sys.argv", ["crawl4ai_research", "qualify", "--receipt", str(receipt)]),
                patch.object(research, "execute", AsyncMock(return_value=1)) as execute,
            ):
                self.assertEqual(research.main(), 1)
            self.assertEqual(execute.await_args.args[0].receipt, receipt)

        def finish(coroutine):
            coroutine.close()
            return 7

        with (
            patch.object(
                research.argparse.ArgumentParser, "parse_args", return_value=SimpleNamespace(command="doctor")
            ),
            patch.object(research.asyncio, "run", side_effect=finish),
            self.assertRaises(SystemExit) as stopped,
        ):
            runpy.run_path(str(Path(research.__file__).resolve()), run_name="__main__")
        self.assertEqual(stopped.exception.code, 7)

    def test_result_rejects_challenges_empty_and_error_pages(self):
        """Purpose: Prevent false crawling success. Inputs: Result doubles. Outputs: Content/status validation."""
        result = SimpleNamespace(
            success=True,
            status_code=200,
            markdown=SimpleNamespace(raw_markdown="Python documentation " * 30),
            error_message="",
            response_headers={},
        )
        row = research.inspect_result(result, "Python")
        self.assertTrue(row["success"])
        self.assertEqual(len(row["markdown_sha256"]), 64)
        self.assertFalse(research.inspect_result(result, "missing marker")["success"])
        result.status_code = 403
        self.assertFalse(research.inspect_result(result)["success"])
        result.status_code = 200
        result.markdown.raw_markdown = "Verify you are human " * 20
        result.error_message = "Blocked by anti-bot protection: interactive challenge"
        self.assertFalse(research.inspect_result(result)["success"])
        result.error_message = ""
        result.markdown.raw_markdown = ""
        self.assertFalse(research.inspect_result(result)["success"])
        result.markdown.raw_markdown = "x" * (research.MAX_MARKDOWN_BYTES + 1)
        self.assertFalse(research.inspect_result(result)["success"])

    def test_native_access_evidence_is_distinct_from_article_words(self):
        """Purpose: Distinguish restrictions from articles. Inputs: Native fields. Outputs: Verified causes."""
        result = SimpleNamespace(
            success=True,
            status_code=200,
            redirected_status_code=None,
            response_headers={},
            error_message="",
            markdown=SimpleNamespace(raw_markdown="Documentation: verify you are human; checking your browser. " * 10),
        )
        self.assertTrue(research.inspect_result(result, "Documentation")["success"])
        for status, final, success, headers, error, category in (
            (307, 200, True, {}, "", ""),
            (200, 403, False, {}, "", "access_denied"),
            (403, None, False, {}, "Access denied by robots.txt", "robots_denied"),
            (403, None, False, {"X-Robots-Status": "Blocked by robots.txt"}, "", "robots_denied"),
            (307, 403, False, {}, "Blocked by anti-bot protection: Cloudflare JS challenge", "access_challenge"),
            (200, None, True, {"Cf-Mitigated": "challenge"}, "", "access_challenge"),
            (429, None, False, {}, "", "rate_limited"),
            (503, None, False, {}, "Service unavailable", "http_error"),
            (0, None, False, {}, "Browser closed", "native_failure"),
            (200, None, False, {}, "Extraction failed", "native_failure"),
        ):
            with self.subTest(category=category, status=status, final=final):
                result.status_code, result.redirected_status_code, result.success = status, final, success
                result.response_headers, result.error_message = headers, error
                row = research.inspect_result(result, "Documentation")
                self.assertEqual(row["failure_category"], category)
                self.assertEqual(row["success"], not category)
                self.assertEqual(row["http_status"], status)
                self.assertEqual(row["final_http_status"], final or status)
                self.assertEqual(row["error"], error)

    def test_capture_preserves_native_text_and_rejects_overwrite_or_oversize(self):
        """Purpose: Preserve inspectable evidence. Inputs: Native serialization. Outputs: Exact immutable capture."""
        native = {"url": "https://example.invalid/doc", "markdown": {"raw_markdown": "Prüfung"}, "success": True}
        result = SimpleNamespace(model_dump=Mock(return_value=native))
        with (
            tempfile.TemporaryDirectory() as directory,
            patch.object(research.importlib.metadata, "version", return_value=tool.VERSION),
        ):
            path = Path(directory) / "capture.json"
            identity = research.retain_capture(path, native["url"], result, {"success": True})
            payload = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(payload["native_result"], native)
            self.assertEqual(payload["provider_version"], tool.VERSION)
            self.assertEqual(identity["capture_sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
            result.model_dump.assert_called_once_with(mode="json", include=research.CAPTURE_FIELDS)
            with self.assertRaises(FileExistsError):
                research.retain_capture(path, native["url"], result, {"success": False})
            self.assertEqual(payload, json.loads(path.read_text(encoding="utf-8")))
            with patch.object(research, "MAX_CAPTURE_BYTES", 10), self.assertRaisesRegex(ValueError, "byte budget"):
                research.retain_capture(Path(directory) / "too-large.json", native["url"], result, {})
            self.assertFalse((Path(directory) / "too-large.json").exists())

    def test_timeout_retains_actionable_failure_without_guessing_bot_restriction(self):
        """Purpose: Separate deadlines from access denials. Inputs: Expired request. Outputs: Cause/evidence."""
        owner = SimpleNamespace(arun=AsyncMock(side_effect=TimeoutError))
        module = SimpleNamespace(CacheMode=SimpleNamespace(BYPASS="bypass"), CrawlerRunConfig=Mock())
        with (
            tempfile.TemporaryDirectory() as directory,
            patch.dict(sys.modules, {"crawl4ai": module}),
            patch.object(research.importlib.metadata, "version", return_value=tool.VERSION),
        ):
            path = Path(directory) / "timeout.json"
            row = asyncio.run(research.fetch(owner, "https://example.invalid/doc", capture=path))
            self.assertFalse(row["success"])
            self.assertEqual(row["failure_category"], "timeout")
            self.assertIn("45-second", row["error"])
            self.assertIsNone(json.loads(path.read_text(encoding="utf-8"))["native_result"])
            module.CrawlerRunConfig.assert_called_once_with(cache_mode="bypass", check_robots_txt=True)

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
