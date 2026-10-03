# Targeted Verification

SuperZip verification is change-aware. Agents must select tests and checks from
the changed path set and its direct consumers. Batch related changes. Tooling
edits, path count and failed checks do not automatically justify unrelated
suites. Unknown paths must be classified before a plan can execute.

## Commands

Plan without running:

```powershell
tools\verification_plan.ps1 -IncludeUntracked
tools\verification_plan.ps1 -ChangedPath src\zip\zip_adapter.cpp -Json
tools\verification_plan.ps1 -IncludeUntracked -Checkpoint intermediate
```

Run the selected local commands:

```powershell
tools\verify_changes.ps1 -IncludeUntracked
```

Force the full local profile when a wider bug or regression is suspected:

```powershell
tools\verify_changes.ps1 -IncludeUntracked -Full
```

After pushing, wait only for relevant workflows for the pushed commit:

```powershell
tools\wait_relevant_workflows.ps1 -Commit <sha>
```

During a multi-commit implementation, use a non-blocking remote status sample
instead of idling on every intermediate commit:

```powershell
tools\wait_relevant_workflows.ps1 -Commit <sha> -Mode opportunistic
```

Choose `-Checkpoint intermediate` on the planner/verifier when another
development step remains, even for workflows, skills, MCP, or full escalation.
Local tests and final audit requirements are unchanged. `-Mode defer` is now a
compatibility alias for one opportunistic sample, not an unchecked skip.
Final review/handoff and release checkpoints require:

```powershell
tools\wait_relevant_workflows.ps1 -Commit <sha> -Mode final -FinalCommit
```

Use `-Full` on either script when the repo state is suspicious. The local runner
stops at the first failed command, preserving its original error. Diagnose
that mechanism before deciding whether wider coverage is justified.

## Classifier Contract

The classifier lives in `tools\superzip_verification.psm1`. It uses normalized
repository-relative paths and produces:

- `requiredLocalCommands`: commands the agent should run now.
- `manualLocalCommands`: expensive commands that are relevant before making
  performance claims or release decisions.
- `postPushWorkflows`: GitHub workflows the agent should wait for after push.
- `longRunningPostPushWorkflows`: slow workflows, currently fuzzing, that
  remain relevant but are checked opportunistically and waited only for final
  handoff or release.
- `postPushAuditRequired`: whether deployment/code-scanning audit is required.
- `workflowWaitPolicy`: explicit checkpoint intent, whether waiting can be
  deferred, recommended mode, and the unchanged final-acceptance requirement.
  `-Checkpoint final` is the safe default; agents choose `intermediate`
  autonomously when more development is planned.
- `fullEscalationRequired`: whether the change must use the broad local profile.

### Normal Targeting

| Change area | Required local verification | Relevant post-push workflows |
| --- | --- | --- |
| Docs only | changed-file hygiene, language lint | `lint` |
| C++ source, tests, CMake | hygiene, language lint, Release build, affected native mechanism or full native driver, changed-code refactor audit | `lint`, `windows-ci` |
| Archive parser, path safety, extraction publication | C++ checks plus external compatibility interop smoke, registry-wide format matrix smoke, security scan, and short fuzz smoke | `lint`, `windows-ci`, `security`; observe `fuzzing` |
| GUI or visual resources | C++ checks plus GUI smoke and screenshot inspection | `lint`, `windows-ci` |
| Workflows, security scanners, release actions | hygiene, language lint, security scan | `lint`, `security`, scanner-specific workflows, `scorecard` |
| Packaging or installer files | Release build, C++ tests, security scan, MSI identity smoke, package smoke | `lint`, `windows-ci`, `security` |
| MCP Python | Python syntax and bounded-child contracts, hygiene/lint/function contracts | relevant lint/security checks |
| Verifier routing | selector, runner, lint-routing and checkpoint contracts, hygiene/lint/function contracts | relevant lint/security checks |
| Performance-sensitive code | normal correctness checks; RAM-only benchmark is manual before claims | workflow set from touched source |

### Explicit Broad Coverage And Classification

