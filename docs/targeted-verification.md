# Targeted Verification

SuperZip verification is change-aware. Agents must select tests and checks from
the changed path set, then escalate automatically when the change is broad,
unclassified, verifier-related, or when a targeted command fails.

## Commands

Plan without running:

```powershell
tools\verification_plan.ps1 -IncludeUntracked
tools\verification_plan.ps1 -ChangedPath src\zip\zip_adapter.cpp -Json
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

Use `-Mode defer` only when the plan allows deferral. Final handoff, release
work, workflow changes, verifier changes, and full-escalation changes must run
the final wait before they are reported complete:

```powershell
tools\wait_relevant_workflows.ps1 -Commit <sha> -Mode final
```

Use `-Full` on either script when the repo state is suspicious. The local runner
also escalates automatically if any targeted command fails.

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
- `workflowWaitPolicy`: whether workflow waiting can be deferred while work is
  still in progress and the recommended wait mode for the current change.
- `fullEscalationRequired`: whether the change must use the broad local profile.

### Normal Targeting

| Change area | Required local verification | Relevant post-push workflows |
| --- | --- | --- |
| Docs only | changed-file hygiene, language lint | `lint` |
| C++ source, tests, CMake | hygiene, language lint, Release build, C++ tests, changed-code refactor audit | `lint`, `windows-ci` |
| Archive parser, path safety, extraction publication | C++ checks plus security scan and short fuzz smoke | `lint`, `windows-ci`, `security`; observe `fuzzing` |
| GUI or visual resources | C++ checks plus GUI smoke and screenshot inspection | `lint`, `windows-ci` |
| Workflows, security scanners, release actions | hygiene, language lint, security scan | `lint`, `security`, scanner-specific workflows, `scorecard` |
| Packaging or installer files | Release build, C++ tests, security scan, package smoke | `lint`, `windows-ci`, `security` |
| MCP Python or verifier routing | full profile plus Python bytecode compile and selector self-test | full-profile workflow set |
| Performance-sensitive code | normal correctness checks; RAM-only benchmark is manual before claims | workflow set from touched source |

### Automatic Full Escalation

The full profile is selected automatically when:

- the caller passes `-Full`;
- at least 25 paths changed;
- any changed path is unknown to the classifier;
- verification tooling, MCP command routing, or SuperZip agent skills changed;
- a targeted local command fails under `tools\verify_changes.ps1`.

The full profile remains SSD-safe. It includes language lint, build, C++ tests,
security scan, changed-code refactor audit, selector self-tests, GUI smoke,
brand verification, short local fuzz smoke, and package smoke. RAM-only
benchmark sweeps stay manual unless a performance claim or tuning decision is
being made.

## Workflow Wait Strategy

GitHub Actions waiting must be relevant and time-aware:

- `final`: blocking wait for every selected workflow on the pushed commit. Use
  this before final handoff, release publication, or any claim that remote CI is
  green.
- `opportunistic`: one remote status sample. It fails immediately on completed
  failures, returns without blocking when runs are missing or still active, and
  is suitable while development continues across multiple commits.
- `defer`: records that waiting is intentionally postponed. It is allowed only
  when `workflowWaitPolicy.deferAllowed=true`; it is rejected for workflow,
  verifier, MCP, skill, or full-escalation changes unless a maintainer makes an
  explicit critical override.

`tools\wait_relevant_workflows.ps1` performs a GitHub CLI preflight before it
polls and resolves local refs or short SHAs to a full commit SHA. If `gh` is not
authenticated, lacks repository access, or `gh run list` fails, the script must
throw immediately. Agents must not keep polling a status line where every
selected workflow is reported as missing because that usually means the wrong
commit/ref or an authentication failure. If all selected workflows remain
missing past the script's missing-workflow grace window, fix the pushed commit
or workflow selection before retrying.

For iterative feature work, run targeted local checks for each commit, use
`-Mode opportunistic` after pushes when useful, continue development while runs
are active, and run `-Mode final` once the feature is ready for handoff. Do not
call work complete because an opportunistic check returned before the workflows
finished.

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
5. Use `-Full` immediately when behavior is inconsistent, a test failure points
   outside the touched files, broad refactoring occurred, or the classifier
   reports unknown paths.
6. After push, follow `workflowWaitPolicy`. Use `-Mode opportunistic` during
   iterative multi-commit work when deferral is allowed, but use `-Mode final`
   before final handoff. If `postPushAuditRequired` is true, the final waiter
   runs `tools\github_post_push_audit.ps1` after the relevant workflows pass.
7. If the classifier output looks wrong, fix the classifier and its tests before
   continuing feature work.

## Selector Self-Tests

`tools\test_verification_selector.ps1` verifies representative path classes:
docs-only, C++ source, archive parser, GUI, workflow, MCP, verifier, unknown
path, and forced-full mode. Verifier or MCP routing changes must run this test
directly, and the full profile runs it automatically.
