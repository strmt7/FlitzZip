from __future__ import annotations

import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

import superzip_mcp


class ProtocolTests(unittest.TestCase):
    def test_indirect_powershell_environment_is_child_only(self) -> None:
        """Purpose: Isolate PS5 discovery from PS7; inputs: mixed-case environment; outputs: no caller mutation."""
        values = {"PSMODULEPATH": "core-modules", "WinPSModulePath": "desktop-modules", "PATH": "unchanged"}
        with mock.patch.dict(os.environ, values, clear=True), mock.patch.object(superzip_mcp.os, "name", "nt"):
            caller = dict(os.environ)
            child = superzip_mcp.child_environment([r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"])
            self.assertEqual(child["PSModulePath"], "desktop-modules")
            self.assertNotIn("PSMODULEPATH", child)
            self.assertEqual(child["PATH"], "unchanged")
            self.assertEqual(dict(os.environ), caller)
            self.assertEqual(superzip_mcp.child_environment(["pwsh"]), caller)
        with (
            mock.patch.dict(os.environ, {"PSModulePath": "core-modules"}, clear=True),
            mock.patch.object(superzip_mcp.os, "name", "nt"),
        ):
            self.assertNotIn("PSModulePath", superzip_mcp.child_environment(["powershell"]))

    @unittest.skipUnless(os.name == "nt", "Windows PowerShell module/priority integration")
    def test_real_windows_powershell_discovers_builtin_hash_command(self) -> None:
        """Purpose: Regress MCP launch failure; inputs: polluted PS7 path; outputs: PS5 command and low priority."""
        with mock.patch.dict(os.environ, {"PSModulePath": "invalid-core-module-path", "WinPSModulePath": ""}):
            result = superzip_mcp.run_bounded_command(
                [
                    "powershell",
                    "-NoProfile",
                    "-Command",
                    "Get-Command Get-FileHash -ErrorAction Stop | Select-Object -ExpandProperty Name; "
                    "[Diagnostics.Process]::GetCurrentProcess().PriorityClass.ToString()",
                ],
                timeout_seconds=15,
            )
        self.assertEqual(result["exit_code"], 0, result["stderr"])
        self.assertEqual(result["stdout"].splitlines(), ["Get-FileHash", "BelowNormal"])

    def test_workflow_commands_preserve_checkpoint_intent(self) -> None:
        """Purpose: Prevent blocking intermediate tools; inputs: allowlist; outputs: final gates remain explicit."""
        for name in ("verification_plan", "verify_changes"):
            command = superzip_mcp.COMMANDS[name]
            self.assertEqual(command[command.index("-Checkpoint") + 1], "intermediate")
        for name in ("wait_relevant_workflows", "wait_relevant_workflows_opportunistic", "defer_relevant_workflows"):
            command = superzip_mcp.COMMANDS[name]
            self.assertEqual(command[command.index("-Mode") + 1], "opportunistic")
        final = superzip_mcp.COMMANDS["wait_final_commit_workflows"]
        self.assertEqual(final[final.index("-Mode") + 1], "final")
        self.assertIn("-FinalCommit", final)
        self.assertIn("-Full", final)
        self.assertNotIn("-SkipPostPushAudit", final)
        self.assertGreater(superzip_mcp.COMMAND_TIMEOUT_SECONDS["wait_final_commit_workflows"], 60 * 60)

    def setUp(self) -> None:
        """Purpose: Isolate each protocol test; inputs: none; outputs: server and captured transport."""
        self.server = superzip_mcp.ProtocolServer()
        self.output = io.StringIO()
        self.redirect = contextlib.redirect_stdout(self.output)
        self.redirect.__enter__()

    def tearDown(self) -> None:
        """Purpose: Release only test-owned state; inputs: none; outputs: joined worker and restored stdout."""
        self.server.close()
        self.redirect.__exit__(None, None, None)

    def request(self, method: str, params: dict | None = None, request_id: object = 1) -> None:
        """Purpose: Send a modern request; inputs: method, params, ID; outputs: protocol effects."""
        values = {
            "_meta": {
                superzip_mcp.META_PREFIX + "protocolVersion": superzip_mcp.PROTOCOL_VERSION,
                superzip_mcp.META_PREFIX + "clientCapabilities": {},
            }
        }
        values.update(params or {})
        self.server.handle({"jsonrpc": "2.0", "id": request_id, "method": method, "params": values})

    def messages(self) -> list[dict]:
        """Purpose: Decode captured wire lines; inputs: current output; outputs: JSON response objects."""
        return [json.loads(line) for line in self.output.getvalue().splitlines()]

    def test_modern_discovery_and_tool_schemas(self) -> None:
        """Purpose: Prove stateless discovery works; inputs: fresh modern requests; outputs: complete tool metadata."""
        self.request("server/discover")
        self.request("tools/list", request_id=2)
        discovery, listing = self.messages()
        self.assertIn("2026-07-28", discovery["result"]["supportedVersions"])
        self.assertIn("tools", discovery["result"]["capabilities"])
        self.assertEqual(discovery["result"]["resultType"], "complete")
        definitions = listing["result"]["tools"]
        self.assertEqual([item["name"] for item in definitions], sorted(superzip_mcp.COMMANDS))
        for item in definitions:
            self.assertEqual(item["inputSchema"]["type"], "object")
            self.assertFalse(item["inputSchema"]["additionalProperties"])
            self.assertTrue(item["description"])

    def test_legacy_handshake_and_notifications(self) -> None:
        """Purpose: Preserve legacy setup; inputs: handshake and notification; outputs: no notification reply."""
        self.server.handle(
            {
                "jsonrpc": "2.0",
                "id": 0,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {"name": "test", "version": "1"},
                },
            }
        )
        self.server.handle({"jsonrpc": "2.0", "method": "notifications/initialized"})
        self.server.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/list"})
        messages = self.messages()
        self.assertEqual(len(messages), 2)
        self.assertEqual(messages[0]["result"]["protocolVersion"], "2025-11-25")
        self.assertNotIn("resultType", messages[1]["result"])
        self.assertTrue(messages[1]["result"]["tools"])

    def test_version_metadata_does_not_leak_between_requests(self) -> None:
        """Purpose: Keep modern requests independent; inputs: versioned then bare requests; outputs: explicit errors."""
        self.request("ping")
        self.server.handle({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        self.request(
            "ping",
            {
                "_meta": {
                    superzip_mcp.META_PREFIX + "protocolVersion": "1900-01-01",
                    superzip_mcp.META_PREFIX + "clientCapabilities": {},
                }
            },
            3,
        )
        self.assertEqual(self.messages()[1]["error"]["code"], -32602)
        self.assertEqual(self.messages()[2]["error"]["code"], -32022)
        self.assertIn("2026-07-28", self.messages()[2]["error"]["data"]["supported"])

    def test_malformed_envelopes_and_arguments_do_not_execute(self) -> None:
        """Purpose: Reject invalid request shapes; inputs: malformed IDs, params and names; outputs: no command."""
        with mock.patch.object(superzip_mcp, "run_bounded_command") as command:
            for value in ([], None, 2, {"method": "tools/list"}):
                self.server.handle(value)
            for value in (None, True, [], 1.5):
                self.request("tools/list", request_id=value)
            for value in (None, [], "test", {"unexpected": 1}):
                self.request("tools/call", {"name": "test", "arguments": value})
            self.request("tools/call", {"name": []})
            self.server.handle({"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": []})
            command.assert_not_called()
        self.assertTrue(all("error" in message for message in self.messages()))

    def test_tool_results_use_mcp_content_and_execution_errors(self) -> None:
        """Purpose: Expose structured command outcomes; inputs: success/failure outputs; outputs: MCP tool results."""
        for exit_code in (0, 1):
            output = {
                "exit_code": exit_code,
                "stdout": "done",
                "stderr": "",
                "timed_out": False,
                "output_limit_exceeded": False,
                "output_truncated": False,
            }
            with mock.patch.object(superzip_mcp, "run_bounded_command", return_value=output):
                self.request("tools/call", {"name": "test", "arguments": {}}, exit_code)
                self.server.worker.join(timeout=2)
            result = self.messages()[-1]["result"]
            self.assertEqual(result["structuredContent"], output)
            self.assertEqual(json.loads(result["content"][0]["text"]), output)
            self.assertEqual(result["isError"], exit_code != 0)

    def test_cancellation_keeps_reader_live_and_drops_cancelled_result(self) -> None:
        """Purpose: Cancel addressed work; inputs: active tool and busy request; outputs: no stale result."""
        started = threading.Event()

        def command(_argv: object, *, cancellation: threading.Event, timeout_seconds: float) -> dict:
            """Purpose: Simulate bounded work; inputs: cancellation/deadline; outputs: completion when cancelled."""
            self.assertGreater(timeout_seconds, 0)
            started.set()
            self.assertTrue(cancellation.wait(timeout=3))
            return {"exit_code": 1, "timed_out": False, "output_limit_exceeded": False}

        with mock.patch.object(superzip_mcp, "run_bounded_command", side_effect=command):
            self.request("tools/call", {"name": "test"}, 10)
            self.assertTrue(started.wait(timeout=2))
            self.request("ping", request_id=11)
            self.request("tools/call", {"name": "test"}, 12)
            self.server.handle({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": 99}})
            self.assertFalse(self.server.cancellation.is_set())
            self.server.handle({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": 10}})
            self.server.worker.join(timeout=2)
        self.assertEqual([item["id"] for item in self.messages()], [11, 12])
        self.assertEqual(self.messages()[-1]["error"]["code"], -32600)

    def test_stdio_recovers_after_parse_error(self) -> None:
        """Purpose: Exercise stdio; inputs: bad JSON then discovery; outputs: error followed by a valid result."""
        request = {
            "jsonrpc": "2.0",
            "id": "probe",
            "method": "server/discover",
            "params": {
                "_meta": {
                    superzip_mcp.META_PREFIX + "protocolVersion": superzip_mcp.PROTOCOL_VERSION,
                    superzip_mcp.META_PREFIX + "clientCapabilities": {},
                }
            },
        }
        result = subprocess.run(
            [sys.executable, str(Path(superzip_mcp.__file__))],
            input="{\n" + json.dumps(request) + "\n",
            text=True,
            encoding="utf-8",
            capture_output=True,
            timeout=5,
            check=True,
        )
        error, discovery = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(error["error"]["code"], -32700)
        self.assertEqual(discovery["id"], "probe")


class MemoryAdmissionTests(unittest.TestCase):
    def test_working_directory_rejects_escapes_and_redirects(self) -> None:
        """Purpose: Preserve checkout containment; inputs: paths and redirects; outputs: refusals before launch."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inside = root / "inputs"
            inside.mkdir()
            with mock.patch.object(superzip_mcp, "ROOT", root):
                self.assertEqual(superzip_mcp.command_working_directory(inside), inside)
                self.assertEqual(superzip_mcp.command_working_directory(None), root)
                for directory in (root.parent, root / "missing", root / ".." / "outside"):
                    with mock.patch.object(superzip_mcp.subprocess, "Popen") as child:
                        with self.assertRaises(ValueError):
                            superzip_mcp.run_bounded_command(["unused"], working_directory=directory)
                        child.assert_not_called()
                with mock.patch.object(Path, "is_junction", return_value=True), self.assertRaises(ValueError):
                    superzip_mcp.command_working_directory(inside)

    @unittest.skipUnless(os.name == "nt", "Windows bounded working-directory integration")
    def test_real_child_uses_snapshot_directory(self) -> None:
        """Purpose: Keep detector paths project-relative; inputs: owned checkout directory; outputs: exact child cwd."""
        with tempfile.TemporaryDirectory(dir=superzip_mcp.ROOT) as temporary:
            result = superzip_mcp.run_bounded_command(
                [sys.executable, "-c", "from pathlib import Path; print(Path.cwd())"],
                working_directory=Path(temporary),
                timeout_seconds=15,
            )
            self.assertEqual(result["exit_code"], 0, result["stderr"])
            self.assertEqual(Path(result["stdout"].strip()), Path(temporary))

    def test_invalid_limits_cannot_disable_deadlines_or_start_children(self) -> None:
        """Purpose: Prevent unbounded checks; inputs: nonfinite deadlines or invalid byte limits; outputs: no child."""
        cases = [{"timeout_seconds": value} for value in (float("nan"), float("inf"), -float("inf"), 0, -1, True, "1")]
        cases += [{name: value} for name in ("max_output_bytes", "response_tail_bytes") for value in (0, -1, True, 1.5)]
        for limits in cases:
            with self.subTest(limits=limits), mock.patch.object(superzip_mcp.subprocess, "Popen") as child:
                with self.assertRaises(ValueError):
                    superzip_mcp.run_bounded_command(["unused"], **limits)
                child.assert_not_called()

    def test_shared_ram_policy_boundaries_and_invalid_inputs(self) -> None:
        """Purpose: Check portable admission arithmetic; inputs: synthetic MiB; outputs: exact budgets or refusal."""
        for available in (0, 2048, 4095):
            with self.subTest(available=available), self.assertRaisesRegex(RuntimeError, "Insufficient available RAM"):
                superzip_mcp.resolve_memory_budget_mib(available)
        for available in (4096, 4097, 8192, 32769, 1048576):
            with self.subTest(available=available):
                self.assertEqual(superzip_mcp.resolve_memory_budget_mib(available), available // 2)
        for available, required in ((True, 1), (-1, 1), (2.5, 1), (4096, False), (4096, 0), (4096, 1.5)):
            with self.subTest(available=available, required=required), self.assertRaises(ValueError):
                superzip_mcp.resolve_memory_budget_mib(available, required)
        self.assertEqual(superzip_mcp.resolve_memory_budget_mib(2049, 1), 1)

    @unittest.skipUnless(os.name == "nt", "Windows shared PowerShell RAM policy parity")
    def test_python_and_powershell_admission_are_identical(self) -> None:
        """Purpose: Prevent cross-language policy drift; inputs: same synthetic hosts; outputs: identical decisions."""
        script = (
            ". ./tools/local_resources.ps1; "
            "foreach ($available in @(0,2048,4095,4096,4097,8192,32769,1048576)) { "
            "try { Resolve-SuperZipLocalMemoryBudget -AvailableMiB $available } catch { 'refused' } }"
        )
        result = superzip_mcp.run_bounded_command(["powershell", "-NoProfile", "-Command", script], timeout_seconds=15)
        self.assertEqual(result["exit_code"], 0, result["stderr"])
        self.assertEqual(result["stdout"].splitlines(), ["refused"] * 3 + ["2048", "2048", "4096", "16384", "524288"])

    def test_nonwindows_launch_refuses_uncontained_execution(self) -> None:
        """Purpose: Fail closed outside the supported command platform; inputs: POSIX platform; outputs: no child."""
        with (
            mock.patch.object(superzip_mcp.os, "name", "posix"),
            mock.patch.object(superzip_mcp.subprocess, "Popen") as child,
        ):
            with self.assertRaisesRegex(RuntimeError, "Windows aggregate job-memory containment"):
                superzip_mcp.run_bounded_command(["unused"])
            child.assert_not_called()

    @unittest.skipUnless(os.name == "nt", "Windows native counter admission")
    def test_invalid_or_overbudget_override_never_starts_child(self) -> None:
        """Purpose: Refuse RAM overrides before launch; inputs: malformed/oversized budgets; outputs: no child."""
        for budget in (0, -1, True, 1.5, 1 << 64):
            with self.subTest(budget=budget), mock.patch.object(superzip_mcp.subprocess, "Popen") as child:
                with self.assertRaises(ValueError):
                    superzip_mcp.run_bounded_command(["unused"], memory_limit_bytes=budget)
                child.assert_not_called()

    @unittest.skipUnless(os.name == "nt", "Windows real memory counter failure")
    def test_unavailable_ram_refuses_before_launch(self) -> None:
        """Purpose: Preserve counter failures; inputs: failed native RAM query; outputs: no fallback or child."""
        kernel32 = mock.Mock()
        kernel32.GlobalMemoryStatusEx.return_value = False
        with (
            mock.patch.object(superzip_mcp.ctypes, "WinDLL", return_value=kernel32),
            mock.patch.object(superzip_mcp.subprocess, "Popen") as child,
        ):
            with self.assertRaisesRegex(OSError, "available physical RAM query failed"):
                superzip_mcp.run_bounded_command(["unused"])
            child.assert_not_called()


@unittest.skipUnless(os.name == "nt", "Windows-native command execution and aggregate job memory")
class BoundedChildTests(unittest.TestCase):
    def test_memory_limit_is_applied_before_child_can_spawn(self) -> None:
        """Purpose: Verify startup containment; inputs: child querying its job; outputs: exact shared ceiling."""
        script = (
            "import ctypes,sys; from ctypes import wintypes; sys.path.insert(0,'mcp'); "
            "from superzip_mcp import _JobObjectExtendedLimitInformation; "
            "k=ctypes.WinDLL('kernel32',use_last_error=True); "
            "k.QueryInformationJobObject.argtypes=[wintypes.HANDLE,ctypes.c_int,"
            "ctypes.c_void_p,wintypes.DWORD,ctypes.c_void_p]; "
            "v=_JobObjectExtendedLimitInformation(); "
            "assert k.QueryInformationJobObject(None,9,ctypes.byref(v),ctypes.sizeof(v),None); "
            "print(v.job_memory_limit); print(v.basic_limit_information.limit_flags)"
        )
        result = superzip_mcp.run_bounded_command(
            [sys.executable, "-c", script], timeout_seconds=10, memory_limit_bytes=128 * superzip_mcp.MIB
        )
        self.assertEqual(result["exit_code"], 0, result["stderr"])
        self.assertEqual(result["job_memory_limit_bytes"], 128 * superzip_mcp.MIB)
        self.assertEqual(result["stdout"].splitlines(), [str(128 * superzip_mcp.MIB), str(0x2200)])

    def test_aggregate_memory_denies_descendant_allocation(self) -> None:
        """Purpose: Prove the cap covers parent and descendant together; inputs: two 48 MiB buffers under 96 MiB.
        Outputs: Each buffer fits alone; a nested buffer is denied while the first remains held, without host stress.
        """
        allocation = "import sys; b=bytearray(48*1024*1024); print('allocated',flush=True)"
        standalone = superzip_mcp.run_bounded_command(
            [sys.executable, "-c", allocation], timeout_seconds=10, memory_limit_bytes=96 * superzip_mcp.MIB
        )
        self.assertEqual(standalone["exit_code"], 0, standalone["stderr"])
        descendant = (
            "import sys\n"
            "try: b=bytearray(48*1024*1024)\n"
            "except MemoryError: print('memory denied',flush=True); sys.exit(42)\n"
            "print('unexpected allocation',flush=True); sys.exit(0)"
        )
        parent = (
            "import subprocess,sys; b=bytearray(48*1024*1024); "
            "p=subprocess.run([sys.executable,'-c',sys.argv[1]]); sys.exit(p.returncode)"
        )
        aggregate = superzip_mcp.run_bounded_command(
            [sys.executable, "-c", parent, descendant], timeout_seconds=10, memory_limit_bytes=96 * superzip_mcp.MIB
        )
        self.assertEqual(aggregate["exit_code"], 42, aggregate)
        self.assertIn("memory denied", aggregate["stdout"])
        self.assertFalse(aggregate["timed_out"])

    def test_failed_containment_never_executes_child(self) -> None:
        """Purpose: Prove fail-closed launch; inputs: injected assignment failure; outputs: no child marker or leak."""
        with tempfile.TemporaryDirectory(prefix="superzip-mcp-") as temporary:
            marker = Path(temporary) / "uncontained.txt"
            script = "import pathlib,sys; pathlib.Path(sys.argv[1]).write_text('ran')"
            with (
                mock.patch.object(superzip_mcp, "ChildContainment", side_effect=RuntimeError("test: containment")),
                self.assertRaisesRegex(RuntimeError, "test: containment"),
            ):
                superzip_mcp.run_bounded_command([sys.executable, "-c", script, str(marker)])
            self.assertFalse(marker.exists())

    def test_failed_resume_releases_contained_child(self) -> None:
        """Purpose: Prove launch unwinding; inputs: injected primary-thread failure; outputs: no command execution."""
        with (
            mock.patch.object(superzip_mcp, "resume_owned_child", side_effect=RuntimeError("test: resume")),
            self.assertRaisesRegex(RuntimeError, "test: resume"),
        ):
            superzip_mcp.run_bounded_command([sys.executable, "-c", "raise RuntimeError('must not run')"])

    def test_cancellation_terminates_real_child(self) -> None:
        """Purpose: Prove cancellation reaches the process owner; inputs: sleeping child; outputs: bounded exit."""
        cancellation = threading.Event()
        timer = threading.Timer(0.2, cancellation.set)
        started = time.monotonic()
        timer.start()
        try:
            result = superzip_mcp.run_bounded_command(
                [sys.executable, "-c", "import time; time.sleep(10)"],
                timeout_seconds=10,
                cancellation=cancellation,
            )
        finally:
            timer.cancel()
            timer.join()
        self.assertNotEqual(result["exit_code"], 0)
        self.assertFalse(result["timed_out"])
        self.assertLess(time.monotonic() - started, 5)

    def test_combined_output_limit_terminates_noisy_child(self) -> None:
        """Purpose: Prove child output is stopped while streaming rather than truncated after full buffering.
        Inputs: A Python child emits 8 MiB on each captured stream against a 128 KiB aggregate limit.
        Outputs: Asserts limit termination, response truncation metadata, and bounded returned text.
        """
        script = (
            "import sys; "
            "sys.stdout.buffer.write(b'o' * (8 * 1024 * 1024)); "
            "sys.stderr.buffer.write(b'e' * (8 * 1024 * 1024))"
        )

        result = superzip_mcp.run_bounded_command(
            [sys.executable, "-c", script],
            timeout_seconds=10,
            max_output_bytes=128 * 1024,
            response_tail_bytes=4096,
        )

        self.assertTrue(result["output_limit_exceeded"])
        self.assertTrue(result["output_truncated"])
        self.assertFalse(result["timed_out"])
        self.assertLessEqual(len(str(result["stdout"])), superzip_mcp.MAX_RESPONSE_CHARACTERS)
        self.assertLessEqual(len(str(result["stderr"])), superzip_mcp.MAX_RESPONSE_CHARACTERS)

    def test_timeout_terminates_child(self) -> None:
        """Purpose: Prove a silent child cannot outlive the configured MCP command deadline.
        Inputs: A child sleeps for ten seconds against a 0.2-second timeout.
        Outputs: Asserts timeout state and bounded wall time.
        """
        started = time.monotonic()

        result = superzip_mcp.run_bounded_command(
            [sys.executable, "-c", "import time; time.sleep(10)"],
            timeout_seconds=0.2,
            max_output_bytes=1024,
            response_tail_bytes=1024,
        )

        self.assertTrue(result["timed_out"])
        self.assertLess(time.monotonic() - started, 5.0)

    @unittest.skipUnless(os.name == "nt", "Windows job-object containment regression")
    def test_output_limit_terminates_descendant_process(self) -> None:
        """Purpose: Prove output-limit termination includes descendants, not only the direct PowerShell-like child.
        Inputs: A parent spawns a delayed marker writer after containment, then exceeds the output budget.
        Outputs: Asserts the descendant never writes its marker after tree termination.
        """
        with tempfile.TemporaryDirectory(prefix="superzip-mcp-") as temporary:
            marker = Path(temporary) / "descendant-survived.txt"
            descendant = "import pathlib,sys,time; time.sleep(1); pathlib.Path(sys.argv[1]).write_text('alive')"
            parent = (
                "import subprocess,sys,time; "
                "time.sleep(0.2); "
                "subprocess.Popen([sys.executable, '-c', sys.argv[1], sys.argv[2]]); "
                "sys.stdout.buffer.write(b'x' * (2 * 1024 * 1024))"
            )

            result = superzip_mcp.run_bounded_command(
                [sys.executable, "-c", parent, descendant, str(marker)],
                timeout_seconds=10,
                max_output_bytes=64 * 1024,
                response_tail_bytes=4096,
            )
            time.sleep(1.2)

            self.assertTrue(result["output_limit_exceeded"])
            self.assertFalse(marker.exists())


if __name__ == "__main__":
    unittest.main()
