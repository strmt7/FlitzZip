# FlitzZip Agent Instructions

This is the repository entry point. Detailed rules remain normative in
[the operating guide](docs/agent-operating-guide.md); use the reading map below
to load only the relevant sections. Moving a rule into the guide does not make
it optional. Do not duplicate that guide into prompts, skills, or new policy
files. Re-read instructions when they change or when the task enters a new
domain, not after every unchanged checkpoint.

## Mission And Boundaries

FlitzZip is a Windows-native C++20 archive application. AMD HIP is the only
approved GPU compute boundary. `.flzip` is its versioned native format;
compatibility formats use their documented in-process adapters. CPU fallback
must never masquerade as required-HIP compression. Preserve format semantics
and capability limits in `docs/archive-format-support.md` and
`docs/native-flzip-format.md`.

Use FlitzZip and `.flzip` consistently in active interfaces, artifacts and
documentation. Predecessor names belong only to explicitly tested migration
inputs or immutable evidence; do not reintroduce old command aliases. Preserve
wire signatures and upgrade identities when a naming change does not change
the underlying format or product lineage.

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
- Preserve the canonical `flitzzip-logo-mark` artwork. Product text must use
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
- Use the self-hosted [Crawl4AI research tool](docs/crawl4ai.md) for web-page
  reading and crawling first. Its portable launcher installs automatically when absent;
  use documented upstream APIs and configuration, and retain access failures.
  For a confirmed site restriction or unsupported content, use a permitted
  alternative under the operating guide's research rules; mandatory use does
  not require endless retries or conceal tool defects. Search remains available
  for discovery. The guide defines evidence, diagnosis and access boundaries.

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
| Archive formats, parsing, create/extract, overwrite | Mission; Security Rules; `docs/archive-format-support.md`; `docs/native-flzip-format.md` when native format/detection changes |
| CPU/GPU performance, compression policy, benchmarks | Non-Negotiable Boundaries; Build And Test Commands; `docs/performance-block-size-validation.md`; `docs/compression-level-and-benchmark-suite.md`; `docs/gpu-accelerated-ui-and-codec-research.md` |
| Alternative GPU building blocks | Performance row plus `docs/compression-backend-evaluation.md` |
| Tools, workflows, MCP, skills, agent routing | Non-Negotiable Boundaries; Security Rules; Git Workflow; `docs/engineering-learning-loop.md` |
| Installer, packaging, release, versioning | Non-Negotiable Boundaries; Security Rules; Git Workflow; `docs/release.md` |
| Refactoring | `docs/refactoring-governance.md`; run `tools/refactor_audit.ps1` before broad cleanup |
| New archive-product behavior | `docs/product-behavior-audit.md` and the affected domain rows; do not copy reference code/UI |
| Documentation only | The domain rules for claims being changed; do not load unrelated source domains |
| Commit, push, final handoff | Git Workflow; Agent Workflow; `docs/targeted-verification.md` |

## Change And Verification Loop

Inspect `git status --short`, then select and execute the actual affected
contracts with `tools\verification_plan.ps1 -IncludeUntracked` and
`tools\verify_changes.ps1 -IncludeUntracked`. Inspect command roles before
execution; an offline contract must not silently rebuild native targets.
[Targeted verification](docs/targeted-verification.md) defines scope, commands,
source/tool/output identity checks and intermediate/final acceptance.

- Batch coherent changes and test their direct consumers. Unknown paths must
  be classified first. A tooling edit, path count or failed focused test does
  not justify unrelated native work. Use `-Full` only for established broad
  impact; never replace required tests with narrower passing checks. Deferrals
  remain deferred, never passed.
- Follow the guide's Engineering Quality Baseline, Coding Standards and Required
  Function Documentation, including C++20/RAII, bounded ownership and concise
  Purpose/Inputs/Outputs contracts for every changed function, including vendor
  edits. The changed-function size/complexity/documentation gate is mandatory.
- Use the [learning loop](docs/engineering-learning-loop.md): repair the production
  boundary, retain a reproducer, review sibling callers and add a recurrence guard.
  Never evade analysis by relocating code, renaming primitives, suppressing rules,
  removing regression coverage or reusing approval after source/location changes.
  Historical ledgers are evidence only; individually proven false positives follow
  the authorized [exact-source review](docs/security-source-remediation-redesign.md).
- Local product builds and releases require HIP. Read Build And Test Commands
  for the standard commands and pinned linter bootstrap. Changed format writers
  require independent interoperability; routing/adapters require the format matrix;
  GUI changes require all-page smoke and screenshot review.
- Apply the guide's shared-host resource policy to all work. Development benchmarks
  remain RAM-only (`memory_only=true`, `disk_write_bytes=0`); filesystem smoke is
  capped at 64 MiB. Compare equal efforts, complete sizes and real HIP telemetry;
  repeat noisy differences before making performance claims.
- Use `-Checkpoint intermediate` while development continues. Sample the exact
  pushed SHA once with `tools\wait_relevant_workflows.ps1 -Commit <sha> -Mode
  opportunistic -IncludeLongRunning`, retain pending results and continue independent
  work. This applies to workflow, verifier, MCP, skill and full-escalation changes;
  it changes waiting, not coverage. Final review/release uses `-Checkpoint final`
  and `-Mode final -FinalCommit`, including required post-push audits. A local pass,
  push or PR closure cannot establish hosted acceptance.
- Follow [release qualification](docs/release.md): x64, HIP, checksums, exact-package
  installer smoke and source obligations. New releases use new versions by default;
  replacement requires exact-version authorization and workflow acknowledgement.
  Never delete the previous release before its replacement validates.

## Context And Token Efficiency

Use [development context](docs/development-context.md) across source, documents,
MCP, research and verification. Inspect outlines and selected report fields first;
read the exact required source next. Retain complete logs locally and return bounded
results with explicit omissions and continuations. Reuse only evidence whose
relevant identities still match. Keep one current local continuation note; durable
rules belong in their canonical guide and executable contracts. Never reduce code
quality, required coverage, security checks or evidence to save tokens.

## Communication

Report blocking tool/service errors immediately with the exact returned cause,
affected work, and next actionable step. Distinguish access-check failures from
eligibility denials. Keep unrelated work moving; do not retry unchanged failures
without new evidence. Report verified outcomes, remaining work, deferred gates,
and measurement limits without promising universal compatibility, zero bugs,
or unmeasured speedups.
