# SuperZip Agent Instructions

This is the repository entry point. Detailed rules remain normative in
[the operating guide](docs/agent-operating-guide.md); use the reading map below
to load only the relevant sections. Moving a rule into the guide does not make
it optional. Do not duplicate that guide into prompts, skills, or new policy
files. Re-read instructions when they change or when the task enters a new
domain, not after every unchanged checkpoint.

## Mission And Boundaries

SuperZip is a Windows-native C++20 archive application. AMD HIP is the only
approved GPU compute boundary. `.suzip` is its versioned native format;
compatibility formats use their documented in-process adapters. CPU fallback
must never masquerade as required-HIP compression. Preserve format semantics
and capability limits in `docs/archive-format-support.md` and
`docs/native-suzip-format.md`.

- Use Windows-native PowerShell, CMake, MSVC, and optional HIP. Do not use WSL
  unless the maintainer explicitly requests it.
- Preserve unrelated working-tree changes. Never use destructive Git commands
  without explicit authorization. Stage only intentional source/docs/config.
- Do not commit credentials, tokens, personal paths, host identities, build
  artifacts, crash dumps, or generated binaries. Pinned provenance archives
  under `third_party/upstream/**` are the only tracked archive exception.
- Keep tokens out of remotes and config. Use short-lived authentication only
  for a single push. Never expose credentials in output or documents.
- Do not stop, reconfigure, or interfere with unrelated host work. Defer only
  benchmark timing when substantial CPU/GPU/RAM/storage contention makes the
  measurement unreliable; ordinary correctness work need not wait for idle.
- Work as one agent. Do not spawn, fork, delegate, or fan out model workers
  unless the maintainer explicitly reverses that rule for a specific task.
  The sole standing exception is the Codex Security vulnerability scanner:
  invoke it on demand only, when explicitly requested by the maintainer. Use
  its serial or one-worker path when supported honestly. Spawn workers only
  when that requested scan genuinely requires delegation or the maintainer
  explicitly authorizes it, using the minimum worker count. Other skills'
  delegation recommendations must be executed serially here.
- Do not add Actions `environment:` blocks or `deployment:` keys. Workflows
  must not create deployment records. Private scanner configuration belongs
  behind the documented OIDC broker, never directly in workflow YAML.
- Use pinned, provenance-recorded dependencies. New runtime dependencies need
  maintainer approval and rationale. Do not edit upstream provenance archives.
- Preserve the canonical `superzip-logo-mark` artwork. Product text must use
  the existing native rendering system; read the GUI rules before UI changes.
- Do not launch the GUI for routine automated work. For requested UI work,
  announce the smoke test first and do not build/package while its GUI runs.
- Caveman and CocoIndex are mandatory: apply
  `.agents/skills/caveman/SKILL.md` to internal AI communication, and use
  `.agents/skills/cocoindex-code-search/SKILL.md` first for broad conceptual
  code navigation. Keep exact `rg` and direct source reads authoritative.
  Neither skill changes product runtime behavior. Use the current skill files,
  not an installed-tool assumption; [development context](docs/development-context.md)
  explains bounded startup, actual-use evidence and freshness checks.

## Required Reading

Read `README.md`, `IMPLEMENTATION_PLAN.md`, and the relevant code before editing.
Use headings to read the applicable sections of
`docs/agent-operating-guide.md`, not the whole file by habit. For broad work or
uncertain impact, read all applicable rows; never infer that a missing section
means its rules do not apply.

| Work | Required guide sections and domain documents |
| --- | --- |
| Any source change | Non-Negotiable Boundaries; Engineering Quality Baseline; Coding Standards; Required Function Documentation |
| Locating modules or dependencies | Project Map; Mission for exact adapter and vendored-version boundaries |
| GUI, state, input, drawing, settings | GUI Rules; `docs/debugging-strategy.md`; `docs/design.md` |
| Archive formats, parsing, create/extract, overwrite | Mission; Security Rules; `docs/archive-format-support.md`; `docs/native-suzip-format.md` when native format/detection changes |
| CPU/GPU performance, compression policy, benchmarks | Non-Negotiable Boundaries; Build And Test Commands; `docs/performance-block-size-validation.md`; `docs/compression-level-and-benchmark-suite.md`; `docs/gpu-accelerated-ui-and-codec-research.md` |
| Alternative GPU building blocks | Performance row plus `docs/compression-backend-evaluation.md` |
| Tools, workflows, MCP, skills, agent routing | Non-Negotiable Boundaries; Security Rules; Git Workflow; `docs/engineering-learning-loop.md` |
| Installer, packaging, release, versioning | Non-Negotiable Boundaries; Security Rules; Git Workflow; `docs/release.md` |
| Refactoring | `docs/refactoring-governance.md`; run `tools/refactor_audit.ps1` before broad cleanup |
| New archive-product behavior | `docs/product-behavior-audit.md` and the affected domain rows; do not copy reference code/UI |
| Documentation only | The domain rules for claims being changed; do not load unrelated source domains |
| Commit, push, final handoff | Git Workflow; Agent Workflow; `docs/targeted-verification.md` |

