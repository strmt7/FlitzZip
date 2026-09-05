#!/usr/bin/env python3
"""Bounded stdio MCP server for SuperZip local operations.

This is intentionally small and local-only. It exposes a narrow command set so
future agents can run approved build/test/security tasks without inventing shell
commands that leak secrets or launch the GUI.
"""

from __future__ import annotations

import contextlib
import ctypes
import json
import os
import signal
import subprocess
import sys
import threading
import time
from collections.abc import Iterator, Sequence
from ctypes import wintypes
from pathlib import Path
from typing import BinaryIO

ROOT = Path(__file__).resolve().parents[1]
MAX_REQUEST_CHARACTERS = 1024 * 1024
MAX_CHILD_OUTPUT_BYTES = 16 * 1024 * 1024
MAX_RESPONSE_TAIL_BYTES = 64 * 1024
MAX_RESPONSE_CHARACTERS = 12_000
CHILD_TIMEOUT_SECONDS = 900.0
READ_CHUNK_BYTES = 64 * 1024
PROTOCOL_VERSION = "2026-07-28"
LEGACY_VERSIONS = ("2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05")
SERVER_INFO = {"name": "superzip-mcp", "version": "0.2.0"}
CAPABILITIES = {"tools": {"listChanged": False}}
META_PREFIX = "io.modelcontextprotocol/"
RESPONSE_LOCK = threading.Lock()
COMMANDS = {
    "verification_plan": [
        "powershell",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        "tools/verification_plan.ps1",
        "-IncludeUntracked",
    ],
    "verify_changes": [
        "powershell",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        "tools/verify_changes.ps1",
        "-IncludeUntracked",
    ],
    "lint": [
        "powershell",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        "tools/lint.ps1",
        "-CppMode",
        "Changed",
    ],
    "wait_relevant_workflows": [
        "powershell",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        "tools/wait_relevant_workflows.ps1",
        "-Mode",
        "final",
    ],
    "wait_relevant_workflows_opportunistic": [
        "powershell",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        "tools/wait_relevant_workflows.ps1",
        "-Mode",
        "opportunistic",
    ],
    "check_long_running_workflows": [
        "powershell",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        "tools/wait_relevant_workflows.ps1",
        "-Mode",
        "opportunistic",
        "-IncludeLongRunning",
    ],
    "wait_final_commit_workflows": [
        "powershell",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        "tools/wait_relevant_workflows.ps1",
        "-Mode",
        "final",
        "-FinalCommit",
    ],
    "defer_relevant_workflows": [
        "powershell",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        "tools/wait_relevant_workflows.ps1",
        "-Mode",
        "defer",
    ],
    "build": ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/build.ps1"],
    "build_hip": [
        "powershell",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        "tools/build.ps1",
        "-EnableHip",
    ],
    "test": ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools/test.ps1"],
    "security_scan": [
        "powershell",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        "tools/security_scan.ps1",
    ],
}


class _JobObjectBasicLimitInformation(ctypes.Structure):
    _fields_ = [
        ("per_process_user_time_limit", ctypes.c_longlong),
        ("per_job_user_time_limit", ctypes.c_longlong),
        ("limit_flags", wintypes.DWORD),
        ("minimum_working_set_size", ctypes.c_size_t),
        ("maximum_working_set_size", ctypes.c_size_t),
        ("active_process_limit", wintypes.DWORD),
        ("affinity", ctypes.c_size_t),
        ("priority_class", wintypes.DWORD),
        ("scheduling_class", wintypes.DWORD),
    ]


class _IoCounters(ctypes.Structure):
    _fields_ = [
        ("read_operation_count", ctypes.c_ulonglong),
        ("write_operation_count", ctypes.c_ulonglong),
        ("other_operation_count", ctypes.c_ulonglong),
        ("read_transfer_count", ctypes.c_ulonglong),
        ("write_transfer_count", ctypes.c_ulonglong),
        ("other_transfer_count", ctypes.c_ulonglong),
    ]


class _JobObjectExtendedLimitInformation(ctypes.Structure):
    _fields_ = [
        ("basic_limit_information", _JobObjectBasicLimitInformation),
        ("io_info", _IoCounters),
        ("process_memory_limit", ctypes.c_size_t),
        ("job_memory_limit", ctypes.c_size_t),
        ("peak_process_memory_used", ctypes.c_size_t),
        ("peak_job_memory_used", ctypes.c_size_t),
    ]


