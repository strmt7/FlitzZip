# Neutron Corpus Transport Review, 5 October 2026

The pending controller originally used a local HTTP server solely to exchange
resident corpus bytes and observations between Hyperfine's serial workers.
Its changed-file preflight reported three `DS162092` locations. The replacement
uses the standard library's Windows shared-memory API and a Windows mutex.
No TCP endpoint, HTTP handler, source disposition or scanner exclusion is used.
The corpus acquisition path still uses authenticated, bounded HTTPS downloads.

## Ownership And Admission

The controller verifies the native build receipt and acquires its measurement
lease before downloading a corpus. It verifies Hyperfine's reviewed version and
holds the executable against modification, recording its measured SHA-256.
Original corpus bytes move once into a shared mapping; the original per-member
buffers are released. Admission includes publication overlap, fixed observation
capacity and child input copies. Workers validate the complete manifest,
contiguous extents and every source digest before invoking the native consumer.

Source views are read-only Python buffer views. Named objects retain Windows'
default access policy; they do not provide isolation against another process
already authorized by that account. Workers never deserialize pickle objects.
One mutex owns a complete repetition. Concurrent, abandoned, partial and excess
runs fail admission. Results occupy bounded slots and are emitted incrementally
after workers exit, avoiding a second collection of all decoded protocols.
Completed partial observations remain visible with `study_qualified=false`.

Subprocesses start suspended and enter the existing memory-admitted Windows job
before executing, at below-normal priority. The canonical job owner now requests
termination and queries accounting until zero active processes, with a finite
five-second deadline. It closes its handle even when accounting or termination
fails. Direct consumers still reap children and close their streams on that
failure path. Closing a root process alone is not accepted as descendant cleanup.

Hyperfine 1.20.0's [direct command parser](https://github.com/sharkdp/hyperfine/blob/v1.20.0/src/command.rs)
uses `shell_words::split`, including on Windows. Windows `list2cmdline` quoting
was therefore incorrect for that input and the real smoke returned
`program not found`. The controller now serializes that parser's arguments with
`shlex.join` while retaining `--shell none`. No intermediate shell is introduced.
The pinned executable then completed two serial fixture runs and four ordered
IPC observations. Those protocols were explicitly mocked for correctness tests;
no GPU ran and no compression or timing result is claimed.

## Evidence And Remaining Work

Offline contracts cover binary preservation across real processes, mapping
cleanup, memory admission before allocation, altered metadata and bytes,
result bounds/order, concurrent and abandoned workers, partial-report retention,
stale native receipts before acquisition, argument boundaries, and descendant
termination. The process owner additionally has deterministic accounting-failure,
timeout and idempotent-close controls. Its MCP and crawler consumers are included
in the change-aware verifier, alongside the corpus controller. These checks do
not rebuild the application.

The full changed-tree preflight retained its raw receipt and SARIF under ignored
`out/scanner-preflight/`; its checked inputs produced zero Gitleaks and DevSkim
findings, with no reviewed source findings admitted. This is changed-source
evidence, not a claim that every historical raw warning disappeared. Hosted
analysis and the exact pushed-commit audit remain separate gates.

The native input-limit, binary-stdin, display-name and HIP event-lifecycle changes
are outside this tooling checkpoint. They still require their selected native
qualification. The reported system freeze remains unexplained; no benchmark,
new GPU stability result, compression improvement or release is certified here.
