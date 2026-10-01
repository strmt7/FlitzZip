# Workflow Performance Review

## Evidence And Scope

Reviewed on 2026-10-01 using GitHub Actions run and job APIs, completed job logs,
and all ten repository workflow definitions. The measured push is
`46b1406ee9686f1cbe16ddec8b1fd06dc6acda47`. These are hosted execution timings,
not application benchmarks or proof of globally optimal CI performance.

| Workflow | Job duration | Queue delay | Main work |
| --- | ---: | ---: | --- |
| Windows CI | 6m21s / 7m17s | 3s / 4s | VS 2022 / VS 2026 builds, tests, format matrix, policy checks |
| CodeQL C++ | 21m46s | 4s | Initialization 2m04s, traced build 13m31s, analysis 5m55s |
| Other security jobs | 7s-77s | 3s-4s | Separate SAST, dependency, secret-history, and SBOM work |
| Sanitizer fuzzing | 15m09s / 15m57s | 3s / 4s | Independent undefined/address builds and intentional 600-second fuzz budgets |
| Lint | 25s | 3s | Pinned tools and changed-file lint |
| Graph verification | 5s | 5s | Reviewed-record tests and deterministic regeneration; no new benchmarks |
| Scorecard | 28s | 3s | Repository supply-chain checks |
| Offline Greenbone integration | 12s | 4s | Broker, script, configuration, and artifact contracts; no live target scan |

Job durations include action overhead; a `fuzz-seconds` input is not a promise
that the whole fuzzing step or job lasts exactly that many seconds.

Four consecutive successful CodeQL C++ jobs provide a cross-check:

| Run ID | Source | Queue | Traced build | Analysis | Whole job |
| --- | --- | ---: | ---: | ---: | ---: |
| 36898177215 | `088e292` | 7s | 12m59s | 5m37s | 20m15s |
| 36910622163 | `8e712c6` | 5s | 13m25s | 5m21s | 19m33s |
| 36913873363 | `7985f0c` | 3s | 13m58s | 5m22s | 20m45s |
| 36919000140 | `46b1406` | 4s | 13m31s | 5m55s | 21m46s |

These runs contain different commits and do not establish an optimization
speedup. On the latest run, the CodeQL CLI came from the runner tool cache.
Finalization and queries used `--threads=4 --ram=14433`, already resolved from
host resources. Do not replace this with a guessed higher thread count or a
fixed memory claim based on an action-description example.

The build configured in approximately 25 seconds. Most of its 811 seconds
were compilation plus extraction, including the GUI, vendored code, and tests.
Restoring object files to omit this compilation would also omit the traced
source. Removing tests or vendored translation units would reduce coverage,
not repair an idle delay. Ordinary VS 2022 compilation took 289 seconds; its
non-traced build is not a drop-in replacement for the CodeQL database build.

## Changes And Preserved Boundaries

- The stateless offline Greenbone integration workflow now cancels a superseded
  run on the same ref. This avoids redundant checks during successive pushes;
  it is not a measured reduction in CodeQL's critical path.
- Live Greenbone scans and release publication retain `cancel-in-progress: false`:
  they manage external scan state or publication transactions, unlike the offline
  checker. No live scan was run without an authorized OIDC broker, and no release
  was dispatched during this review.
- Independent security jobs, Windows configurations, and sanitizer lanes already
  run concurrently. No artificial sleep or cross-job dependency was added.
- Full-history secret checks, hash verification, scanner coverage, quality queries,
  fuzzing duration, MSI smoke, and release replacement safeguards remain intact.

The PR-only dependency review, manual release, and optional live scan were
reviewed statically; their current execution speed was not measured. No paid
runner, unverified executable cache, or source exclusion was introduced.

## Repeatable Follow-Up

Use the Actions API's exact `head_sha` filter, then retrieve each selected
run's jobs. Record event, attempt, runner, status, creation/start/completion
timestamps, and step durations. Separate dependent-job waiting from queueing;
do not time an unfinished step as though it had completed. Compare several
equivalent runs before attributing a gain. API failure is unavailable evidence,
not a zero-duration result. Preserve the full validation path after any change.

GitHub's [build-mode guidance](https://docs.github.com/en/code-security/reference/code-scanning/codeql/build-options-for-compiled-languages)
explains traced compilation. Its
[analysis performance guidance](https://docs.github.com/en/code-security/reference/code-scanning/troubleshoot-analysis-errors/analysis-takes-too-long)
describes resource tuning and the coverage consequences of splitting analysis.
[Dependency-cache guidance](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching)
requires treating restored contents as untrusted. Cache only a measured setup
bottleneck with an explicit trust boundary and revalidated pinned inputs; do not
cache final vulnerability decisions or assume a same-version executable is valid.

## Local CodeQL Preflight

The follow-up host-resource change removes fixed four-worker CMake/MSBuild
environment values from Windows CI and C++ CodeQL. The shared build helper now
uses logical CPUs subject to RAM admission and one aggregate MSBuild budget.
The table above describes the earlier four-worker runs; it is not a measured
speed improvement for the new scheduling policy.

Local CodeQL is supplementary feedback, not hosted acceptance. The October
pilot uses the hosted CLI version (2.27.1) and C++ query pack (1.9.0), an isolated
CPU-only validation checkout, and the unchanged production build. It never
replaces the primary HIP build, restores compiled objects into extraction,
uploads a local report over the hosted category, or skips quality queries.
Tool archives, databases, logs and source-bearing SARIF stay ignored and local.
Use the shared RAM planning budget, reserve memory for tracing plus compiler
workers during creation, and use CodeQL's `--ram` and `--threads=0` for analysis
at below-normal priority. Native RAM options are planning controls, not a hard
process-tree operating-system limit. No CPU affinity or arbitrary fixed CPU cap
is needed. Check available memory again between creation and analysis.

GitHub's [incremental-analysis prerequisites](https://docs.github.com/en/code-security/how-tos/find-and-fix-code-vulnerabilities/scan-from-the-command-line/incremental-analysis)
support C++ overlays only with `build-mode: none`, not this repository's manual
traced build. Do not change build mode to obtain overlays. Reuse cached queries
on the same immutable database for focused rule-family investigation; changed
compiled source requires fresh build evidence. Diff-only results cannot establish
whole-repository remediation. No local speed improvement is claimed until a
complete equivalent analysis has been measured.

The initial pilot failed before compilation because an intermediate native
process passed PowerShell 7 module paths into Windows PowerShell 5.1. A traced
probe reproduced the missing `Get-FileHash` command and restored it by normalizing
only that child to `$PSHOME/Modules`. No host module configuration changed.
[Microsoft documents this indirect-launch boundary](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.core/about/about_psmodulepath).
Preserve this distinction when automating traced Windows builds.

[GitHub's CodeQL terms](https://github.com/github/codeql-cli-binaries/blob/main/LICENSE.md)
permit analysis and automated database generation for an OSI-licensed codebase
hosted and maintained on GitHub.com. This applies to the current public AGPL
repository, not blanket future private/commercial use. Do not redistribute the
CodeQL binary with SuperZip; recheck entitlement if repository licensing changes.
