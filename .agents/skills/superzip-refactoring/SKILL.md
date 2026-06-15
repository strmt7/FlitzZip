---
name: superzip-refactoring
description: Plan, audit, and execute behavior-preserving SuperZip refactors with tests, benchmark gates, and security boundaries. Use when the user asks for refactoring, cleanup, architecture simplification, automatic code-quality remediation, or large-file/function reduction.
---

# SuperZip Refactoring Skill

Read `docs/refactoring-governance.md` before editing code.

Required sequence:

1. Run the read-only audit:
   ```powershell
   tools/refactor_audit.ps1
   ```
   Add `-CheckContracts` only when explicitly auditing function comments; it is
   a heuristic and can flag lambdas or test macros.
2. Identify the behavior that must remain unchanged.
3. Make one focused structural change at a time.
4. Run the narrowest affected tests first.
5. Run the full relevant gate before pushing.

Rules:

- Refactoring must preserve behavior unless a maintainer explicitly requests a
  functional change in the same task.
- Do not refactor vendored upstream code under `third_party/upstream`.
- Do not combine broad cleanup with benchmark claims unless the benchmark is
  rerun and recorded.
- For GUI refactors, run `tools/gui_smoke.ps1 -Configuration Release`.
- For codec or performance refactors, run RAM-only CPU/GPU benchmarking at
  compression level 5 and record compression ratio:
  ```powershell
  tools/bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1 -BlockSizeKiB 256,1024,4096,16384
  ```
- Never use `tools/refactor_audit.ps1 -FailOnFindings` as a new required gate
  until existing findings have been triaged.