class ChildContainment:
    """Own process-tree containment for one allowlisted child command."""

    def __init__(self, process: subprocess.Popen[bytes]) -> None:
        """Purpose: Put a child in a kill-on-close Windows job or a dedicated POSIX process group.
        Inputs: process is the newly started allowlisted child.
        Outputs: Stores containment ownership or kills the child and raises if Windows containment cannot be applied.
        """
        self._handle: int | None = None
        self._kernel32: object | None = None
        if os.name != "nt":
            return
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        kernel32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        kernel32.SetInformationJobObject.restype = wintypes.BOOL
        kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
        kernel32.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
        kernel32.TerminateJobObject.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL

        handle = kernel32.CreateJobObjectW(None, None)
        information = _JobObjectExtendedLimitInformation()
        information.basic_limit_information.limit_flags = 0x00002000
        configured = handle and kernel32.SetInformationJobObject(
            handle,
            9,
            ctypes.byref(information),
            ctypes.sizeof(information),
        )
        assigned = configured and kernel32.AssignProcessToJobObject(handle, wintypes.HANDLE(process._handle))
        if not assigned:
            error = ctypes.get_last_error()
            if handle:
                kernel32.CloseHandle(handle)
            process.kill()
            process.wait(timeout=5)
            raise RuntimeError(f"child process containment failed ({error})")
        self._handle = handle
        self._kernel32 = kernel32

    def terminate(self, process: subprocess.Popen[bytes]) -> None:
        """Purpose: Terminate the contained child and every descendant.
        Inputs: process is the root child associated with this containment object.
        Outputs: Requests hard tree termination and falls back to killing the root process.
        """
        if self._handle is not None and self._kernel32 is not None:
            self._kernel32.TerminateJobObject(self._handle, 1)
        elif os.name != "nt":
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
        if process.poll() is None:
            process.kill()

    def close(self) -> None:
        """Purpose: Release containment and kill any descendants that outlived a completed root child.
        Inputs: None.
        Outputs: Closes the Windows kill-on-close job handle exactly once.
        """
        if self._handle is not None and self._kernel32 is not None:
            self._kernel32.CloseHandle(self._handle)
            self._handle = None


class BoundedOutput:
    """Retain fixed stdout/stderr tails while accounting for aggregate child output."""

    def __init__(self, output_limit: int, tail_limit: int) -> None:
        """Purpose: Initialize fixed child-output accounting.
        Inputs: output_limit caps aggregate bytes and tail_limit caps retained bytes per stream.
        Outputs: Creates empty synchronized tails and a limit event.
        """
        self.output_limit = output_limit
        self.tail_limit = tail_limit
        self.total_bytes = 0
        self.stream_bytes = {"stdout": 0, "stderr": 0}
        self.tails = {"stdout": bytearray(), "stderr": bytearray()}
        self.limit_exceeded = threading.Event()
        self.lock = threading.Lock()

    def append(self, channel: str, chunk: bytes) -> None:
        """Purpose: Account for one child-output chunk and retain only its bounded stream tail.
        Inputs: channel is stdout or stderr and chunk is newly read binary output.
        Outputs: Updates counters/tail and signals when aggregate output exceeds policy.
        """
        with self.lock:
            self.total_bytes += len(chunk)
            self.stream_bytes[channel] += len(chunk)
            tail = self.tails[channel]
            tail.extend(chunk)
            if len(tail) > self.tail_limit:
                del tail[: len(tail) - self.tail_limit]
            if self.total_bytes > self.output_limit:
                self.limit_exceeded.set()

    def render(self, channel: str) -> tuple[str, bool]:
        """Purpose: Decode one retained binary tail into a response-sized UTF-8 replacement string.
        Inputs: channel selects stdout or stderr.
        Outputs: Returns bounded text and whether bytes or decoded characters were omitted.
        """
        with self.lock:
            raw = bytes(self.tails[channel])
            byte_count = self.stream_bytes[channel]
        decoded = raw.decode("utf-8", errors="replace")
        truncated = byte_count > len(raw) or len(decoded) > MAX_RESPONSE_CHARACTERS
        return decoded[-MAX_RESPONSE_CHARACTERS:], truncated


