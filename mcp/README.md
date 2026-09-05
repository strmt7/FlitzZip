# SuperZip Local MCP

Run `py -3 mcp/superzip_mcp.py` from a stdio-capable MCP client. Python 3.12+
is sufficient; the server has no third-party Python runtime dependencies.
Commands resolve the repository from the script location, not the client CWD.

## Protocol

- MCP `2026-07-28`: per-request version/capability metadata, `server/discover`,
  standard tool definitions, complete results, and structured command output.
- Legacy clients: `initialize` and `notifications/initialized` for
  `2025-11-25`, `2025-06-18`, `2025-03-26`, and `2024-11-05`.
- Tool arguments are always an empty object. The published schema rejects
  extra properties; callers cannot append shell arguments.
- Exactly one command runs at a time. A second command receives a busy error;
  discovery and ping remain responsive. There is no pending command queue.
- `notifications/cancelled` stops only the matching owned command tree and
  suppresses its response. Closing stdin also cancels owned work.
- Command failures are MCP tool results with `isError: true`; malformed
  requests and unknown tools use JSON-RPC errors. Notifications receive no reply.

The command timeout is 900 seconds. Aggregate child output is limited to 16 MiB;
only bounded stdout/stderr tails are returned with explicit truncation flags.
These are development commands, not a remote archive-processing API. A client
must obtain user approval appropriate to the selected command. `security_scan`
is the repository's local policy script, not the Codex Security plugin scanner.
No model workers or automatic scans are launched by connection or discovery.

## Verification

```powershell
py -3 -m unittest mcp.test_superzip_mcp -v
```

Tests exercise current discovery, legacy initialization, schemas, result/error
shapes, invalid requests, cancellation, real stdio framing, output limits,
timeouts, and Windows descendant containment. Full verification selects this
suite automatically. No network transport, resources, prompts, sampling,
elicitation, subscriptions, or optional MCP extensions are advertised.

Protocol references checked on 2026-09-05:

- [Versioning](https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning)
- [Discovery](https://modelcontextprotocol.io/specification/2026-07-28/server/discover)
- [Tools](https://modelcontextprotocol.io/specification/2026-07-28/server/tools)
- [Stdio](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/stdio)
