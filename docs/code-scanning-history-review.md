# Code-Scanning Incident History Review

## Evidence And Scope

The initial 2026-09-30 GitHub API inventory retrieved 1,677 distinct alert IDs, covering
IDs 1 through 1,677 without gaps, across 98 rule IDs. These are scanner incident
records, not 1,677 independently demonstrated vulnerabilities. The inventory
includes all current and historical states, not only the first page or open
alerts. Counts are a dated snapshot, not a permanent policy target.

| Scanner | Open | Fixed | Dismissed |
| --- | ---: | ---: | ---: |
| CodeQL | 183 | 546 | 17 |
| DevSkim | 0 | 182 | 595 |
| Grype | 10 | 6 | 0 |
| OSV Scanner | 10 | 14 | 0 |
| Scorecard | 3 | 10 | 0 |
| Semgrep OSS | 0 | 4 | 0 |
| Trivy | 0 | 2 | 0 |
| zizmor | 0 | 94 | 1 |
| Total | 206 | 858 | 613 |

Every incident's metadata was inventoried and grouped by scanner, rule, state,
and source boundary. This review did not independently reproduce every old
incident. Source review prioritizes the newly exposed critical bounds traces,
pointer scaling, boolean conditions, and lifetime boundaries. Existing closed
records retain their original disposition; no alert was dismissed or reopened
by this review.

The post-push refresh for `e2fb955` subsequently added alert 1678 in a new
GPU-only test matrix. Its build-configuration cause and recurrence control are
listed below; closure requires a fresh hosted analysis, not a local HIP pass.

[GitHub's alert API](https://docs.github.com/en/rest/code-scanning/code-scanning)
distinguishes open, fixed, and dismissed states. A disappearing alert can also
reflect changed source coverage, compilation, or scanner behavior. Historical
closure is therefore evidence to investigate, not a substitute for testing the
current source and exact analysis commit/category.

## Recurrence Map

| Incident family | Existing control and evidence | Review requirement |
| --- | --- | --- |
| Workflow injection, mutable or mismatched action references, unsafe secret handling | Changed-file hygiene, full repository policy scan, hosted Semgrep, Actions CodeQL, and zizmor | Verify the action repository, SHA and version association; use quoted environment indirection and the OIDC broker. A syntactically valid SHA alone is not provenance. |
| Scanner and MCP dependency advisories | Target-platform hash locks, runtime checks, OSV, Grype, and post-push audit | Resolve the complete parent/child graph, check advisory data, and preserve unsatisfiable resolution evidence. A source lock update is not a completed hosted remediation. |
| Suspected credentials and weak-hash usage | Full-history secret scan, raw DevSkim SARIF, benchmark provenance | Distinguish file/corpus digests from authentication material by field producer and use. Do not rename fields, erase provenance, or generalize an earlier false-positive dismissal. |
| Bounds, allocation sizes, pointer scaling, return paths and parser conditions | Real manual Windows CodeQL build, bounded archive adapters, unit tests and sanitizer/fuzz targets | Read the complete allocation-to-use trace, guards, units, ABI and architecture. Safe typed table layouts are not proved unsafe merely by a cast; a guard is not proved sufficient without its capacity contract. |
| Borrowed buffers and stack-address lifetime | Scoped stream input reset, caller-buffer reuse tests, RAII and synchronous/asynchronous contracts | Prove every completion, error, cancellation and cleanup path. Asynchronous work must consume, copy or own input before its caller may reuse it. |
| Function contracts, switch size, unused declarations and commented code | Changed-function documentation/complexity gate, compiler checks and provenance | Improve owned source with behavior-preserving tests. Preserve upstream archives; vendor comment churn does not repair a demonstrated vulnerability. |
| Feature-gated test comparisons | CPU-only hosted CodeQL and actual HIP test execution | Follow the operating guide's fixed-matrix rule; preserve hardware guards and all backend assertions. Alert 1678 reported the effort counter as constant where required-GPU encoding always throws in the CPU-only build. A HIP test pass alone does not validate that analysis configuration. |
| Generated or newly compiled upstream source | Checksum-pinned source inputs and full manual build database | Inventory newly visible translation units before relying on previous SAST counts. Keep new findings visible until evidence-backed triage is complete. |
| Scorecard governance and binary provenance | Post-push residual-rule gate, license/provenance checks, fuzzing and security workflows | Governance residuals remain limited to the existing approved rules. Vulnerabilities or binary artifacts do not become approved because another workflow passes. |

The largest historical dismissal group is DevSkim's generic token pattern:
579 records. Existing comments identify public corpus/archive/binary digests,
Git commits, and source-package pins, with separate secret-scan evidence. Those
comments are historical evidence, not authorization to dismiss any new digest
or credential report automatically. Eight records were dismissed as "won't
fix"; that state is not equivalent to a vulnerability fix.

## Current Blockers

All 183 current CodeQL alerts originate in the Zstandard 1.5.7 source now compiled
for bounded multithreaded compression. The previous app-local prebuilt DLL did
not expose those translation units to this repository's build-traced analysis.
The unchanged version number does not mean unchanged scanning coverage.

The six `cpp/invalid-pointer-deref` alerts trace to dictionary-training
allocations. Alerts 1501-1503 follow `dict + tail` into XXH64 with the matching
`dictBufferCapacity - tail` length; the hash processes eight/four/one-byte reads
under corresponding remaining-length guards. Alerts 1504-1506 follow frame
header advancement into block writes; the header size is deducted from capacity,
and the block writer checks the required capacity. These relationships must be
included in validation; the initial severity label alone does not prove a
reachable out-of-bounds access. Boundary testing and full triage remain open.

The 20 pointer-scaling reports concern mixed-width FSE/Huffman table layouts.
The six boolean-operator reports concern eager OR conditions whose operands are
null tests or integer truth values. Neither group has been dismissed. The 22
stack-address reports include dictionary-training worker contexts and legacy
decoder history pointers; worker joins and borrowed-buffer lifetimes require
individual validation rather than a blanket upstream false-positive claim.

The remaining 129 CodeQL records are quality-rule findings: 92 commented-code,
nine long-switch, seven documentation, seven loop-variable, seven header-guard,
four guarded-free, two unused-function, and one fixme-comment report. Quality
classification does not authorize bypassing the existing open-alert gate.

OSV and Grype each report the same ten PyJWT advisories in the Semgrep CI lock.
The supported parent/child version conflict is recorded in
[the scanner dependency blocker](security-code-scanning.md#unresolved-scanner-dependency).
Scorecard also reports that unresolved dependency, alongside two approved
governance residuals. No release is authorized while these blockers remain.

## Reuse Without Repeating Work

Run the existing audit on demand, with a new report destination:

```powershell
tools/github_post_push_audit.ps1 -IncludeHistory -HistoryReportPath out/security/history-review.json
```

It saves every incident ID, scanner/version, rule, severity, state, location, and
latest analysis identity without raw reviewer text. It retains separate fixed/dismissed counts
and never uses those records to pass an unapproved open finding. Normal push
audits stay open-only. API failure, malformed evidence, duplicate IDs across
pages, and a report-file collision fail explicitly rather than returning a
misleading clean result.

Reuse a snapshot for unchanged sources and scanner results. Refresh after an
affected source, scanner version, analysis commit/category or advisory update.
For a repeated failure, compare those identities before rerunning an expensive
scan. Change instructions only for a verified root cause; use the narrowest
regression test and keep the operating guide as the normative rule source.

This reduces repeated review work. It cannot guarantee that new defects,
advisories, compiler behavior, or upstream scanner findings never occur.