def _drain_stream(stream: BinaryIO, channel: str, output: BoundedOutput) -> None:
    """Purpose: Drain one child pipe concurrently without retaining unbounded bytes.
    Inputs: stream is a binary pipe, channel identifies it, and output owns bounded accounting.
    Outputs: Reads until EOF or pipe closure and updates output for each fixed-size chunk.
    """
    try:
        while chunk := stream.read(READ_CHUNK_BYTES):
            output.append(channel, chunk)
    except OSError:
        pass


def run_bounded_command(
    command: Sequence[str],
    *,
    timeout_seconds: float = CHILD_TIMEOUT_SECONDS,
    max_output_bytes: int = MAX_CHILD_OUTPUT_BYTES,
    response_tail_bytes: int = MAX_RESPONSE_TAIL_BYTES,
    cancellation: threading.Event | None = None,
) -> dict[str, object]:
    """Purpose: Run one allowlisted command with process-tree, time, and streaming-output limits.
    Inputs: command is fixed argv; limits bound work; optional cancellation stops only this command's tree.
    Outputs: Returns exit code, fixed stream tails, truncation state, timeout state, and output-limit state.
    """
    if timeout_seconds <= 0 or max_output_bytes <= 0 or response_tail_bytes <= 0:
        raise ValueError("child command limits must be positive")
    creation_options: dict[str, object]
    if os.name == "nt":
        creation_options = {
            "creationflags": subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW,
        }
    else:
        creation_options = {"start_new_session": True}
    process = subprocess.Popen(
        list(command),
        cwd=ROOT,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        **creation_options,
    )
    containment = ChildContainment(process)
    output = BoundedOutput(max_output_bytes, response_tail_bytes)
    assert process.stdout is not None and process.stderr is not None
    readers = [
        threading.Thread(target=_drain_stream, args=(process.stdout, "stdout", output), daemon=True),
        threading.Thread(target=_drain_stream, args=(process.stderr, "stderr", output), daemon=True),
    ]
    for reader in readers:
        reader.start()

    timed_out = False
    deadline = time.monotonic() + timeout_seconds
    try:
        while process.poll() is None:
            if cancellation is not None and cancellation.is_set():
                containment.terminate(process)
                break
            if output.limit_exceeded.is_set():
                containment.terminate(process)
                break
            if time.monotonic() >= deadline:
                timed_out = True
                containment.terminate(process)
                break
            time.sleep(0.02)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            containment.terminate(process)
            process.wait(timeout=5)
    finally:
        containment.close()
        for reader in readers:
            reader.join(timeout=5)
        process.stdout.close()
        process.stderr.close()

    stdout, stdout_truncated = output.render("stdout")
    stderr, stderr_truncated = output.render("stderr")
    return {
        "exit_code": process.returncode,
        "stdout": stdout,
        "stderr": stderr,
        "output_truncated": stdout_truncated or stderr_truncated,
        "output_limit_exceeded": output.limit_exceeded.is_set(),
        "timed_out": timed_out,
    }


def respond(message_id: object, result: object = None, error: object = None) -> None:
    """Purpose: Emit one JSON-RPC response.
    Inputs: message_id is echoed from the request; result or error is serialized as JSON.
    Outputs: Writes a flushed response line to stdout.
    """
    payload = {"jsonrpc": "2.0", "id": message_id}
    if error is None:
        payload["result"] = result
    else:
        payload["error"] = error
    with RESPONSE_LOCK:
        print(json.dumps(payload, separators=(",", ":"), allow_nan=False), flush=True)


def tool_definitions() -> list[dict[str, object]]:
    """Purpose: Describe the fixed command surface for MCP clients.
    Inputs: None; COMMANDS supplies the executable allowlist.
    Outputs: Returns deterministic tool metadata with a closed, no-argument schema.
    """
    return [
        {
            "name": name,
            "description": f"Run the repository's {name.replace('_', ' ')} check with fixed arguments.",
            "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
        }
        for name in sorted(COMMANDS)
    ]


class ProtocolError(Exception):
    """A JSON-RPC failure carrying a stable code and optional protocol data."""

    def __init__(self, code: int, message: str, data: object = None) -> None:
        """Purpose: Construct a protocol failure; inputs are wire error fields; output is an exception."""
        super().__init__(message)
        self.payload: dict[str, object] = {"code": code, "message": message}
        if data is not None:
            self.payload["data"] = data


