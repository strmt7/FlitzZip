#!/usr/bin/env python3
"""Bounded stdio MCP server for SuperZip local operations.

This is intentionally small and local-only. It exposes a narrow command set so
future agents can run approved build/test/security tasks without inventing shell
commands that leak secrets or launch the GUI.
"""

from __future__ import annotations

import ctypes
import json
import math
import os
import subprocess
import sys
import threading
import time
from collections.abc import Iterator, Sequence
from ctypes import wintypes
from pathlib import Path, PureWindowsPath
from typing import BinaryIO

ROOT = Path(__file__).resolve().parents[1]
MAX_REQUEST_CHARACTERS = 1024 * 1024
MAX_CHILD_OUTPUT_BYTES = 16 * 1024 * 1024
MAX_RESPONSE_TAIL_BYTES = 64 * 1024
MAX_RESPONSE_CHARACTERS = 12_000
CHILD_TIMEOUT_SECONDS = 900.0
COMMAND_TIMEOUT_SECONDS = {"verify_changes": 3600.0, "wait_final_commit_workflows": 4500.0}
READ_CHUNK_BYTES = 64 * 1024
MIB = 1024 * 1024
CREATE_SUSPENDED = 0x00000004
JOB_OBJECT_LIMIT_JOB_MEMORY = 0x00000200
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
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
        "-Checkpoint",
        "intermediate",
    ],
    "verify_changes": [
        "powershell",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        "tools/verify_changes.ps1",
        "-IncludeUntracked",
        "-Checkpoint",
        "intermediate",
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
        "-IncludeUntracked",
    ],
    "wait_relevant_workflows": [
        "powershell",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        "tools/wait_relevant_workflows.ps1",
        "-Mode",
        "opportunistic",
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
        "-Full",
    ],
    "defer_relevant_workflows": [
        "powershell",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        "tools/wait_relevant_workflows.ps1",
        "-Mode",
        "opportunistic",
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


class _JobObjectBasicAccountingInformation(ctypes.Structure):
    """Typed Windows accounting result used to confirm complete owned-tree shutdown."""

    _fields_ = [
        ("total_user_time", ctypes.c_longlong),
        ("total_kernel_time", ctypes.c_longlong),
        ("period_user_time", ctypes.c_longlong),
        ("period_kernel_time", ctypes.c_longlong),
        ("total_page_fault_count", wintypes.DWORD),
        ("total_processes", wintypes.DWORD),
        ("active_processes", wintypes.DWORD),
        ("total_terminated_processes", wintypes.DWORD),
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


class _MemoryStatus(ctypes.Structure):
    _fields_ = [
        ("length", wintypes.DWORD),
        ("memory_load", wintypes.DWORD),
        ("total_physical", ctypes.c_ulonglong),
        ("available_physical", ctypes.c_ulonglong),
        ("total_page_file", ctypes.c_ulonglong),
        ("available_page_file", ctypes.c_ulonglong),
        ("total_virtual", ctypes.c_ulonglong),
        ("available_virtual", ctypes.c_ulonglong),
        ("available_extended_virtual", ctypes.c_ulonglong),
    ]


class _ThreadEntry(ctypes.Structure):
    _fields_ = [
        ("size", wintypes.DWORD),
        ("usage", wintypes.DWORD),
        ("thread_id", wintypes.DWORD),
        ("process_id", wintypes.DWORD),
        ("base_priority", wintypes.LONG),
        ("delta_priority", wintypes.LONG),
        ("flags", wintypes.DWORD),
    ]


def resolve_memory_budget_mib(available_mib: int, required_mib: int = 2048) -> int:
    """Purpose: Mirror the shared PowerShell RAM admission policy with integer arithmetic.
    Inputs: available_mib is free physical RAM; required_mib is the positive minimum safe job budget.
    Outputs: Returns at most half free RAM while retaining 2 GiB; raises on unknown or insufficient capacity.
    """
    if type(available_mib) is not int or available_mib < 0 or type(required_mib) is not int or required_mib <= 0:
        raise ValueError("RAM admission requires nonnegative available and positive required integer MiB")
    budget = max(0, available_mib - max(2048, (available_mib + 1) // 2))
    if budget < required_mib:
        raise RuntimeError(
            f"Insufficient available RAM for local work: {available_mib} MiB available, "
            f"{budget} MiB admitted, {required_mib} MiB required."
        )
    return budget


def admit_child_memory_bytes(requested_bytes: int | None = None) -> int:
    """Purpose: Sample current Windows RAM before launching an owned command tree.
    Inputs: Optional smaller positive internal test budget; callers cannot exceed ordinary shared-policy admission.
    Outputs: Returns job committed-memory ceiling; raises before launch if counters, capacity, or platform fail.
    """
    if os.name != "nt":
        raise RuntimeError("SuperZip command execution requires Windows aggregate job-memory containment")
    if requested_bytes is not None and (type(requested_bytes) is not int or requested_bytes <= 0):
        raise ValueError("requested job memory must be a positive integer byte count")
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GlobalMemoryStatusEx.argtypes = [ctypes.POINTER(_MemoryStatus)]
    kernel32.GlobalMemoryStatusEx.restype = wintypes.BOOL
    status = _MemoryStatus(length=ctypes.sizeof(_MemoryStatus))
    if not kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        raise OSError(ctypes.get_last_error(), "available physical RAM query failed")
    budget_bytes = resolve_memory_budget_mib(status.available_physical // MIB) * MIB
    if requested_bytes is not None and requested_bytes > budget_bytes:
        raise ValueError("requested job memory exceeds current shared-policy RAM admission")
    return budget_bytes if requested_bytes is None else requested_bytes


def resume_owned_child(process: subprocess.Popen[bytes]) -> None:
    """Purpose: Resume only the primary thread of our newly created, contained suspended Windows child.
    Inputs: process retains its live process handle; no child code has executed yet.
    Outputs: Resumes one verified owned thread or raises; never resumes a thread not verified as owned.
    """
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.Thread32First.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ThreadEntry)]
    kernel32.Thread32First.restype = wintypes.BOOL
    kernel32.Thread32Next.argtypes = kernel32.Thread32First.argtypes
    kernel32.Thread32Next.restype = wintypes.BOOL
    kernel32.OpenThread.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenThread.restype = wintypes.HANDLE
    kernel32.GetProcessIdOfThread.argtypes = [wintypes.HANDLE]
    kernel32.GetProcessIdOfThread.restype = wintypes.DWORD
    kernel32.ResumeThread.argtypes = [wintypes.HANDLE]
    kernel32.ResumeThread.restype = wintypes.DWORD
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL
    snapshot = kernel32.CreateToolhelp32Snapshot(0x00000004, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        raise OSError(ctypes.get_last_error(), "owned child thread snapshot failed")
    try:
        entry = _ThreadEntry(size=ctypes.sizeof(_ThreadEntry))
        found = kernel32.Thread32First(snapshot, ctypes.byref(entry))
        thread_ids = []
        while found:
            if entry.process_id == process.pid:
                thread_ids.append(entry.thread_id)
            entry.size = ctypes.sizeof(_ThreadEntry)
            found = kernel32.Thread32Next(snapshot, ctypes.byref(entry))
        if ctypes.get_last_error() != 18:
            raise OSError(ctypes.get_last_error(), "owned child thread enumeration failed")
        if len(thread_ids) != 1 or process.poll() is not None:
            raise RuntimeError("suspended child must have exactly one live owned thread")
        # Query ownership through the opened handle as well as the read-only snapshot.
        thread = kernel32.OpenThread(0x00000802, False, thread_ids[0])
        if not thread:
            raise OSError(ctypes.get_last_error(), "owned child thread open failed")
        try:
            if kernel32.GetProcessIdOfThread(thread) != process.pid:
                raise RuntimeError("suspended child thread ownership changed")
            if kernel32.ResumeThread(thread) != 1:
                raise RuntimeError("owned child primary thread did not have one initial suspend count")
        finally:
            kernel32.CloseHandle(thread)
    finally:
        kernel32.CloseHandle(snapshot)


class ChildContainment:
    """Own process-tree containment for one allowlisted child command."""

    def __init__(self, process: subprocess.Popen[bytes], memory_limit_bytes: int) -> None:
        """Purpose: Put a suspended child in a verified memory-limited, kill-on-close Windows job.
        Inputs: process is our newly created suspended child; memory_limit_bytes is RAM-admitted committed memory.
        Outputs: Stores containment or raises; the launch owner must terminate and reap on failure before resuming.
        """
        self._handle: int | None = None
        self._kernel32: object | None = None
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        kernel32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        kernel32.SetInformationJobObject.restype = wintypes.BOOL
        kernel32.QueryInformationJobObject.argtypes = [
            wintypes.HANDLE,
            ctypes.c_int,
            ctypes.c_void_p,
            wintypes.DWORD,
            ctypes.c_void_p,
        ]
        kernel32.QueryInformationJobObject.restype = wintypes.BOOL
        kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
        kernel32.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
        kernel32.TerminateJobObject.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL

        handle = kernel32.CreateJobObjectW(None, None)
        information = _JobObjectExtendedLimitInformation()
        information.basic_limit_information.limit_flags = (
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_JOB_MEMORY
        )
        information.job_memory_limit = memory_limit_bytes
        configured = handle and kernel32.SetInformationJobObject(
            handle,
            9,
            ctypes.byref(information),
            ctypes.sizeof(information),
        )
        actual = _JobObjectExtendedLimitInformation()
        queried = configured and kernel32.QueryInformationJobObject(
            handle, 9, ctypes.byref(actual), ctypes.sizeof(actual), None
        )
        verified = (
            queried
            and actual.basic_limit_information.limit_flags == information.basic_limit_information.limit_flags
            and actual.job_memory_limit == memory_limit_bytes
        )
        assigned = verified and kernel32.AssignProcessToJobObject(handle, wintypes.HANDLE(process._handle))
        if not assigned:
            error = ctypes.get_last_error()
            if handle:
                kernel32.CloseHandle(handle)
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
        if process.poll() is None:
            process.kill()

    def close(self) -> None:
        """Purpose: Terminate and drain the owned process tree before releasing containment.
        Inputs: None.
        Outputs: Confirms zero active processes within five seconds, or raises; always closes the job exactly once.
        """
        if self._handle is not None and self._kernel32 is not None:
            try:
                if not self._kernel32.TerminateJobObject(self._handle, 1):
                    raise RuntimeError(f"owned job termination failed ({ctypes.get_last_error()})")
                deadline = time.monotonic() + 5
                while True:
                    accounting = _JobObjectBasicAccountingInformation()
                    if not self._kernel32.QueryInformationJobObject(
                        self._handle, 1, ctypes.byref(accounting), ctypes.sizeof(accounting), None
                    ):
                        raise RuntimeError(f"owned job shutdown query failed ({ctypes.get_last_error()})")
                    if accounting.active_processes == 0:
                        break
                    if time.monotonic() >= deadline:
                        raise TimeoutError("owned process tree did not finish termination within five seconds")
                    time.sleep(0.01)
            finally:
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


def child_environment(command: Sequence[str]) -> dict[str, str]:
    """Purpose: Prevent indirect Windows PowerShell launches from inheriting incompatible PS7 modules.
    Inputs: command is fixed argv; the caller environment is copied, never changed.
    Outputs: Returns child-only environment; honors WinPSModulePath or lets Windows PowerShell construct defaults.
    """
    environment = dict(os.environ)
    if (
        os.name != "nt"
        or not command
        or PureWindowsPath(command[0]).name.casefold() not in {"powershell", "powershell.exe"}
    ):
        return environment
    legacy_path = next((value for name, value in environment.items() if name.casefold() == "winpsmodulepath"), "")
    for name in list(environment):
        if name.casefold() == "psmodulepath":
            del environment[name]
    if legacy_path:
        environment["PSModulePath"] = legacy_path
    return environment


def command_working_directory(directory: Path | None) -> Path:
    """Purpose: Keep child working directories inside this checkout without reparse redirects.
    Inputs: Optional existing directory; None retains the checkout root.
    Outputs: Absolute directory or a refusal before any child is started.
    """
    candidate = Path(os.path.abspath(ROOT if directory is None else directory))
    try:
        relative = candidate.relative_to(ROOT)
    except ValueError:
        raise ValueError("child working directory must stay inside the checkout") from None
    current = ROOT
    for part in ("", *relative.parts):
        current /= part
        if current.is_symlink() or current.is_junction():
            raise ValueError("child working directory crosses a reparse point")
    if not candidate.is_dir():
        raise ValueError("child working directory must be an existing directory")
    return candidate


def run_bounded_command(
    command: Sequence[str],
    *,
    timeout_seconds: float = CHILD_TIMEOUT_SECONDS,
    max_output_bytes: int = MAX_CHILD_OUTPUT_BYTES,
    response_tail_bytes: int = MAX_RESPONSE_TAIL_BYTES,
    cancellation: threading.Event | None = None,
    memory_limit_bytes: int | None = None,
    working_directory: Path | None = None,
) -> dict[str, object]:
    """Purpose: Run one allowlisted command with aggregate memory, process-tree, time, and streaming-output limits.
    Inputs: Fixed argv, positive limits and checkout-contained directory; cancellation stops only this tree.
    Outputs: Returns bounded outcome and admitted job-memory bytes; no child runs before verified containment.
    """
    if (
        type(timeout_seconds) not in (int, float)
        or not math.isfinite(timeout_seconds)
        or timeout_seconds <= 0
        or type(max_output_bytes) is not int
        or max_output_bytes <= 0
        or type(response_tail_bytes) is not int
        or response_tail_bytes <= 0
    ):
        raise ValueError("child command limits require a finite positive deadline and positive integer byte counts")
    admitted_bytes = admit_child_memory_bytes(memory_limit_bytes)
    child_directory = command_working_directory(working_directory)
    output = BoundedOutput(max_output_bytes, response_tail_bytes)
    creation_options = {
        "creationflags": subprocess.CREATE_NEW_PROCESS_GROUP
        | subprocess.CREATE_NO_WINDOW
        | subprocess.BELOW_NORMAL_PRIORITY_CLASS
        | CREATE_SUSPENDED,
    }
    process = subprocess.Popen(
        list(command),
        cwd=child_directory,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=child_environment(command),
        **creation_options,
    )
    containment = None
    try:
        containment = ChildContainment(process, admitted_bytes)
        resume_owned_child(process)
    except BaseException:
        try:
            if containment is not None:
                containment.close()
        finally:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            if process.stdout is not None:
                process.stdout.close()
            if process.stderr is not None:
                process.stderr.close()
        raise
    readers: list[threading.Thread] = []
    timed_out = False
    try:
        assert process.stdout is not None and process.stderr is not None
        readers = [
            threading.Thread(target=_drain_stream, args=(process.stdout, "stdout", output), daemon=True),
            threading.Thread(target=_drain_stream, args=(process.stderr, "stderr", output), daemon=True),
        ]
        for reader in readers:
            reader.start()
        deadline = time.monotonic() + timeout_seconds
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
        try:
            containment.close()
        finally:
            try:
                process.wait(timeout=5)
            finally:
                for reader in readers:
                    if reader.ident is not None:
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
        "job_memory_limit_bytes": admitted_bytes,
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
                output = run_bounded_command(
                    COMMANDS[name],
                    cancellation=self.cancellation,
                    timeout_seconds=COMMAND_TIMEOUT_SECONDS.get(name, CHILD_TIMEOUT_SECONDS),
                )
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
            try:
                self.worker = threading.Thread(target=self.execute, args=(message_id, name, modern))
                self.worker.start()
            except RuntimeError as exc:
                self.worker = None
                self.active_id = None
                raise ProtocolError(-32603, "could not start command worker") from exc

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
