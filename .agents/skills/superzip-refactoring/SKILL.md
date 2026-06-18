---
name: superzip-refactoring
description: Plan, audit, and execute behavior-preserving SuperZip refactors with tests, benchmark gates, and security boundaries. Use when the user asks for refactoring, cleanup, architecture simplification, automatic code-quality remediation, or large-file/function reduction.
---

# SuperZip Refactoring Skill

Read `docs/refactoring-governance.md` before editing code.
Read `docs/targeted-verification.md` before choosing checks.

Required sequence:

1. Run the read-only audit:
   ```powershell
   tools/refactor_audit.ps1
   ```
   Add `-CheckContracts` only when explicitly auditing function comments; it is
   a heuristic and can flag lambdas or test macros.
   Before committing, run the changed-code gate:
   ```powershell
   tools/refactor_audit.ps1 -ChangedOnly -CheckContracts -MaxFunctionLines 120 -MaxComplexityMarkers 35 -FailOnFindings
   ```
2. Identify the behavior that must remain unchanged.
3. Make one focused structural change at a time.
4. Run `tools/verification_plan.ps1 -IncludeUntracked`.
5. Run the selected checks with `tools/verify_changes.ps1 -IncludeUntracked`.
6. Use `tools/verify_changes.ps1 -IncludeUntracked -Full` when the plan
   escalates, a targeted check fails, or the refactor points to a wider bug.
7. For pushed refactor commits, use opportunistic workflow checks only while
   work is still in progress and `workflowWaitPolicy.deferAllowed=true`; run
   final relevant workflow waiting before handoff.

Rules:

- Refactoring must preserve behavior unless a maintainer explicitly requests a
  functional change in the same task.
- Keep refactors aligned with secure-by-design defaults: fail closed, preserve
  explicit opt-ins, and keep trust boundaries documented.
- Do not refactor vendored upstream code under `third_party/upstream`.
- Do not combine broad cleanup with benchmark claims unless the benchmark is
  rerun and recorded.
- For GUI refactors, run the plan-selected GUI smoke. The classifier selects
  `tools/gui_smoke.ps1 -Configuration Release` for GUI changes and full
  escalation.
- New or changed functions must stay small enough to avoid CodeQL
  poorly-documented/large-function findings. If a changed function approaches
  120 lines or mixes multiple responsibilities, split it before pushing.
- For codec or performance refactors, run RAM-only CPU/GPU benchmarking at
  compression level 5 and record compression ratio:
  ```powershell
  tools/bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1 -BlockSizeKiB 256,512,1024,2048,4096,8192,16384
  ```
- Never use `tools/refactor_audit.ps1 -FailOnFindings` as a new required gate
  until existing findings have been triaged.