class ProtocolServer:
    """Serve modern per-request MCP and legacy handshakes with at most one running command."""

    def __init__(self) -> None:
        """Purpose: Initialize transport-local legacy and command state; inputs: none; output: idle server."""
        self.legacy_version: str | None = None
        self.initialized = False
        self.worker: threading.Thread | None = None
        self.active_id: str | int | None = None
        self.cancellation = threading.Event()
        self.lock = threading.Lock()

    def reply(self, message_id: str | int, result: dict, modern: bool) -> None:
        """Purpose: Emit version-correct results; inputs: request ID, body and era; output: one response."""
        if modern:
            result = {**result, "resultType": "complete", "_meta": {META_PREFIX + "serverInfo": SERVER_INFO}}
        respond(message_id, result)

    def request_era(self, params: dict) -> bool:
        """Purpose: Resolve request-local protocol metadata without borrowing modern connection state.
        Inputs: params is one validated JSON object.
        Outputs: Returns modern/legacy mode or raises a version/parameter error.
        """
        meta = params.get("_meta", {})
        if not isinstance(meta, dict):
            raise ProtocolError(-32602, "_meta must be an object")
        if any(key.startswith(META_PREFIX) for key in meta):
            version = meta.get(META_PREFIX + "protocolVersion")
            if not isinstance(version, str) or not isinstance(meta.get(META_PREFIX + "clientCapabilities"), dict):
                raise ProtocolError(-32602, "protocolVersion and clientCapabilities metadata are required")
            if version != PROTOCOL_VERSION:
                raise ProtocolError(
                    -32022,
                    "Unsupported protocol version",
                    {
                        "supported": [PROTOCOL_VERSION, *LEGACY_VERSIONS],
                        "requested": version,
                    },
                )
            return True
        if not self.initialized:
            raise ProtocolError(-32602, "request needs protocol metadata or a completed legacy initialization")
        return False

    def initialize(self, message_id: str | int, params: dict) -> None:
        """Purpose: Negotiate legacy clients; inputs: ID and initialization fields; output: server capabilities."""
        version = params.get("protocolVersion")
        client = params.get("clientInfo")
        if (
            not isinstance(version, str)
            or not isinstance(params.get("capabilities"), dict)
            or not isinstance(client, dict)
            or not isinstance(client.get("name"), str)
            or not isinstance(client.get("version"), str)
        ):
            raise ProtocolError(-32602, "invalid initialization parameters")
        if self.legacy_version is not None:
            raise ProtocolError(-32600, "legacy initialization already received")
        self.legacy_version = version if version in LEGACY_VERSIONS else LEGACY_VERSIONS[0]
        self.reply(
            message_id,
            {"protocolVersion": self.legacy_version, "capabilities": CAPABILITIES, "serverInfo": SERVER_INFO},
            False,
        )

    def notification(self, method: str, params: dict) -> None:
        """Purpose: Consume one-way lifecycle/cancellation messages; inputs: method and params; output: no response."""
        if method == "notifications/initialized" and self.legacy_version is not None:
            self.initialized = True
        elif method == "notifications/cancelled":
            requested = params.get("requestId")
            with self.lock:
                if type(requested) in (str, int) and requested == self.active_id:
                    self.cancellation.set()

    def execute(self, message_id: str | int, name: str, modern: bool) -> None:
        """Purpose: Run one command off the reader thread while accepting cancellation.
        Inputs: request ID, validated tool name and protocol era.
        Outputs: Emits an MCP tool result unless cancelled; releases the active slot on every exit.
        """
        try:
            try:
                output = run_bounded_command(COMMANDS[name], cancellation=self.cancellation)
                failed = output["exit_code"] != 0 or output["timed_out"] or output["output_limit_exceeded"]
                result = {
                    "content": [{"type": "text", "text": json.dumps(output, separators=(",", ":"))}],
                    "isError": bool(failed),
                }
                if modern or self.legacy_version in ("2025-11-25", "2025-06-18"):
                    result["structuredContent"] = output
            except Exception as exc:  # noqa: BLE001 - command failure belongs in the tool result
                result = {"content": [{"type": "text", "text": f"tool execution failed: {exc}"}], "isError": True}
            with self.lock:
                if not self.cancellation.is_set():
                    self.reply(message_id, result, modern)
        finally:
            with self.lock:
                self.active_id = None

    def call_tool(self, message_id: str | int, params: dict, modern: bool) -> None:
        """Purpose: Start a fixed tool without creating an unbounded queue.
        Inputs: validated request ID, params and protocol era.
        Outputs: Starts one worker or raises a parameter/busy error; never accepts shell arguments.
        """
        name = params.get("name")
        if not isinstance(name, str) or name not in COMMANDS:
            raise ProtocolError(-32602, "unknown tool")
        if params.get("arguments", {}) != {}:
            raise ProtocolError(-32602, "this tool accepts only an empty arguments object")
        with self.lock:
            if self.active_id is not None:
                raise ProtocolError(-32600, "another command is running; wait or cancel it first")
            self.active_id = message_id
            self.cancellation.clear()
            self.worker = threading.Thread(target=self.execute, args=(message_id, name, modern))
            try:
                self.worker.start()
            except RuntimeError:
                self.active_id = None
                raise

    def handle(self, request: object) -> None:
        """Purpose: Dispatch one MCP message with strict JSON-RPC envelope handling.
        Inputs: request is parsed JSON, not necessarily an object.
        Outputs: Sends at most one immediate response, consumes notifications, or starts a bounded command.
        """
        if (
            not isinstance(request, dict)
            or request.get("jsonrpc") != "2.0"
            or not isinstance(request.get("method"), str)
        ):
            respond(None, error={"code": -32600, "message": "invalid JSON-RPC request"})
            return
        message_id = request.get("id")
        params = request.get("params", {})
        if "id" not in request:
            if isinstance(params, dict):
                self.notification(request["method"], params)
            return
        if type(message_id) not in (str, int):
            respond(None, error={"code": -32600, "message": "request ID must be a string or integer"})
            return
        try:
            if not isinstance(params, dict):
                raise ProtocolError(-32602, "params must be an object")
            method = request["method"]
            if method == "initialize":
                self.initialize(message_id, params)
                return
            modern = self.request_era(params)
            if method == "server/discover" and modern:
                self.reply(
                    message_id,
                    {"supportedVersions": [PROTOCOL_VERSION, *LEGACY_VERSIONS], "capabilities": CAPABILITIES},
                    modern,
                )
            elif method == "ping":
                self.reply(message_id, {}, modern)
            elif method == "tools/list":
                if params.get("cursor") is not None:
                    raise ProtocolError(-32602, "invalid cursor: tool list is not paginated")
                self.reply(message_id, {"tools": tool_definitions()}, modern)
            elif method == "tools/call":
                self.call_tool(message_id, params, modern)
            else:
                raise ProtocolError(-32601, "method not found")
        except ProtocolError as exc:
            respond(message_id, error=exc.payload)

    def close(self) -> None:
        """Purpose: Stop owned work on EOF; inputs: none; output: cancelled command tree and joined worker."""
        with self.lock:
            self.cancellation.set()
        if self.worker is not None:
            self.worker.join()


