---
name: superzip-security
description: Review and repair SuperZip parser, filesystem, runtime, GPU lifetime and supply-chain boundaries with source-bound evidence and recurrence tests.
---

# SuperZip Security

Follow `AGENTS.md`, the operating guide's
[Security Rules](../../../docs/agent-operating-guide.md#security-rules) and
[Engineering Quality Baseline](../../../docs/agent-operating-guide.md#engineering-quality-baseline).
Those documents own single-agent operation, authorization boundaries, memory
contracts, source admission and release gates.

## Investigation

1. Reuse a complete finding inventory only after matching its source, scanner
   and configuration identities. Read new or changed reports in full. Distinguish
   a diagnostic from a demonstrated defect and keep individual finding evidence.
2. Trace the trust boundary, ownership, initialized extents, resource consumption
   and failure propagation through direct callers and sibling implementations.
3. Preserve the original failure and an independent oracle where practical.
   Repair the production mechanism and apply the
   [engineering learning loop](../../../docs/engineering-learning-loop.md).
4. Run the actual pinned detectors selected by
   [targeted verification](../../../docs/targeted-verification.md), including
   changed-input preflight before expensive qualification. Inspect raw output
   and affected native or tool consumers when a gate fails.
5. Follow the [source-remediation requirements](../../../docs/security-source-remediation-redesign.md)
   for closure. Historical approvals, passing policy tests and GitHub dismissal
   do not establish a source repair.

## Boundary reading map

| Boundary | Reference and direct evidence |
| --- | --- |
| Archive metadata and routing | [Format support](../../../docs/archive-format-support.md); malformed input, unsupported methods and independent compatibility consumers |
| Native blocks and GPU payloads | [SUZIP format](../../../docs/native-suzip-format.md); versions, bounded layouts, actual required-HIP execution and independent readback |
| Extraction and publication | Security Rules; pinned identities, reparse parents, overwrite, corrupt trailers and aggregate bounds |
| GPU asynchronous ownership | Engineering Quality Baseline; lifetime through actual completion, cancellation, cleanup and bounded kernel progress |
| Runtime DLLs, processes and installers | Security Rules, [MCP contracts](../../../mcp/README.md) and [release qualification](../../../docs/release.md) |
| Scanners and dependencies | [Scanner coverage and operation](../../../docs/security-code-scanning.md), provenance and installed-consumer checks |

## Hosted verification

Read canonical scanner configuration before changing analysis coverage or build
commands. Review both complete CPU and HIP-host CodeQL analyses and the same-run
Semgrep coverage diagnostics. HIP-host extraction is not device-kernel analysis.
Preserve complete reports, stable categories and required source consumers.

The operating guide's Git Workflow defines intermediate sampling and final
audits. Current reproduced dismissed reports remain part of the final inventory.
Keep findings and unresolved evidence visible; do not duplicate workflow recipes
or turn this skill into a count-based approval ledger.
