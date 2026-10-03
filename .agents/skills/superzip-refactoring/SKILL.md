---
name: superzip-refactoring
description: Plan, audit, and execute behavior-preserving SuperZip refactors with tests, benchmark gates, and security boundaries. Use when the user asks for refactoring, cleanup, architecture simplification, automatic code-quality remediation, or large-file/function reduction.
---

# SuperZip Refactoring Skill

Read `docs/refactoring-governance.md` before editing code.
Read `docs/debugging-strategy.md` before broad bug hunting or GUI/product
defect work.
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
6. Diagnose failed focused checks before widening coverage. Batch related
   changes; test affected mechanisms and direct consumers. Use `-Full` only
   for explicitly selected broad coverage supported by cross-component impact.
7. Choose planner/verifier `-Checkpoint intermediate` when further work remains,
   including full local escalation. Sample the pushed SHA once opportunistically
   and continue independent work with pending gates recorded. Final review/handoff
   uses `-Checkpoint final` and `-Mode final -FinalCommit` over the accumulated
   change range. Follow the guide's shared-host resource policy.

Rules:

- Refactoring must preserve behavior unless a maintainer explicitly requests a
  functional change in the same task.
- Broad "modernize" or "enterprise quality" requests must be converted into
  verifiable invariants: smaller functions, clearer ownership, bounded
  CPU/RAM/VRAM/disk/handle use, explicit contracts, dependency simplification,
  stronger tests, or measured performance evidence. Do not perform vague
  formatting churn or novelty-driven rewrites.
- Keep refactors aligned with secure-by-design defaults: fail closed, preserve
  explicit opt-ins, and keep trust boundaries documented.
- Do not refactor vendored upstream code under `third_party/upstream`.
- Do not combine broad cleanup with benchmark claims unless the benchmark is
  rerun and recorded.
- For GUI refactors, run the plan-selected GUI smoke. The classifier selects
  `tools/gui_smoke.ps1 -Configuration Release` for GUI changes and full
  escalation.
- GUI refactors must preserve existing behavior unless the maintainer asks for
  the behavior change: Downloads defaults, Queue fixed-header scrolling,
  table-only drag/drop, total-GPU graph semantics, and System graph cadence are
  explicit regression boundaries.
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
