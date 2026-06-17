#!/usr/bin/env python3
"""Minimal JSON-RPC helper for safe SuperZip local operations.

This is intentionally small and local-only. It exposes a narrow command set so
future agents can run approved build/test/security tasks without inventing shell
commands that leak secrets or launch the GUI.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
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
    print(json.dumps(payload), flush=True)


def handle(request: dict) -> None:
    """Purpose: Dispatch a restricted SuperZip MCP-style JSON-RPC request.
    Inputs: request is a parsed JSON object from stdin.
    Outputs: Runs only allowlisted commands and returns captured stdout, stderr, and exit code.
    """
    message_id = request.get("id")
    method = request.get("method")
    if method == "initialize":
        respond(message_id, {"name": "superzip-mcp", "version": "0.1.0"})
        return
    if method == "tools/list":
        respond(message_id, {"tools": [{"name": name} for name in COMMANDS]})
        return
    if method == "tools/call":
        params = request.get("params") or {}
        name = params.get("name")
        if name not in COMMANDS:
            respond(message_id, error={"code": -32602, "message": "unknown tool"})
            return
        completed = subprocess.run(
            COMMANDS[name],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=900,
        )
        respond(
            message_id,
            {
                "exit_code": completed.returncode,
                "stdout": completed.stdout[-12000:],
                "stderr": completed.stderr[-12000:],
            },
        )
        return
    respond(message_id, error={"code": -32601, "message": "method not found"})


def main() -> int:
    """Purpose: Process newline-delimited JSON-RPC requests until stdin closes.
    Inputs: stdin supplies one JSON object per line.
    Outputs: Returns a process exit code after all requests are handled.
    """
    for line in sys.stdin:
        if not line.strip():
            continue
        try:
            handle(json.loads(line))
        except Exception as exc:  # noqa: BLE001 - JSON-RPC boundary
            respond(None, error={"code": -32603, "message": str(exc)})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
