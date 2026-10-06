---
name: superzip-refactoring
description: Review and refactor SuperZip architecture, ownership and large functions while preserving observable behavior and validating affected consumers.
---

# SuperZip Refactoring

Use the repository reading map in `AGENTS.md`. The
[refactoring guide](../../../docs/refactoring-governance.md) defines review and
acceptance; the [operating guide](../../../docs/agent-operating-guide.md) owns
coding, resource and security rules. Read
[debugging strategy](../../../docs/debugging-strategy.md) for defect investigation.

## Procedure

1. Inspect the working tree and run the read-only `tools/refactor_audit.ps1`.
   Treat size and complexity findings as review leads. An inventory or heuristic
   result does not establish that a file's logic has been reviewed.
2. Name the invariant and direct consumers before editing: archive bytes,
   compatibility, ownership, resource bounds, error propagation, cancellation,
   CLI output or GUI behavior. Trace a defect through sibling callers.
3. Make a coherent change that removes a demonstrated problem. Keep provenance
   archives immutable; production dependency changes use their documented
   patch and regeneration mechanisms.
4. Retain an independent regression for a repaired defect and, where practical,
   demonstrate the original failure. Apply the
   [learning loop](../../../docs/engineering-learning-loop.md).
5. Run [change-aware verification](../../../docs/targeted-verification.md)
   and its selected consumers. Use the changed-function gate in `AGENTS.md`;
   adding comments alone does not validate behavior.
6. Review the final diff and match reused evidence to unchanged inputs. Record
   exact source, verification scope, remaining findings and measurement limits.

## Domain routing

| Change | Additional procedure |
| --- | --- |
| Codec, scheduling, memory or HIP hot path | `superzip-performance`; compare the affected mode using existing benchmark controllers |
| Parser, publication, runtime loading or trust boundary | `superzip-security`; preserve failure-path and malformed-input controls |
| Native GUI state or rendering | Operating guide GUI Rules and the selected all-page smoke with screenshot inspection |
| Build, packaging or verification orchestration | `superzip-build-test`; inspect actual command roles before execution |

Use intermediate checkpoints during development and the operating guide's final
workflow/audit procedure at handoff over the accumulated change range. Keep
review progress and per-scan results in evidence records, not this skill.