## Change And Verification Loop

Prevent recurrence using the current source and retained evidence. Never move
code, templates, tests or configuration into a passive-data folder to avoid
analysis, rename primitives to evade a rule, or inherit an old disposition after
its source/location changes. Reuse a completed check only after matching its
source, tool/configuration and relevant output identities; rerun the affected
consumer when they change. Check the planner's actual command roles before
execution: an offline tool contract must not silently rebuild native targets.
The operating guide and executable scope/admission contracts enforce the
specific boundaries; instructions alone are not proof of error prevention.

Inspect `git status --short`, then ask the verifier what the actual changed
paths require before selecting tests, smoke runs, benchmarks, or workflow waits:

```powershell
tools\verification_plan.ps1 -IncludeUntracked
tools\verify_changes.ps1 -IncludeUntracked
```

Batch related changes and run the affected component contracts and direct
consumers. A tooling/skill edit, path count, or failed focused check does not
justify unrelated product tests. Diagnose failures before widening coverage;
classify unknown paths before execution. Use `-Full` only for explicitly
selected broad coverage supported by cross-component impact. Do not replace
required tests with a narrower passing check. Explicit maintainer
deferrals must remain recorded as deferred, never passed; do not weaken the
verifier or remove scanners to hide them.

- Follow C++20/RAII, bounded resources, explicit ownership, and the existing
  module patterns. Prefer behavior-preserving, reviewable changes unless a
  behavior change was requested.
- Every new or changed function needs a concise Purpose/Inputs/Outputs
  contract, including locally edited vendor code. Run the changed-function
  size/complexity/documentation gate before publication:

  ```powershell
  tools\refactor_audit.ps1 -ChangedOnly -CheckContracts -MaxFunctionLines 120 -MaxComplexityMarkers 35 -FailOnFindings
  ```

- Security repairs and failures discovered during rewrites require the existing
  learning loop in `docs/engineering-learning-loop.md`: repair the production
  boundary, retain the reproducer, review sibling versions/callers, and add the
  narrowest executable recurrence guard. Read the guide's Engineering Quality
  Baseline and Coding Standards before memory/ownership rewrites. Do not close a
  finding through scanner suppression, primitive renaming, removed regression
  coverage, or a passing check against older source bytes.

- The ordinary local build is HIP-enabled:

  ```powershell
  tools\build.ps1 -Configuration Release
  tools\test.ps1 -Configuration Release
  tools\lint.ps1 -CppMode Changed -IncludeUntracked
  ```

  CPU-only validation is for hosted/static-analysis environments without HIP,
  not product releases. Missing linters should be bootstrapped through
  `tools\bootstrap_lint_env.ps1`, using the pinned repository toolchain.
- Changed standard-format writers require independent interoperability smoke;
  format routing or adapter contracts require the registry-driven format
  matrix. GUI changes require all-page smoke and screenshot review. The
  reading map and verifier define the exact commands.
- Development benchmarks are RAM-only: `memory_only=true`,
  `disk_write_bytes=0`. Filesystem smoke is capped at 64 MiB. Compare equal
  compression levels and report sizes, ratio, block size, CPU/GPU mode, and
  real HIP telemetry. Repeat small/noisy differences before drawing conclusions.
- Choose checkpoint intent autonomously: when further development is planned,
  pass `-Checkpoint intermediate` to the planner/verifier and sample the exact
  pushed SHA once with `tools\wait_relevant_workflows.ps1 -Commit <sha> -Mode
  opportunistic -IncludeLongRunning`. This also applies to workflow, verifier,
  MCP, skill, and full-escalation changes; it changes waiting, not test coverage.
  Record pending checks and continue independent work without repeated unchanged
  polling. Final review/handoff and release checkpoints use `-Checkpoint final`
  and `-Mode final -FinalCommit`, including required post-push audits. Never
  describe an intermediate snapshot as final acceptance.
- Use the guide's shared-host resource policy for all local work. Preserve RAM
  headroom, prefer below-normal priority for this task's heavy correctness or
  analysis processes, and use available CPUs without arbitrary fixed caps.
  Memory admission, bounded lifetime/output, and benchmark isolation still apply.
- Follow the guide's post-push audit requirement after remediations. Never
  treat a successful local test, push, PR closure, or unrelated green workflow
  as proof that all hosted checks passed.
- Releases remain x64, HIP-enabled, checksummed, and installer-smoke-tested.
  A new release uses a new version by default. Replacement needs explicit
  exact-version maintainer authorization and the workflow acknowledgement;
  never delete an old release before its replacement has built and validated.

## Communication

Report blocking tool/service errors immediately with the exact returned cause,
affected work, and next actionable step. Distinguish access-check failures from
eligibility denials. Keep unrelated work moving; do not retry unchanged failures
without new evidence. Report verified outcomes, remaining work, deferred gates,
and measurement limits without promising universal compatibility, zero bugs,
or unmeasured speedups.