def iter_bounded_request_lines() -> Iterator[str]:
    """Purpose: Read newline-delimited requests without allowing one line to consume unbounded memory.
    Inputs: stdin is the local JSON-RPC transport.
    Outputs: Yields bounded complete lines and emits an error while draining every oversized line.
    """
    while line := sys.stdin.readline(MAX_REQUEST_CHARACTERS + 1):
        if len(line) > MAX_REQUEST_CHARACTERS:
            while not line.endswith("\n"):
                line = sys.stdin.readline(MAX_REQUEST_CHARACTERS + 1)
                if not line:
                    break
            respond(None, error={"code": -32600, "message": "request exceeds input limit"})
            continue
        yield line


def main() -> int:
    """Purpose: Process newline-delimited JSON-RPC requests until stdin closes.
    Inputs: stdin supplies one JSON object per line.
    Outputs: Returns a process exit code after all requests are handled.
    """
    sys.stdin.reconfigure(encoding="utf-8", errors="strict")
    sys.stdout.reconfigure(encoding="utf-8", errors="strict")
    server = ProtocolServer()
    try:
        for line in iter_bounded_request_lines():
            if not line.strip():
                continue
            try:
                server.handle(json.loads(line))
            except (ValueError, RecursionError):
                respond(None, error={"code": -32700, "message": "invalid JSON"})
    finally:
        server.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
