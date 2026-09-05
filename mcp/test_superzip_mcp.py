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

from mcp import superzip_mcp


class ProtocolTests(unittest.TestCase):
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

        def command(_argv: object, *, cancellation: threading.Event) -> dict:
            """Purpose: Simulate cancellable work; inputs: cancellation event; outputs: completion when cancelled."""
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


class BoundedChildTests(unittest.TestCase):
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
