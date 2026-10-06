# SuperZip Local MCP

Run `py -3 mcp/superzip_mcp.py` from a stdio-capable MCP client. Python 3.12+
is sufficient; the server has no third-party Python runtime dependencies.
Commands resolve the repository from the script location, not the client CWD.
The `mcp` directory contains standalone integration scripts, not a Python package
named `mcp`. This preserves the installed MCP SDK's import namespace when other
repository tools use it. The existing client launch path is unchanged.

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

The ordinary command timeout is 900 seconds. Full local verification has a
3600-second deadline; the explicit final workflow waiter has 4500 seconds,
covering its 60-minute remote deadline plus the audit rather than killing it
prematurely after 15 minutes. Cancellation and output limits still apply.
Aggregate child output is limited to 16 MiB;
only bounded stdout/stderr tails are returned with explicit truncation flags.
These are development commands, not a remote archive-processing API. A client
must obtain user approval appropriate to the selected command. `security_scan`
is the repository's local policy script, not the Codex Security plugin scanner.
No model workers or automatic scans are launched by connection or discovery.
Windows children use below-normal priority without CPU affinity/rate limits.
Indirect Windows PowerShell launches use a child-only compatible module path,
not the inherited PowerShell 7 paths; neither setting changes the caller or
unrelated host tasks.

## Memory Containment

Every command samples current available physical RAM, keeps at least 2 GiB and
half the available RAM for host headroom, and requires a 2 GiB admitted budget.
The same admission arithmetic is regression-tested against
`tools/local_resources.ps1`; unknown counters or insufficient capacity refuse
execution. Windows then enforces the admitted byte ceiling across the complete
command tree through `JOB_OBJECT_LIMIT_JOB_MEMORY`, not a per-child allowance.
This limits aggregate committed memory, not total host working-set usage or
GPU VRAM. The server's small bounded transport buffers are outside that job.

The child starts suspended. Its kill-on-close and memory limits are queried
back, assignment must succeed, and only its verified primary thread is resumed.
Launch failure reaps the suspended child and closes owned handles/pipes; a
descendant cannot launch ahead of containment. Only our child is affected.
No CPU quota, affinity, breakaway permission or working-set throttling is set.
Normal exit, cancellation, timeout and output-limit handling retain the existing
tree cleanup. Failure to start either output-reader thread also terminates and
reaps the owned child and closes its pipes before returning the error.
Results include `job_memory_limit_bytes`; allocation denial remains
a command failure, never a passed or deferred check.

Command execution requires Windows; discovery, framing and pure protocol tests
do not. Other platforms fail before spawning instead of advertising an
unenforced aggregate cap. Tool callers cannot supply a larger budget or change
the command allowlist. Build and analysis tools still need their own
RAM-admitted scheduling/native limits; a job ceiling is containment, not proof
that their estimates fit every invocation. See Microsoft's
[job memory contract](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information)
and [suspended-process flag](https://learn.microsoft.com/en-us/windows/win32/procthread/process-creation-flags).

Verification tools plan intermediate development checkpoints without reducing
local test coverage. `wait_relevant_workflows`, its opportunistic variant, and
the legacy defer tool now take one exact-SHA status sample; they do not block
ongoing development or label pending checks as accepted. Use
`wait_final_commit_workflows` at a real final checkpoint. Final acceptance must
cover accumulated changes; the final MCP tool selects `-Full` conservatively.
Use the PowerShell waiter with an explicit `-BaseRef` for narrower evidence-backed
accumulated scope. Required
audits cannot be skipped. See [targeted verification](../docs/targeted-verification.md).

## Verification

```powershell
py -3 -m unittest discover -s mcp -p test_superzip_mcp.py -v
```

Tests exercise current discovery, legacy initialization, schemas, result/error
shapes, invalid requests, cancellation, real stdio framing, output limits,
timeouts, shared RAM-policy parity, actual aggregate parent/descendant allocation
denial, fail-closed launch, and Windows descendant containment. Full verification selects this
suite automatically. No network transport, resources, prompts, sampling,
elicitation, subscriptions, or optional MCP extensions are advertised.

The lint tool includes untracked files, matching repository development policy;
new source must be checked before its first commit.

Protocol references checked on 2026-09-05:

- [Versioning](https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning)
- [Discovery](https://modelcontextprotocol.io/specification/2026-07-28/server/discover)
- [Tools](https://modelcontextprotocol.io/specification/2026-07-28/server/tools)
- [Stdio](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/stdio)
