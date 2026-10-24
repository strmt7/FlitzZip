from __future__ import annotations

import contextlib
import ctypes
import io
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import venv
from ctypes import wintypes
from pathlib import Path
from unittest import mock

import flitzzip_mcp


class FailedReadPipe:
    def __init__(self, stream: io.BufferedReader) -> None:
        """Purpose: Inject a transport read failure while retaining the real owned pipe.
        Inputs: The newly spawned child's stream. Outputs: A cleanup-compatible proxy.
        """
        self.stream = stream

    def read(self, _size: int) -> bytes:
        """Purpose: Model failed output acquisition. Inputs: Requested extent. Outputs: Controlled I/O error."""
        raise OSError("controlled pipe read failure")

    def close(self) -> None:
        """Purpose: Preserve production cleanup. Inputs: None. Outputs: Closes the original pipe."""
        self.stream.close()

    @property
    def closed(self) -> bool:
        """Purpose: Observe actual cleanup. Inputs: None. Outputs: Original pipe closure state."""
        return self.stream.closed


class ProtocolTests(unittest.TestCase):
    def test_local_character_budget_preserves_protocol_default_and_byte_bounds(self) -> None:
        """Purpose: Separate local log retention from RPC clipping without removing byte bounds.
        Inputs: UTF-8 text larger than a normal response, explicit local character budgets and a short byte tail.
        Outputs: Default responses stay clipped; local text is exact; independent byte truncation remains visible.
        """
        text = "independent diagnostic " * 800 + "\u754c\U0001f9ea-tail"
        payload = text.encode("utf-8")
        default = flitzzip_mcp.BoundedOutput(65536, 65536)
        default.append("stdout", payload)
        rendered, truncated = default.render("stdout")
        self.assertEqual(len(rendered), 12000)
        self.assertTrue(rendered.endswith("\u754c\U0001f9ea-tail"))
        self.assertTrue(truncated)
        local = flitzzip_mcp.BoundedOutput(65536, 65536, character_limit=65536)
        local.append("stdout", payload)
        self.assertEqual(local.render("stdout"), (text, False))
        small = flitzzip_mcp.BoundedOutput(65536, 4, character_limit=65536)
        small.append("stdout", payload)
        self.assertEqual(small.render("stdout"), ("tail", True))
        exceeded = flitzzip_mcp.BoundedOutput(4, 4, character_limit=65536)
        exceeded.append("stdout", payload)
        self.assertTrue(exceeded.limit_exceeded.is_set())
        self.assertEqual(exceeded.render("stdout"), ("tail", True))
        for invalid in (0, -1, True, 1.5):
            with self.subTest(character_limit=invalid), self.assertRaises(ValueError):
                flitzzip_mcp.BoundedOutput(65536, 65536, character_limit=invalid)

    @unittest.skipUnless(os.name == "nt", "Windows suspended-thread admission contract")
    def test_primary_thread_admission_failures_close_only_owned_handles(self) -> None:
        """Purpose: Reject unavailable, ambiguous or changed primary threads without leaking enumeration handles.
        Inputs: Independent Windows API failures, ownership changes and suspend counts, plus a valid control.
        Outputs: Exact guard errors and ordered cleanup; no thread resumes before identity validation.
        """
        stages = {
            "snapshot": (OSError, "snapshot failed"),
            "enumeration": (OSError, "enumeration failed"),
            "empty": (RuntimeError, "exactly one"),
            "multiple": (RuntimeError, "exactly one"),
            "foreign": (RuntimeError, "exactly one"),
            "exited": (RuntimeError, "exactly one"),
            "open": (OSError, "thread open failed"),
            "owner": (RuntimeError, "ownership changed"),
            "resume-zero": (RuntimeError, "initial suspend count"),
            "resume-two": (RuntimeError, "initial suspend count"),
            "resume-error": (RuntimeError, "initial suspend count"),
            "valid": None,
        }
        for stage, expected_error in stages.items():
            with self.subTest(stage=stage):
                library = mock.Mock()
                process = mock.Mock(pid=1001)
                process.poll.return_value = 0 if stage == "exited" else None
                library.CreateToolhelp32Snapshot.return_value = ctypes.c_void_p(-1).value if stage == "snapshot" else 17
                entries = [] if stage == "empty" else [(1002 if stage == "foreign" else 1001, 271)]
                if stage == "multiple":
                    entries.append((1001, 272))
                records = iter(entries)

                def enumerate_thread(_snapshot, pointer, *, records=records):
                    """Purpose: Supply independently framed read-only thread records.
                    Inputs: Native entry buffer and frozen records. Outputs: One record or end of enumeration.
                    """
                    try:
                        owner, thread = next(records)
                    except StopIteration:
                        return False
                    entry = ctypes.cast(pointer, ctypes.POINTER(flitzzip_mcp._ThreadEntry)).contents
                    entry.process_id, entry.thread_id = owner, thread
                    return True

                library.Thread32First.side_effect = enumerate_thread
                library.Thread32Next.side_effect = enumerate_thread
                last_error = {"value": 5 if stage in ("snapshot", "enumeration") else 18}

                def open_thread(_access, _inherit, _thread, *, stage=stage, last_error=last_error):
                    """Purpose: Model stable thread-local error state rather than a consumable error sequence.
                    Inputs: Native open arguments and frozen failure stage. Outputs: Owned handle or access denial.
                    """
                    if stage == "open":
                        last_error["value"] = 5
                        return 0
                    return 19

                library.OpenThread.side_effect = open_thread
                library.GetProcessIdOfThread.return_value = 1002 if stage == "owner" else 1001
                library.ResumeThread.return_value = {
                    "resume-zero": 0,
                    "resume-two": 2,
                    "resume-error": 0xFFFFFFFF,
                }.get(stage, 1)
                with (
                    mock.patch.object(flitzzip_mcp.ctypes, "WinDLL", return_value=library),
                    mock.patch.object(
                        flitzzip_mcp.ctypes, "get_last_error", side_effect=lambda state=last_error: state["value"]
                    ),
                ):
                    if expected_error:
                        with self.assertRaisesRegex(*expected_error):
                            flitzzip_mcp.resume_owned_child(process)
                    else:
                        self.assertIsNone(flitzzip_mcp.resume_owned_child(process))
                thread_opened = stage in ("owner", "resume-zero", "resume-two", "resume-error", "valid")
                expected_closed = [] if stage == "snapshot" else ([19, 17] if thread_opened else [17])
                self.assertEqual(library.CloseHandle.call_args_list, [mock.call(handle) for handle in expected_closed])
                if stage.startswith("resume-") or stage == "valid":
                    library.ResumeThread.assert_called_once_with(19)
                else:
                    library.ResumeThread.assert_not_called()

    @unittest.skipUnless(os.name == "nt", "Windows job-object creation contract")
    def test_job_admission_failure_preserves_explicit_limits_and_closes_handles(self) -> None:
        """Purpose: Refuse unverified native memory containment rather than launching without an enforced cap.
        Inputs: Create/configure/query/verification/assignment failures and an independently framed valid job.
        Outputs: Every failure closes its created handle; successful creation retains the exact requested limits.
        """
        for stage in ("create", "configure", "query", "flags", "memory", "assign", "valid"):
            with self.subTest(stage=stage):
                library = mock.Mock()
                library.CreateJobObjectW.return_value = 0 if stage == "create" else 23
                library.SetInformationJobObject.return_value = stage != "configure"
                library.AssignProcessToJobObject.return_value = stage != "assign"
                library.TerminateJobObject.return_value = True

                def query(_handle, kind, pointer, _size, _returned, *, stage=stage):
                    """Purpose: Return independent job limits or zero active-process accounting.
                    Inputs: Native query buffer and frozen failure stage. Outputs: Controlled OS-query result.
                    """
                    if kind == 1:
                        info = ctypes.cast(
                            pointer, ctypes.POINTER(flitzzip_mcp._JobObjectBasicAccountingInformation)
                        ).contents
                        info.active_processes = 0
                        return True
                    info = ctypes.cast(
                        pointer, ctypes.POINTER(flitzzip_mcp._JobObjectExtendedLimitInformation)
                    ).contents
                    info.basic_limit_information.limit_flags = 0x2000 if stage == "flags" else 0x2200
                    info.job_memory_limit = 127 * flitzzip_mcp.MIB if stage == "memory" else 128 * flitzzip_mcp.MIB
                    return stage != "query"

                library.QueryInformationJobObject.side_effect = query
                process = mock.Mock(_handle=29)
                with (
                    mock.patch.object(flitzzip_mcp.ctypes, "WinDLL", return_value=library),
                    mock.patch.object(flitzzip_mcp.ctypes, "get_last_error", return_value=5),
                ):
                    if stage != "valid":
                        with self.assertRaisesRegex(RuntimeError, "child process containment failed"):
                            flitzzip_mcp.ChildContainment(process, 128 * flitzzip_mcp.MIB)
                    else:
                        owner = flitzzip_mcp.ChildContainment(process, 128 * flitzzip_mcp.MIB)
                        assigned = library.AssignProcessToJobObject.call_args.args
                        self.assertEqual((assigned[0], assigned[1].value), (23, 29))
                        configured = library.SetInformationJobObject.call_args.args[2]._obj
                        self.assertEqual(configured.job_memory_limit, 128 * flitzzip_mcp.MIB)
                        self.assertEqual(configured.basic_limit_information.limit_flags, 0x2200)
                        owner.close()
                self.assertEqual(library.CloseHandle.call_args_list, [] if stage == "create" else [mock.call(23)])

    def test_read_failure_retains_first_cause_and_refuses_partial_output(self) -> None:
        """Purpose: Prevent a secondary pipe shutdown from obscuring the original transport error.
        Inputs: Bounded captured bytes followed by opposite-channel I/O failures.
        Outputs: Both renderers reject incomplete evidence with the original exception as their cause.
        """
        for first, second in (("stdout", "stderr"), ("stderr", "stdout")):
            with self.subTest(first=first):
                output = flitzzip_mcp.BoundedOutput(100, 50)
                output.append("stdout", b"bounded prefix")
                original = OSError("independent first failure")
                output.record_read_failure(first, original)
                output.record_read_failure(second, OSError("secondary shutdown"))
                self.assertTrue(output.read_failure.is_set())
                self.assertEqual(output.tails["stdout"], b"bounded prefix")
                for rendered in ("stdout", "stderr"):
                    with self.assertRaisesRegex(RuntimeError, "child " + first + " pipe read failed") as failure:
                        output.render(rendered)
                    self.assertIs(failure.exception.__cause__, original)

    def test_job_close_confirms_zero_and_is_idempotent(self) -> None:
        """Purpose: Verify drain ordering. Inputs: Job accounting moving from one to zero. Outputs: Termination
        then one close.
        """
        library = mock.Mock()
        library.TerminateJobObject.return_value = True
        active = iter((1, 0))

        def query(handle, kind, result, size, returned):
            """Purpose: Supply owned accounting. Inputs: Typed query buffer. Outputs: Next controlled active-
            process count.
            """
            ctypes.cast(
                result, ctypes.POINTER(flitzzip_mcp._JobObjectBasicAccountingInformation)
            ).contents.active_processes = next(active)
            return True

        library.QueryInformationJobObject.side_effect = query
        owner = flitzzip_mcp.ChildContainment.__new__(flitzzip_mcp.ChildContainment)
        owner._handle, owner._kernel32 = 123, library
        with mock.patch.object(flitzzip_mcp.time, "sleep"):
            owner.close()
            owner.close()
        library.TerminateJobObject.assert_called_once_with(123, 1)
        library.CloseHandle.assert_called_once_with(123)
        self.assertEqual(library.QueryInformationJobObject.call_count, 2)

    def test_job_close_rejects_query_failure_and_timeout(self) -> None:
        """Purpose: Preserve shutdown failures. Inputs: Failed termination, invalid accounting or no progress.
        Outputs: Failure plus
        released owner.
        """
        for failure in ("query", "timeout", "termination"):
            with self.subTest(failure=failure):
                library = mock.Mock()
                library.TerminateJobObject.return_value = failure != "termination"

                def query(handle, kind, result, size, returned, *, fail=failure == "query"):
                    """Purpose: Model a shutdown failure. Inputs: Query buffer. Outputs: Failure or a persistently
                    active job.
                    """
                    ctypes.cast(
                        result, ctypes.POINTER(flitzzip_mcp._JobObjectBasicAccountingInformation)
                    ).contents.active_processes = 1
                    return not fail

                library.QueryInformationJobObject.side_effect = query
                owner = flitzzip_mcp.ChildContainment.__new__(flitzzip_mcp.ChildContainment)
                owner._handle, owner._kernel32 = 123, library
                expected = TimeoutError if failure == "timeout" else RuntimeError
                with (
                    mock.patch.object(flitzzip_mcp.time, "monotonic", side_effect=(0, 6)),
                    mock.patch.object(flitzzip_mcp.ctypes, "get_last_error", return_value=5),
                    self.assertRaises(expected),
                ):
                    owner.close()
                library.CloseHandle.assert_called_once_with(123)
                self.assertIsNone(owner._handle)

    def test_indirect_powershell_environment_is_child_only(self) -> None:
        """Purpose: Isolate PS5 discovery from PS7; inputs: mixed-case environment; outputs: no caller mutation."""
        values = {"PSMODULEPATH": "core-modules", "WinPSModulePath": "desktop-modules", "PATH": "unchanged"}
        with mock.patch.dict(os.environ, values, clear=True), mock.patch.object(flitzzip_mcp.os, "name", "nt"):
            caller = dict(os.environ)
            child = flitzzip_mcp.child_environment([r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"])
            self.assertEqual(child["PSModulePath"], "desktop-modules")
            self.assertNotIn("PSMODULEPATH", child)
            self.assertEqual(child["PATH"], "unchanged")
            self.assertEqual(dict(os.environ), caller)
            self.assertEqual(flitzzip_mcp.child_environment(["pwsh"]), caller)
        with (
            mock.patch.dict(os.environ, {"PSModulePath": "core-modules"}, clear=True),
            mock.patch.object(flitzzip_mcp.os, "name", "nt"),
        ):
            self.assertNotIn("PSModulePath", flitzzip_mcp.child_environment(["powershell"]))

    @unittest.skipUnless(os.name == "nt", "Windows PowerShell module/priority integration")
    def test_real_windows_powershell_discovers_builtin_hash_command(self) -> None:
        """Purpose: Regress MCP launch failure; inputs: polluted PS7 path; outputs: PS5 command and low priority."""
        with mock.patch.dict(os.environ, {"PSModulePath": "invalid-core-module-path", "WinPSModulePath": ""}):
            result = flitzzip_mcp.run_bounded_command(
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
            command = flitzzip_mcp.COMMANDS[name]
            self.assertEqual(command[command.index("-Checkpoint") + 1], "intermediate")
        for name in ("wait_relevant_workflows", "wait_relevant_workflows_opportunistic", "defer_relevant_workflows"):
            command = flitzzip_mcp.COMMANDS[name]
            self.assertEqual(command[command.index("-Mode") + 1], "opportunistic")
        final = flitzzip_mcp.COMMANDS["wait_final_commit_workflows"]
        self.assertEqual(final[final.index("-Mode") + 1], "final")
        self.assertIn("-FinalCommit", final)
        self.assertIn("-Full", final)
        self.assertNotIn("-SkipPostPushAudit", final)
        self.assertGreater(flitzzip_mcp.COMMAND_TIMEOUT_SECONDS["wait_final_commit_workflows"], 60 * 60)

    def test_lint_includes_new_files(self) -> None:
        """Purpose: Keep new source in MCP lint coverage; inputs: tool allowlist; outputs: required discovery flag."""
        self.assertIn("-IncludeUntracked", flitzzip_mcp.COMMANDS["lint"])

    def test_worker_start_failure_preserves_server_lifecycle(self) -> None:
        """Purpose: Keep command startup failures recoverable at the protocol boundary.
        Inputs: A rejected worker start followed by ping and close. Outputs: Internal error and no unstarted worker.
        """
        with mock.patch.object(threading.Thread, "start", side_effect=RuntimeError("controlled worker failure")):
            self.request("tools/call", {"name": "lint"})
        failure = json.loads(self.output.getvalue().splitlines()[-1])
        self.assertEqual(failure["error"]["code"], -32603)
        self.assertIsNone(self.server.active_id)
        self.assertIsNone(self.server.worker)
        self.request("ping", request_id=2)
        self.assertEqual(json.loads(self.output.getvalue().splitlines()[-1])["id"], 2)
        self.server.close()

    def setUp(self) -> None:
        """Purpose: Isolate each protocol test; inputs: none; outputs: server and captured transport."""
        self.server = flitzzip_mcp.ProtocolServer()
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
                flitzzip_mcp.META_PREFIX + "protocolVersion": flitzzip_mcp.PROTOCOL_VERSION,
                flitzzip_mcp.META_PREFIX + "clientCapabilities": {},
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
        self.assertEqual([item["name"] for item in definitions], sorted(flitzzip_mcp.COMMANDS))
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
                    flitzzip_mcp.META_PREFIX + "protocolVersion": "1900-01-01",
                    flitzzip_mcp.META_PREFIX + "clientCapabilities": {},
                }
            },
            3,
        )
        self.assertEqual(self.messages()[1]["error"]["code"], -32602)
        self.assertEqual(self.messages()[2]["error"]["code"], -32022)
        self.assertIn("2026-07-28", self.messages()[2]["error"]["data"]["supported"])

    def test_malformed_envelopes_and_arguments_do_not_execute(self) -> None:
        """Purpose: Reject invalid request shapes; inputs: malformed IDs, params and names; outputs: no command."""
        with mock.patch.object(flitzzip_mcp, "run_bounded_command") as command:
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
            with mock.patch.object(flitzzip_mcp, "run_bounded_command", return_value=output):
                self.request("tools/call", {"name": "test", "arguments": {}}, exit_code)
                self.server.worker.join(timeout=2)
            result = self.messages()[-1]["result"]
            self.assertEqual(result["structuredContent"], output)
            self.assertEqual(json.loads(result["content"][0]["text"]), output)
            self.assertEqual(result["isError"], exit_code != 0)

    def test_metadata_shape_rejection_preserves_stateless_discovery(self) -> None:
        """Purpose: Reject malformed per-request negotiation without initializing or executing commands.
        Inputs: Invalid metadata containers and missing/wrongly typed modern negotiation fields.
        Outputs: Exact invalid-params errors, followed by successful independent modern discovery.
        """
        prefix = flitzzip_mcp.META_PREFIX
        invalid = (
            None,
            [],
            "metadata",
            1,
            {prefix + "protocolVersion": flitzzip_mcp.PROTOCOL_VERSION},
            {prefix + "protocolVersion": 1, prefix + "clientCapabilities": {}},
            {prefix + "protocolVersion": flitzzip_mcp.PROTOCOL_VERSION, prefix + "clientCapabilities": []},
        )
        with mock.patch.object(flitzzip_mcp, "run_bounded_command") as command:
            for request_id, metadata in enumerate(invalid):
                self.request("tools/list", {"_meta": metadata}, request_id)
                response = self.messages()[-1]
                self.assertEqual(response["id"], request_id)
                self.assertEqual(response["error"]["code"], -32602)
                self.assertNotIn("result", response)
            self.assertFalse(self.server.initialized)
            self.request("server/discover", request_id="recovered")
            self.assertEqual(self.messages()[-1]["id"], "recovered")
            self.assertIn("capabilities", self.messages()[-1]["result"])
            command.assert_not_called()

    def test_invalid_and_duplicate_legacy_initialization_preserves_session(self) -> None:
        """Purpose: Keep failed negotiation from poisoning a valid legacy session or silently renegotiating it.
        Inputs: Independently malformed initialization fields, a valid handshake and a duplicate handshake.
        Outputs: Invalid-params/invalid-request errors; a subsequent bare ping retains the original session.
        """
        valid = {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}}
        for key, value in (
            ("protocolVersion", 1),
            ("capabilities", []),
            ("clientInfo", []),
            ("clientInfo", {"name": 1, "version": "1"}),
            ("clientInfo", {"name": "test"}),
        ):
            self.server.handle({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {**valid, key: value}})
            self.assertEqual(self.messages()[-1]["error"]["code"], -32602)
            self.assertFalse(self.server.initialized)
        self.server.handle({"jsonrpc": "2.0", "id": 2, "method": "initialize", "params": valid})
        self.server.handle({"jsonrpc": "2.0", "method": "notifications/initialized"})
        self.server.handle({"jsonrpc": "2.0", "id": 3, "method": "initialize", "params": valid})
        self.assertEqual(self.messages()[-1]["error"]["code"], -32600)
        self.assertEqual(self.server.legacy_version, "2025-11-25")
        self.server.handle({"jsonrpc": "2.0", "id": 4, "method": "ping"})
        self.assertEqual(self.messages()[-1], {"jsonrpc": "2.0", "id": 4, "result": {}})

    def test_unknown_methods_and_cursors_reject_without_command_execution(self) -> None:
        """Purpose: Reject unsupported routing and pagination without changing discovery or running a tool.
        Inputs: An unknown method and every representative non-null cursor type.
        Outputs: Specified errors and successful cursor-free listing after rejection.
        """
        with mock.patch.object(flitzzip_mcp, "run_bounded_command") as command:
            self.request("unknown/method")
            self.assertEqual(self.messages()[-1]["error"]["code"], -32601)
            for cursor in ("next", 0, False, [], {}):
                self.request("tools/list", {"cursor": cursor})
                self.assertEqual(self.messages()[-1]["error"]["code"], -32602)
            self.request("tools/list", {"cursor": None})
            self.assertTrue(self.messages()[-1]["result"]["tools"])
            command.assert_not_called()

    def test_runner_exception_is_a_tool_error_and_does_not_stall_reader(self) -> None:
        """Purpose: Preserve protocol responsiveness after a command transport failure.
        Inputs: An independently raised runner exception followed by a ping.
        Outputs: A tool error with the actual cause, no structured success and an idle, responsive session.
        """
        with mock.patch.object(
            flitzzip_mcp, "run_bounded_command", side_effect=RuntimeError("independent I/O failure")
        ):
            self.request("tools/call", {"name": "test", "arguments": {}}, "failed")
            self.server.worker.join(timeout=2)
        self.assertFalse(self.server.worker.is_alive())
        result = self.messages()[-1]["result"]
        self.assertTrue(result["isError"])
        self.assertNotIn("structuredContent", result)
        self.assertIn("independent I/O failure", result["content"][0]["text"])
        self.assertIsNone(self.server.active_id)
        self.request("ping", request_id="after-error")
        self.assertEqual(self.messages()[-1]["id"], "after-error")
        self.assertNotIn("error", self.messages()[-1])

    def test_stdio_drains_oversized_frames_and_recovers_after_blank_lines(self) -> None:
        """Purpose: Bound malformed input independently from JSON parsing and keep framing synchronized.
        Inputs: Blank lines, an oversized line with a valid next request, and an oversized unterminated EOF.
        Outputs: Exactly one oversize error per frame, successful next discovery and clean process shutdown.
        """
        request = {
            "jsonrpc": "2.0",
            "id": "after-oversize",
            "method": "server/discover",
            "params": {
                "_meta": {
                    flitzzip_mcp.META_PREFIX + "protocolVersion": flitzzip_mcp.PROTOCOL_VERSION,
                    flitzzip_mcp.META_PREFIX + "clientCapabilities": {},
                }
            },
        }
        oversized = "x" * (flitzzip_mcp.MAX_REQUEST_CHARACTERS + 17)
        for recover in (False, True):
            payload = "\n \t\n" + oversized + ("\n" + json.dumps(request) + "\n" if recover else "")
            result = subprocess.run(
                [sys.executable, str(Path(flitzzip_mcp.__file__))],
                input=payload,
                text=True,
                encoding="utf-8",
                capture_output=True,
                timeout=10,
                check=True,
            )
            messages = [json.loads(line) for line in result.stdout.splitlines()]
            self.assertEqual(result.stderr, "")
            self.assertEqual(messages[0]["error"]["code"], -32600)
            self.assertEqual(messages[0]["error"]["message"], "request exceeds input limit")
            self.assertEqual(len(messages), 2 if recover else 1)
            if recover:
                self.assertEqual(messages[1]["id"], "after-oversize")
                self.assertIn("capabilities", messages[1]["result"])

    def test_cancellation_keeps_reader_live_and_drops_cancelled_result(self) -> None:
        """Purpose: Cancel addressed work; inputs: active tool and busy request; outputs: no stale result."""
        started = threading.Event()

        def command(_argv: object, *, cancellation: threading.Event, timeout_seconds: float) -> dict:
            """Purpose: Simulate bounded work; inputs: cancellation/deadline; outputs: completion when cancelled."""
            self.assertGreater(timeout_seconds, 0)
            started.set()
            self.assertTrue(cancellation.wait(timeout=3))
            return {"exit_code": 1, "timed_out": False, "output_limit_exceeded": False}

        with mock.patch.object(flitzzip_mcp, "run_bounded_command", side_effect=command):
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
                    flitzzip_mcp.META_PREFIX + "protocolVersion": flitzzip_mcp.PROTOCOL_VERSION,
                    flitzzip_mcp.META_PREFIX + "clientCapabilities": {},
                }
            },
        }
        result = subprocess.run(
            [sys.executable, str(Path(flitzzip_mcp.__file__))],
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
            with mock.patch.object(flitzzip_mcp, "ROOT", root):
                self.assertEqual(flitzzip_mcp.command_working_directory(inside), inside)
                self.assertEqual(flitzzip_mcp.command_working_directory(None), root)
                for directory in (root.parent, root / "missing", root / ".." / "outside"):
                    with mock.patch.object(flitzzip_mcp.subprocess, "Popen") as child:
                        with self.assertRaises(ValueError):
                            flitzzip_mcp.run_bounded_command(["unused"], working_directory=directory)
                        child.assert_not_called()
                with mock.patch.object(Path, "is_junction", return_value=True), self.assertRaises(ValueError):
                    flitzzip_mcp.command_working_directory(inside)

    @unittest.skipUnless(os.name == "nt", "Windows bounded working-directory integration")
    def test_real_child_uses_snapshot_directory(self) -> None:
        """Purpose: Keep detector paths project-relative; inputs: owned checkout directory; outputs: exact child cwd."""
        with tempfile.TemporaryDirectory(dir=flitzzip_mcp.ROOT) as temporary:
            result = flitzzip_mcp.run_bounded_command(
                [sys.executable, "-c", "from pathlib import Path; print(Path.cwd())"],
                working_directory=Path(temporary),
                timeout_seconds=15,
            )
            self.assertEqual(result["exit_code"], 0, result["stderr"])
            self.assertEqual(Path(result["stdout"].strip()), Path(temporary))

    def test_invalid_limits_cannot_disable_deadlines_or_start_children(self) -> None:
        """Purpose: Prevent unbounded checks; inputs: nonfinite deadlines or invalid byte limits; outputs: no child."""
        cases = [{"timeout_seconds": value} for value in (float("nan"), float("inf"), -float("inf"), 0, -1, True, "1")]
        cases += [
            {name: value}
            for name in ("max_output_bytes", "response_tail_bytes", "response_max_characters")
            for value in (0, -1, True, 1.5)
        ]
        for limits in cases:
            with self.subTest(limits=limits), mock.patch.object(flitzzip_mcp.subprocess, "Popen") as child:
                with self.assertRaises(ValueError):
                    flitzzip_mcp.run_bounded_command(["unused"], **limits)
                child.assert_not_called()

    def test_shared_ram_policy_boundaries_and_invalid_inputs(self) -> None:
        """Purpose: Check portable admission arithmetic; inputs: synthetic MiB; outputs: exact budgets or refusal."""
        for available in (0, 2048, 4095):
            with self.subTest(available=available), self.assertRaisesRegex(RuntimeError, "Insufficient available RAM"):
                flitzzip_mcp.resolve_memory_budget_mib(available)
        for available in (4096, 4097, 8192, 32769, 1048576):
            with self.subTest(available=available):
                self.assertEqual(flitzzip_mcp.resolve_memory_budget_mib(available), available // 2)
        for available, required in ((True, 1), (-1, 1), (2.5, 1), (4096, False), (4096, 0), (4096, 1.5)):
            with self.subTest(available=available, required=required), self.assertRaises(ValueError):
                flitzzip_mcp.resolve_memory_budget_mib(available, required)
        self.assertEqual(flitzzip_mcp.resolve_memory_budget_mib(2049, 1), 1)

    @unittest.skipUnless(os.name == "nt", "Windows shared PowerShell RAM policy parity")
    def test_python_and_powershell_admission_are_identical(self) -> None:
        """Purpose: Prevent cross-language policy drift; inputs: same synthetic hosts; outputs: identical decisions."""
        script = (
            ". ./tools/local_resources.ps1; "
            "foreach ($available in @(0,2048,4095,4096,4097,8192,32769,1048576)) { "
            "try { Resolve-FlitzZipLocalMemoryBudget -AvailableMiB $available } catch { 'refused' } }"
        )
        result = flitzzip_mcp.run_bounded_command(["powershell", "-NoProfile", "-Command", script], timeout_seconds=15)
        self.assertEqual(result["exit_code"], 0, result["stderr"])
        self.assertEqual(result["stdout"].splitlines(), ["refused"] * 3 + ["2048", "2048", "4096", "16384", "524288"])

    def test_nonwindows_launch_refuses_uncontained_execution(self) -> None:
        """Purpose: Fail closed outside the supported command platform; inputs: POSIX platform; outputs: no child."""
        with (
            mock.patch.object(flitzzip_mcp.os, "name", "posix"),
            mock.patch.object(flitzzip_mcp.subprocess, "Popen") as child,
        ):
            with self.assertRaisesRegex(RuntimeError, "Windows aggregate job-memory containment"):
                flitzzip_mcp.run_bounded_command(["unused"])
            child.assert_not_called()

    @unittest.skipUnless(os.name == "nt", "Windows native counter admission")
    def test_invalid_or_overbudget_override_never_starts_child(self) -> None:
        """Purpose: Refuse RAM overrides before launch; inputs: malformed/oversized budgets; outputs: no child."""
        for budget in (0, -1, True, 1.5, 1 << 64):
            with self.subTest(budget=budget), mock.patch.object(flitzzip_mcp.subprocess, "Popen") as child:
                with self.assertRaises(ValueError):
                    flitzzip_mcp.run_bounded_command(["unused"], memory_limit_bytes=budget)
                child.assert_not_called()

    @unittest.skipUnless(os.name == "nt", "Windows real memory counter failure")
    def test_unavailable_ram_refuses_before_launch(self) -> None:
        """Purpose: Preserve counter failures; inputs: failed native RAM query; outputs: no fallback or child."""
        kernel32 = mock.Mock()
        kernel32.GlobalMemoryStatusEx.return_value = False
        with (
            mock.patch.object(flitzzip_mcp.ctypes, "WinDLL", return_value=kernel32),
            mock.patch.object(flitzzip_mcp.subprocess, "Popen") as child,
        ):
            with self.assertRaisesRegex(OSError, "available physical RAM query failed"):
                flitzzip_mcp.run_bounded_command(["unused"])
            child.assert_not_called()


@unittest.skipUnless(os.name == "nt", "Windows-native command execution and aggregate job memory")
class BoundedChildTests(unittest.TestCase):
    def test_delayed_termination_retries_wait_and_preserves_cancellation_status(self) -> None:
        """Purpose: Finish owned process cleanup when asynchronous termination outlasts the first wait.
        Inputs: Cancellation after verified admission and one controlled wait timeout.
        Outputs: Repeated owned termination, joined readers, closed pipes and a failure distinct from deadline expiry.
        """
        cancellation = threading.Event()
        process = mock.Mock(returncode=None)
        process.stdout, process.stderr = io.BytesIO(b"captured prefix"), io.BytesIO()
        process.poll.side_effect = lambda: process.returncode
        waits = []

        def wait(*, timeout):
            """Purpose: Model delayed process rundown without spending wall time or starting an unowned process.
            Inputs: Production wait deadline. Outputs: One timeout followed by a reaped failed process.
            """
            waits.append(timeout)
            if len(waits) == 1:
                raise subprocess.TimeoutExpired(["owned-unit-command"], timeout)
            process.returncode = 1
            return 1

        def resume(_process):
            """Purpose: Deliver cancellation after admission. Inputs: Owned suspended child. Outputs: Set event."""
            cancellation.set()

        process.wait.side_effect = wait
        owner = mock.Mock()
        with (
            mock.patch.object(flitzzip_mcp.subprocess, "Popen", return_value=process),
            mock.patch.object(flitzzip_mcp, "ChildContainment", return_value=owner) as containment,
            mock.patch.object(flitzzip_mcp, "resume_owned_child", side_effect=resume),
        ):
            result = flitzzip_mcp.run_bounded_command(
                ["owned-unit-command"],
                cancellation=cancellation,
                timeout_seconds=5,
                memory_limit_bytes=128 * flitzzip_mcp.MIB,
            )
        containment.assert_called_once_with(process, 128 * flitzzip_mcp.MIB)
        self.assertEqual(owner.terminate.call_args_list, [mock.call(process), mock.call(process)])
        owner.close.assert_called_once_with()
        self.assertEqual(waits, [5, 5, 5])
        self.assertTrue(process.stdout.closed)
        self.assertTrue(process.stderr.closed)
        self.assertEqual(result["exit_code"], 1)
        self.assertEqual(result["stdout"], "captured prefix")
        self.assertFalse(result["timed_out"])
        self.assertFalse(result["output_limit_exceeded"])

    def test_pipe_read_failure_terminates_owned_child_and_fails(self) -> None:
        """Purpose: Prevent failed output acquisition from masquerading as complete command evidence.
        Inputs: Real contained children with independently failed stdout or stderr reads.
        Outputs: A transport error after child termination and complete pipe cleanup, without waiting for timeout.
        """
        original_popen = subprocess.Popen
        cases = [
            (channel, script)
            for channel in ("stdout", "stderr")
            for script in ("print('unobserved output')", "import time; time.sleep(10)")
        ]
        for channel, script in cases:
            children = []

            def launch(*args, channel=channel, children=children, **kwargs):
                """Purpose: Inject only the stream fault after real suspended creation.
                Inputs: Unchanged production creation arguments. Outputs: Actual child with an owned pipe proxy.
                """
                process = original_popen(*args, **kwargs)
                setattr(process, channel, FailedReadPipe(getattr(process, channel)))
                children.append(process)
                return process

            try:
                with (
                    self.subTest(channel=channel, script=script),
                    mock.patch.object(flitzzip_mcp.subprocess, "Popen", side_effect=launch),
                    self.assertRaisesRegex(RuntimeError, "child " + channel + " pipe read failed"),
                ):
                    flitzzip_mcp.run_bounded_command(
                        [sys.executable, "-c", script],
                        timeout_seconds=2,
                        memory_limit_bytes=128 * flitzzip_mcp.MIB,
                    )
                self.assertEqual(len(children), 1)
                self.assertIsNotNone(children[0].poll())
                self.assertTrue(children[0].stdout.closed)
                self.assertTrue(children[0].stderr.closed)
            finally:
                for child in children:
                    if child.poll() is None:
                        child.kill()
                    child.wait(timeout=5)
                    child.stdout.close()
                    child.stderr.close()

    def test_reader_start_failure_reaps_child_and_closes_pipes(self) -> None:
        """Purpose: Regress partial reader startup leaks using an actual contained child.
        Inputs: A controlled first or second reader-start failure. Outputs: Reaped child and closed pipes before return.
        """
        for fail_at in (1, 2):
            with self.subTest(fail_at=fail_at):
                self.check_reader_start_failure(fail_at)

    def check_reader_start_failure(self, fail_at: int) -> None:
        """Purpose: Exercise one startup failure without leaking test resources.
        Inputs: One-based reader index to reject. Outputs: Asserts cleanup with independent test ownership.
        """
        create_child = subprocess.Popen
        create_owner = flitzzip_mcp.ChildContainment
        start_reader = threading.Thread.start
        children, owners, readers = [], [], []

        def launch(*args, **kwargs):
            """Purpose: Retain owned test processes; inputs: real launch arguments; outputs: actual child."""
            child = create_child(*args, **kwargs)
            children.append(child)
            return child

        def contain(*args, **kwargs):
            """Purpose: Retain test cleanup ownership; inputs: real child and limit; outputs: native job owner."""
            owner = create_owner(*args, **kwargs)
            owners.append(owner)
            return owner

        def start(reader):
            """Purpose: Fail one reader startup; inputs: actual thread; outputs: real start or controlled failure."""
            readers.append(reader)
            if len(readers) == fail_at:
                raise RuntimeError("controlled reader startup failure")
            start_reader(reader)

        try:
            with (
                mock.patch.object(flitzzip_mcp.subprocess, "Popen", side_effect=launch),
                mock.patch.object(flitzzip_mcp, "ChildContainment", side_effect=contain),
                mock.patch.object(threading.Thread, "start", start),
                self.assertRaisesRegex(RuntimeError, "controlled reader startup failure"),
            ):
                flitzzip_mcp.run_bounded_command(
                    [sys.executable, "-c", "import time; time.sleep(10)"],
                    timeout_seconds=2,
                    memory_limit_bytes=128 * flitzzip_mcp.MIB,
                )
            self.assertEqual(len(children), 1)
            self.assertIsNotNone(children[0].poll(), "reader failure leaked the running child")
            self.assertTrue(children[0].stdout.closed)
            self.assertTrue(children[0].stderr.closed)
        finally:
            for owner in owners:
                owner.close()
            for child in children:
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=5)
            for reader in readers:
                if reader.ident is not None:
                    reader.join(timeout=5)
            for child in children:
                child.stdout.close()
                child.stderr.close()

    def _assert_owned_memory_job(self, interpreter: str) -> None:
        """Purpose: Verify the actual owned job before resuming its child, independent of launcher-added jobs.
        Inputs: A real Python interpreter. Outputs: Exact OS-enforced limits, verified membership and child success.
        """
        original_containment = flitzzip_mcp.ChildContainment
        observed = []

        def inspect_assignment(process, memory_limit_bytes):
            """Purpose: Observe real assignment before the production runner resumes the suspended child.
            Inputs: Owned process and admitted memory. Outputs: Actual containment owner, or complete cleanup on error.
            """
            owner = original_containment(process, memory_limit_bytes)
            try:
                kernel32 = owner._kernel32
                information = flitzzip_mcp._JobObjectExtendedLimitInformation()
                self.assertTrue(
                    kernel32.QueryInformationJobObject(
                        owner._handle,
                        9,
                        ctypes.byref(information),
                        ctypes.sizeof(information),
                        None,
                    )
                )
                kernel32.IsProcessInJob.argtypes = [
                    wintypes.HANDLE,
                    wintypes.HANDLE,
                    ctypes.POINTER(wintypes.BOOL),
                ]
                kernel32.IsProcessInJob.restype = wintypes.BOOL
                member = wintypes.BOOL()
                self.assertTrue(
                    kernel32.IsProcessInJob(
                        wintypes.HANDLE(process._handle),
                        owner._handle,
                        ctypes.byref(member),
                    )
                )
                self.assertTrue(member.value)
                observed.append((information.job_memory_limit, information.basic_limit_information.limit_flags))
                return owner
            except BaseException:
                owner.close()
                raise

        with mock.patch.object(flitzzip_mcp, "ChildContainment", side_effect=inspect_assignment):
            result = flitzzip_mcp.run_bounded_command(
                [interpreter, "-c", "print('started')"],
                timeout_seconds=10,
                memory_limit_bytes=128 * flitzzip_mcp.MIB,
            )
        self.assertEqual(result["exit_code"], 0, result["stderr"])
        self.assertEqual(result["job_memory_limit_bytes"], 128 * flitzzip_mcp.MIB)
        self.assertEqual(result["stdout"].splitlines(), ["started"])
        self.assertEqual(observed, [(128 * flitzzip_mcp.MIB, 0x2200)])

    def test_memory_limit_is_applied_before_child_can_spawn(self) -> None:
        """Purpose: Verify startup containment. Inputs: Current Python. Outputs: Exact owned-job limits/membership."""
        self._assert_owned_memory_job(sys.executable)

    def test_aggregate_memory_denies_descendant_allocation(self) -> None:
        """Purpose: Verify real tree-wide denial. Inputs: Current Python.
        Outputs: Independent and aggregate allocation controls.
        """
        self._assert_aggregate_memory_denial(sys.executable)

    def _assert_aggregate_memory_denial(self, interpreter: str) -> None:
        """Purpose: Prove the cap covers parent and descendant together; inputs: two 48 MiB buffers under 96 MiB.
        Outputs: Each buffer fits alone; a nested buffer is denied while the first remains held, without host stress.
        """
        allocation = "import sys; b=bytearray(48*1024*1024); print('allocated',flush=True)"
        standalone = flitzzip_mcp.run_bounded_command(
            [interpreter, "-c", allocation], timeout_seconds=10, memory_limit_bytes=96 * flitzzip_mcp.MIB
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
        aggregate = flitzzip_mcp.run_bounded_command(
            [interpreter, "-c", parent, descendant], timeout_seconds=10, memory_limit_bytes=96 * flitzzip_mcp.MIB
        )
        self.assertEqual(aggregate["exit_code"], 42, aggregate)
        self.assertIn("memory denied", aggregate["stdout"])
        self.assertFalse(aggregate["timed_out"])

    def test_virtual_environment_launcher_retains_aggregate_memory_boundary(self) -> None:
        """Purpose: Cover a launcher that adds another Windows job without assuming that job's identity.
        Inputs: A fresh standard-library virtual environment without pip. Outputs: Exact ownership and real denial.
        """
        with tempfile.TemporaryDirectory(prefix="flitzzip-mcp-venv-") as temporary:
            venv.EnvBuilder(with_pip=False).create(temporary)
            interpreter = str(Path(temporary) / "Scripts/python.exe")
            self._assert_owned_memory_job(interpreter)
            self._assert_aggregate_memory_denial(interpreter)

    def test_failed_containment_never_executes_child(self) -> None:
        """Purpose: Prove fail-closed launch; inputs: injected assignment failure; outputs: no child marker or leak."""
        with tempfile.TemporaryDirectory(prefix="flitzzip-mcp-") as temporary:
            marker = Path(temporary) / "uncontained.txt"
            script = "import pathlib,sys; pathlib.Path(sys.argv[1]).write_text('ran')"
            with (
                mock.patch.object(flitzzip_mcp, "ChildContainment", side_effect=RuntimeError("test: containment")),
                self.assertRaisesRegex(RuntimeError, "test: containment"),
            ):
                flitzzip_mcp.run_bounded_command([sys.executable, "-c", script, str(marker)])
            self.assertFalse(marker.exists())

    def test_failed_resume_releases_contained_child(self) -> None:
        """Purpose: Prove launch unwinding; inputs: injected primary-thread failure; outputs: no command execution."""
        with (
            mock.patch.object(flitzzip_mcp, "resume_owned_child", side_effect=RuntimeError("test: resume")),
            self.assertRaisesRegex(RuntimeError, "test: resume"),
        ):
            flitzzip_mcp.run_bounded_command([sys.executable, "-c", "raise RuntimeError('must not run')"])

    def test_cancellation_terminates_real_child(self) -> None:
        """Purpose: Prove cancellation reaches the process owner; inputs: sleeping child; outputs: bounded exit."""
        cancellation = threading.Event()
        timer = threading.Timer(0.2, cancellation.set)
        started = time.monotonic()
        timer.start()
        try:
            result = flitzzip_mcp.run_bounded_command(
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

        result = flitzzip_mcp.run_bounded_command(
            [sys.executable, "-c", script],
            timeout_seconds=10,
            max_output_bytes=128 * 1024,
            response_tail_bytes=4096,
        )

        self.assertTrue(result["output_limit_exceeded"])
        self.assertTrue(result["output_truncated"])
        self.assertFalse(result["timed_out"])
        self.assertLessEqual(len(str(result["stdout"])), flitzzip_mcp.MAX_RESPONSE_CHARACTERS)
        self.assertLessEqual(len(str(result["stderr"])), flitzzip_mcp.MAX_RESPONSE_CHARACTERS)

    def test_timeout_terminates_child(self) -> None:
        """Purpose: Prove a silent child cannot outlive the configured MCP command deadline.
        Inputs: A child sleeps for ten seconds against a 0.2-second timeout.
        Outputs: Asserts timeout state and bounded wall time.
        """
        started = time.monotonic()

        result = flitzzip_mcp.run_bounded_command(
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
        with tempfile.TemporaryDirectory(prefix="flitzzip-mcp-") as temporary:
            marker = Path(temporary) / "descendant-survived.txt"
            descendant = "import pathlib,sys,time; time.sleep(1); pathlib.Path(sys.argv[1]).write_text('alive')"
            parent = (
                "import subprocess,sys,time; "
                "time.sleep(0.2); "
                "subprocess.Popen([sys.executable, '-c', sys.argv[1], sys.argv[2]]); "
                "sys.stdout.buffer.write(b'x' * (2 * 1024 * 1024))"
            )

            result = flitzzip_mcp.run_bounded_command(
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