The full profile is selected when the caller explicitly passes `-Full` (the
module's `-SuspectGlobalBug` equivalent). Use it when evidence establishes
cross-component impact; it is not a routine iteration default.

Tool contracts have explicit component/dependency patterns in the canonical
planner. Production build inputs require a build; receipt fixture edits select
receipt regressions without rebuilding unrelated codec code. Fuzz build and
production resource changes retain their actual bounded sanitizer smoke.
The full native driver already includes four toolchain helper contracts, so the
planner does not add duplicate standalone invocations of those same helpers.
The component driver does not execute them; separately changed helper contracts
remain selected.

### Native Component Selection

`tools/native_test_selection.ps1` owns the reviewed dependency mapping and reads
case names from the main test target's CMake source registration. The initial
production mapping covers static-prefix and adaptive/Huffman encoder files and
their required-HIP integration consumers: 17 unique cases. A batch changing
both files runs their union once. Registered test-file-only changes select all
recognized cases in those files; they do not trigger unrelated format-matrix,
fuzz, GUI or package work. Shared headers, other unmapped production mechanisms,
separate native test targets and explicit `-Full` keep the full native driver.
Add reviewed dependency mappings as further mechanisms are investigated.

The component driver validates the current successful native build receipt
before running. HIP-dependent cohorts require a compiled backend and an
available device before and after execution. It invokes exact names using the
native runner's `=case_name` filter, verifies one requested case per invocation
and stops at the first failure. Existing substring filters and the default
complete registry remain compatible. Duplicate names, unsupported registration
syntax, empty selections and invalid changed-path metadata fail explicitly.
The path decoder is checked under Windows PowerShell 5.1 and PowerShell 7.

Changes to the native test runner itself select eight controlled executable
contracts, compiling the actual runner source with three fixture registrations.
They cover default/substring/exact selection, failing cases, missing and empty
exact names, and extra arguments. They do not require rerunning every archive
fixture. The hosted component lane admits this runner source dependency; it
compiles the small contract fixture and keeps product builds in the native lane.

This is an incremental coverage map, not proof that all native source families
have fine-grained routing. A local required-HIP cohort must never be relabeled as
passed GPU qualification on a hosted runner without hardware.

### Hosted Native Work

`tools/ci_native_plan.ps1` projects build, native runtime, format-matrix and
repository-policy decisions from the same canonical planner. Pushes use the
complete event base; pull requests use their event base against the checked-out
merge result. Initial pushes classify tracked files, and manual dispatch remains
explicit broad CPU qualification. Missing or malformed bases fail rather than
silently using an arbitrary preceding commit. Checkout retains comparison history.

Windows CI executes only admitted steps. Registered test-only changes use their
component cohort; runner-only changes retain their compiled registry contracts
in the component lane. Format-matrix smoke runs when selected for a production
parser/routing mechanism or explicit broad qualification. Selected repository
policy checks remain on one Windows matrix leg, since their logic is independent
of compiler generation. Other lint, SAST, fuzz and release coverage remains in
its existing lanes and under its own admission rules.

HIP implementation files are excluded from the CPU-only build's event filter.
They instead trigger `rocm-qualification`, which provisions the complete pinned
SDK, compiles and links all release targets, and validates the current required-HIP
build receipt. GPU sources, shared GPU/core headers, vendored dependencies and
compiler/provisioning inputs admit that qualification; shared/CPU inputs still
admit Windows CI. The path exclusions follow
[GitHub's ordered path-filter rules](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#onpushpull_requestpull_request_targetpathspaths-ignore).
Planner/filter parity checks cover every current HIP translation unit declared
in CMake, mixed CPU/HIP batches and future nested paths.

Hosted component execution explicitly uses `-CpuOnlyValidation`. Both its
receipt and CLI must prove HIP is disabled; the mode rejects a HIP-enabled local
build. The runner records exact selected invocations and states that GPU
assertions are unqualified. It does not enable CPU fallback in a required-GPU
archive operation. Physical HIP correctness and RAM timing remain local/device
qualification; compiling all architectures does not prove execution on them.

Shared or unmapped source mechanisms still use the broad native driver. Compiler
or SDK changes with known wider behavior require explicit broad qualification;
metadata-only workflow/tool changes do not justify rebuilding the application.

Path count alone does not widen a documentation batch. An unknown path fails
planning with its name and a classification request, rather than guessing that
every product check is relevant. `-NoAutoEscalate` remains accepted for script
compatibility; stopping at a failure is now the default.

The `component-contracts` workflow uses this same planner over the actual
push/PR commit range and executes only offline tool contracts. Native Windows
CI is filtered to native build inputs, the native test driver and its own
workflow; every produced build still validates its actual invocation receipt.
Moved tool regressions are routed in the dedicated lane, not deleted.
GitHub supports [YAML anchors](https://docs.github.com/en/actions/reference/workflows-and-actions/reusing-workflow-configurations#yaml-anchors-and-aliases),
which keep push/PR input lists identical. Its
[path-filter limits](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#onpushpull_requestpull_request_targetpathspaths-ignore)
still apply: missing required exact-SHA runs must be investigated and dispatched
when authorized, never treated as passed. Future required-check configuration
must also account for workflows skipped by filters.

The full profile remains SSD-safe. It includes language lint, build, C++ tests,
external compatibility interop smoke, the registry-wide format matrix smoke,
security scan, changed-code refactor audit, selector self-tests, GUI smoke,
brand verification, short local fuzz smoke, and package smoke. RAM-only
benchmark sweeps stay manual unless a performance claim or tuning decision is
being made.

## Format Matrix Evidence

`tools/format_matrix_smoke.ps1` reads the built CLI registry and tests each
registered format's public contract. Create-capable formats roundtrip generated
files and directories; level-aware writers roundtrip every accepted level 1-9.
These are small correctness fixtures, not timing or compression-ratio benchmarks.
The SUZIP lane here is explicitly CPU-only; required-HIP proof remains separate.

For extract-only formats, the matrix runs an exact positive C++ fixture test
and requires one passing test plus its exported archive and verified output
tree. It then checks CLI identification, auto and explicit-format extraction,
overwrite refusal without output changes, explicit overwrite, and every
advertised extension alias. Each extraction must reproduce the expected path
tree and SHA-256 file hashes. A test source file's existence is not coverage.
Fixture export is opt-in for that child process through the test-only
`SUPERZIP_TEST_FIXTURE_EXPORT` variable; ordinary unit tests do not export files.

Both the CLI and native test executable must be rebuilt before this check.
Child commands have a two-minute deadline, captured diagnostics, and exact
Windows argument quoting. Running with a spaced `-WorkRoot` also exercises
paths that previously failed through the expected-error subprocess wrapper.
All fixture copies and CLI outputs stay inside the matrix-owned temporary
directory and are cleaned up on completion or failure.

## Workflow Wait Strategy

GitHub Actions waiting must be relevant and time-aware:

- `final`: blocking wait for every selected workflow on the pushed commit. Use
  this before final handoff, release publication, or any claim that remote CI is
  green.
- `opportunistic`: one remote status sample. It fails immediately on completed
  failures, returns without blocking when runs are missing or still active, and
  is suitable while development continues across multiple commits.
- `defer`: compatibility alias for `opportunistic`; it must not skip API checks
  or hide known failures. No critical override is needed for intermediate work.

Every successful API sample saves the commit, timestamp, selected runs, observed
state and pending/passed/failed audit state under `out/workflow-status/<sha>.json`.
Intermediate records are never final acceptance, even if the sampled runs are
green. API errors and invalid/wrong-SHA responses throw rather than replace
missing evidence with success. Final checks use increasing 30-120-second polling
intervals while state is unchanged and print only state transitions. Actual
failures still throw on the next sample. `-FinalCommit` rejects nonblocking modes,
omitted required workflows, and the audit-skip switch. All final-mode waits include
relevant long-running workflows unless an explicit narrower selection is supplied;
such a narrower selection is not final commit acceptance.

`tools\wait_relevant_workflows.ps1` performs a GitHub CLI preflight before it
polls, resolves local refs or short SHAs to a full commit SHA, and queries the
Actions REST API by `head_sha`. Do not replace that with `gh run list --commit`;
that filter has returned empty results for valid SuperZip commits. If `gh` is
not authenticated, lacks repository access, or the Actions API lookup fails, the
script must throw immediately. Agents must not keep polling a status line where
every selected workflow is reported as missing because that usually means the
wrong commit/ref or an authentication failure. If all selected workflows remain
missing past the script's missing-workflow grace window, fix the pushed commit
or workflow selection before retrying.

For iterative work, run selected local checks for each commit, sample after each
push, and continue independent development while runs are active. Recheck at a
useful decision boundary, not repeatedly with unchanged inputs. Investigate an
actual failure before relying on its output; unrelated work can continue.
Do not start a blocking waiter merely to narrate progress. Run final acceptance
once the accumulated change is ready for review, handoff or release. Supply
`-BaseRef <accepted-base>` for multi-commit coverage, or `-Full` when that scope is
uncertain; the latest commit's diff alone may omit earlier security/GUI changes.
Superseded intermediate commits need no separate wait. Do not call work complete
because an opportunistic command returned successfully.

The hosted fuzz workflow now runs on changes to its actual compiled source
families, vendored codec inputs, harnesses, container/build recipe or its own
workflow. The selector checks those triggers against the literal source inputs
declared by the production fuzz build. UI, GPU-only, documentation and unrelated
agent-tool edits do not start that Linux sanitizer build. Scheduled fuzzing and
manual dispatch remain available with both sanitizers and unchanged run budgets.
An archive adapter outside that build still requires its own relevant correctness
and interoperability checks; running unrelated fuzz targets does not qualify it.

The offline Greenbone audit runs on its configuration/scripts, pinned tool
requirements and the offline/live workflow contracts. Live scans retain their
separate schedule, broker authorization and stateful execution. Neither workflow
filter turns an unresolved finding into a pass. As with the native/component
filters above, review GitHub's path-filter limits and required-check settings.

Fuzzing is long-running by design. Do not wait for it during ordinary
development pushes. Use `-Mode opportunistic -IncludeLongRunning` occasionally
to check for completed fuzzing failures while continuing other work. Use
`-Mode final -FinalCommit` only when the current commit is the final handoff,
release, or other explicit completion point.

The lint lane is also change-aware. Push and pull-request runs lint the files
affected by the commit range and expand non-C++ languages to the whole relevant
language only when that language's lint configuration changed. C++ formatting is
enforced on changed C/C++ files only until a deliberate repo-wide formatting
migration is planned and tested. Manual `workflow_dispatch` lint runs recheck
the latest commit range.

Post-incident lessons live in `docs/engineering-learning-loop.md` and are
enforced through the hygiene/security scans when they describe a repeatable,
locally detectable failure class. Do not turn lessons into broad heavyweight
workflow waits; keep them as narrow invariants unless the verifier explicitly
escalates.

## Rules For Agents

1. Start with `tools\verification_plan.ps1 -IncludeUntracked`.
2. Run `tools\verify_changes.ps1 -IncludeUntracked` unless the plan needs a
   manually inspected GUI screenshot or a manual benchmark.
3. Do not run unrelated heavyweight gates merely because they exist.
4. Do not skip a gate that the plan marks required.
5. Diagnose failures and classify unknown paths. Widen coverage when evidence
   points outside the affected mechanism; select `-Full` explicitly when that
   wider impact requires the complete product profile.
6. Choose `-Checkpoint intermediate` when more development is planned and use
   one opportunistic post-push sample. Choose `final` for actual acceptance and
   use `-Mode final -FinalCommit` over the accumulated change range. Required
   audits remain pending during iteration and must pass at final acceptance.
7. If the classifier output looks wrong, fix the classifier and its tests before
   continuing feature work.

## Selector Self-Tests

`tools\test_verification_selector.ps1` verifies representative path classes:
docs-only, C++ source, archive parser, GUI, workflow, MCP, verifier, unknown
path, and forced-full mode. Verifier or MCP routing changes must run this test
directly, and the full profile runs it automatically.
