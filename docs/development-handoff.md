# Development Handoff And Release Readiness Plan

## Start Here

The [3 October intermediate beta review](beta-review-2026-10-03.md) is the current
candidate packet. It records the frozen codec identity, fresh block/effort/format
evidence, packaging asset repair and held acceptance gates. The dated checkpoint
below remains historical evidence. The maintainer requested a goal pause once
the review packet is ready; release publication still requires explicit approval.

Prepared on **2 October 2026** for another agent or programmer to continue
without relying on this chat. This is a development checkpoint, not enterprise
readiness, final security acceptance or permission to publish a release.

The accepted implementation baseline is
`e6848d1998c9fcfb13c9e136d688423b40ec10f9` on `main`, aligned with the GitHub
branch when checked. The subsequent handoff commit also separates test object
compilation from executable prerequisites; resolve the current `HEAD` rather
than expecting these documents to name their own commit. No codec prototype is
being handed over. Development remains on `main`.

Use [AGENTS.md](../AGENTS.md) and its domain reading map as the normative rules;
this document does not replace or duplicate the operating guide. The wider
[implementation ledger](../IMPLEMENTATION_PLAN.md) preserves prior work. Its
chronological entries are historical evidence, not a current completion list.

**Release 0.8.0 is on hold.** Complete the gates below, present the reviewable
application, results and documentation, then obtain explicit maintainer approval
before dispatching publication. Do not reuse the earlier authorization to
replace 0.5.0, delete a release, or silently choose another version.

## Verified Checkpoint

Observations below were collected after the C++ job completed at 08:48:53 UTC
on October 2, superseding the earlier pending-job snapshot.
Remote counts and workflow states must be refreshed at the next useful boundary.

| Area | Evidence and limits |
| --- | --- |
| Local validation of `e6848d1` | All 31 full-profile commands passed, including 565 native tests, nine allocation-failure groups, four CTest targets, the 36-format matrix, independent readers, 14 sanitizer targets, eight-page GUI review, unpublished HIP package checks and 24 MCP tests. This was the implementation batch, not a new run for this documentation. |
| Scheduling change validation | All eight classifier-selected checks pass, including the HIP build, 565 native tests, four CTest targets, contracts, repository policy, MSI identity and unpublished packaging. A fresh isolated CPU-only build of `superzip_tests` also builds its CLI/GUI prerequisites and passes 564 native tests. Generated CMake metadata confirms 63 unchanged object sources and the intended prerequisite separation. Hosted duration/coverage for this change still requires the next exact-SHA run. |
| Hosted workflows at `e6848d1` | All seven selected workflows passed: Windows CI, lint, fuzzing, Scorecard, scanner-configuration validation, explicitly dispatched graph validation and security run `36984122605`, including its secret-history scan. |
| Hosted C++ analysis | Completed analysis `1879859954`, CodeQL `2.27.1`, category `/language:c-cpp`, source `e6848d1`, has an empty analysis error. Header alerts `1639` and `1641` are fixed at 08:48:24 UTC; the alert API retains their last positive instance at the preceding SHA. |
| Code-scanning inventory | 1,739 incident records: 210 open, 905 fixed, 624 dismissed. There are 208 open alerts outside the existing two Scorecard residuals; the post-push acceptance audit exits 1. Incident counts are not counts of independently proved vulnerabilities. |
| Dependencies and PRs | GitHub returned zero open Dependabot alerts and zero open PRs. This does not prove all packages are current or all advisory sources are clear. |
| Release and live scanner | No release was created in this round. The optional live Greenbone scan has no available authorized OIDC broker; its configuration workflow passing does not mean a live vulnerability scan ran. |

Local evidence is ignored, not shipped. On this checkout, inspect:

- `out/zstd-private-header-full-verification-corrected-20261002.json`: exit 0,
  no timeout or output-limit breach; the Windows Job memory cap was
  15,816,720,384 bytes. Summary tails are not the full command logs.
- `build/Testing/Temporary/LastTest.log`: implementation batch native/CTest
  results. Check timestamps before attributing it to a different build.
- `out/code-scanning-handoff-20261002-e6848d1-completed.json`: validated all-state
  inventory after completed analysis; preserves per-alert tool, rule, path and
  analysis SHA. The earlier file without `-completed` is superseded evidence.
- `out/workflow-status/e6848d1998c9fcfb13c9e136d688423b40ec10f9.json`:
  earlier opportunistic workflow snapshot, not the latest status or acceptance.
- `out/handoff-scheduling-verification-20261002.json` and
  `out/handoff-cpu-scheduling-verification-20261002.json`: passing scheduling
  checks, with no timeout or output-limit breach. Stored output is a bounded
  tail; CPU verification does not replace the primary HIP build.

These files may not exist on another host. Reproduce evidence using the checked-in
tools and GitHub APIs; their absence must not be replaced by a passing claim.

## Immediate Next Batch

Finish the existing dictionary-training bounds review before starting another
unrelated scanner batch. No new boundary test or production repair for these
six alerts has been implemented. Prior static notes alone do not complete it.

The persistent Codex Security supplemental artifact is
`artifacts/zstd-bounds-codeql-traces-8085b5c.json`, in the repository-scoped
collection `artifacts-96a8326f28fa217283328f171ae3880b8d1608d021a2fd5a3a1b5b1dca48b1ee`.
Its digest is
`d3bf8927b0537af3b249db1ef0608dee5d626e48820391ddad3136578bca88fd`.
Read it through the Codex Security artifact reader with the current repository
target and persistent storage. If that service is unavailable, obtain SARIF for
analysis `1879690462` through the authenticated GitHub API. Do not commit raw
private reports or dump all SARIF traces into model context.

| Alerts | Imported evidence | Proof still needed |
| --- | --- | --- |
| `1501`, `1502`, `1503` | Respectively four, four and two flows from COVER/FASTCOVER dictionary allocations into XXH64 suffix reads of one, eight and four bytes. `dict + tail` is paired with `dictBufferCapacity - tail`, not the full allocation length. | Complete FASTCOVER tail/range validation, shrinking-dictionary suffix contracts, hash remainder/alignment cases and product reachability. Prove every distinct caller, not only one representative path. |
| `1504`, `1505`, `1506` | Respectively four, four and three flows from `COVER_checkTotalCompressedSize` into little-endian block writes and `op[3]`. The original destination uses `ZSTD_compressBound(maxSampleSize)`; frame/block writers consume remaining capacity. | Trace all capacity reductions and overflow assumptions through the 21 total flows. Validate tiny, exact-capacity and failing buffers, frame headers, empty/raw/RLE paths and successful controls against actual production dependency code. |

Generated source is under `build/zstd-v1.5.7-source/zstd-1.5.7/lib/`.
Relevant files are `dictBuilder/cover.c`, `dictBuilder/fastcover.c`,
`dictBuilder/zdict.c`, `common/xxhash.h`, `common/mem.h`,
`compress/zstd_compress.c` and `compress/zstd_compress_internal.h`.
Confirm the generated source identity before using line numbers from the traces.

Recommended next steps:

1. Refresh the completed C++ analysis and all six alert instances. Preserve the
   imported analysis as historical evidence if the new one differs.
2. Resolve the applicable security policy and finish a per-alert static
   assessment, retaining unknown reachability or impact rather than inferring
   exploitability from a severity label.
3. Locate existing memory-test helpers with CocoIndex, then verify exact source
   with `rg`. Design guard-page or canary tests against the production Zstandard
   target and its real APIs. Check exports/linkage before choosing a test target;
   internal XXH functions are not assumed to be DLL exports. Do not fork a codec.
4. Include successful compression/dictionary controls and independent read-back,
   so a test cannot pass merely by taking an early error path. Faults must be
   contained to a bounded test process, not risk the host.
5. Fix a demonstrated root cause through a hash-checked downstream recipe when
   needed. Preserve the upstream archive and licenses; require fresh extraction,
   idempotence, drift and interrupted-write coverage for recipe changes.
6. Run focused regressions first, then the classifier-required verification
   once the coherent batch is ready. Verify exact-SHA hosted findings after push.
   An unresolved result stays open; do not blanket-dismiss or rewrite code just
   to defeat a heuristic.

Also inspect newly reported alert `1739`, DevSkim `DS140021`, in
`tests/zstd/headers/header_contract.c:44`: `strlen(argv[0])` triggers its generic
unterminated-string warning. The fixture is compile/link validation, not an
archive parser. No out-of-bounds runtime bug has been demonstrated. Determine
whether a deterministic fixed-size hash input improves the test contract, or
retain precise counterevidence; changing the function name to evade the scanner
is not a fix. Existing alert `1734`, `DS161085`, is in the fault allocator fixture
and still needs its own evidence-based disposition.

## Ordered Implementation Workstreams

Use coherent batches with explicit entry/exit criteria. The order below comes
from remaining risk and measured costs, not alert-count targets. A change that
affects a later workstream must carry its related regression coverage now.

| Priority | Work and primary entry points | Acceptance evidence |
| --- | --- | --- |
| P0: reconcile hosted state | The `e6848d1` workflows and two header closures are verified. Inspect the subsequent scheduling/handoff commit, reconcile every report with its SHA/category and investigate new first-party findings. | Exact workflow conclusions, completed analysis identity and fresh complete alert inventory. Green scanner execution alone is insufficient. |
| P1: security root causes | Complete bounds, ownership, pointer-unit and condition batches before quality-only cleanup. The current backlog includes 22 stack escapes, 20 pointer-scaling reports, six bounds reports, four guarded frees and six boolean reports. Use `docs/code-scanning-history-review.md` and retained per-alert evidence. | Per-instance allocation/lifetime/capacity proof, reproductions where applicable, real fixes, independent regressions, full affected scan coverage and hosted closure or supported disposition. No unapproved alert is waived by this plan. |
| P2: aggregate resources | Inspect `src/core/host_memory_budget.*`, `worker_budget.hpp`, `archive_blocks.cpp`, retained host pools and HIP reservations. The three-buffer window estimate is not complete codec/process accounting. | Lifetime-aware accounting of codec workspaces, candidate payloads, descriptors, queues and staging; overflow and allocation-failure tests; varied host RAM/worker matrices; shared product/benchmark policy. Do not claim an OS snapshot reserves physical RAM. |
| P3: CPU formats and search | Review all shared streams/adapters and native candidate search, not only SUZIP. Profile context setup, repeated effort trials, checksums, copies, streaming and durable publication before optimization. | Same shipping path in CLI/GUI/benchmarks; held-out files, independent readers, all supported levels, complete archive sizes, unchanged correctness and matched end-to-end gains. Preserve lower-effort winners and error/cancellation behavior. |
| P4: GPU pipeline and kernels | Follow `docs/archive-engineering-review.md`, GPU codec documents and retained stage telemetry. Classification allocation/admission and upload are the largest demonstrated current worker costs; Mixed owned-decode materialization remains a target. | Explicit stream/event/buffer ownership, bounded host/device memory, actual HIP work, every effort and supported block size, corrupt/tail/small/large inputs, independent CPU oracles and reproducible wall-time/size gains. New wire representation requires a versioned format design and compatibility decision. |
| P5: product behavior and portability | Review CLI/GUI parity, CPU/GPU/Automatic mode semantics, settings migration, cancellation, overwrite, Unicode, destination publication, progress and missing dependencies. Entry points include `src/app/main_window_archive_support.cpp` and format registry contracts. | Product journeys and failure cases, all-page UI smoke and screenshots only for requested UI work, clean-host installation and missing-runtime tests. Current required/preferred HIP labels cannot simply be renamed CPU/GPU without correcting behavior. |
| P6: complete code and tool review | Inventory every tracked file and review source, tests, build tools, workflows, MCP, skills and documents. Evaluate duplication, stale/commented code, ownership and contracts. Update pins and tools from official releases with provenance and compatibility tests. | File-level record with review SHA, findings, changes/tests and unresolved areas; generated code traced to its generator and vendor code to exact provenance. Automated inventory is not a claim of line-by-line review. No blanket vendor comment removal or broad stylistic churn. |
| P7: final bug hunt and packaging | After code changes, revisit full user journeys, concurrency, malformed archives, failure injection, ABI/runtime loading, installer maintenance, portable/MSI parity and system-resource limits. | Full HIP profile, hosted CPU configuration, independent interoperability, fuzzing/sanitizers, release-target image checks, clean installer smoke and a closed-or-explicitly-blocking issue ledger. Tests cannot establish universal compatibility or zero defects. |
| P8: licensed final comparisons and docs | Finish comparator adapters/cache and eligible corpus manifests, then collect final repeated comparisons. Rewrite all user-facing material and release notes only after code and data stabilize. | The publication contract below, complete linked records, visually inspected plots, end-user README and consistent capability/version claims across every document. Historical plots must not appear current. |
| P9: release review | Prepare `docs/releases/0.8.0.md`, corresponding sources, notices, checksummed HIP-enabled x64 portable/MSI artifacts and actual validation facts. | All release gates below and explicit human publication approval. No release dispatch in a development or handoff checkpoint. |

The remaining quality backlog also includes 92 commented-code reports, nine
long switches, seven changed-loop-variable reports, seven documentation reports,
six header-guard reports and two unused-static reports. These are current
scanner categories, not proof that deleting the highlighted code is correct.
Review each instance against supported configurations and useful contracts.

The maintainer requests nine meaningful effort levels and better size/time
tradeoffs in every format, including GPU. All nine native settings exist, but
distinct budgets do not prove distinct sizes on all workloads. This objective
remains unmet universally. Test complete serialized sizes across levels; retain
smaller prior candidates, improve real search/coding, and report saturation.
Never pad low efforts, add delays, weaken output or mislabel uncompressed and
extract-only formats to manufacture strictly different sizes or times. Any
capability conflict remains visible for release review, not silently accepted.

## Additional Production Review Areas

These are review hypotheses and feature opportunities, not newly demonstrated
defects. Establish current behavior before implementing anything. Fold confirmed
issues into the relevant workstream rather than starting speculative rewrites.

| Area | Investigation and acceptance condition |
| --- | --- |
| Crash and power-loss recovery | Exercise interruption around archive/footer completion, flush, publication, repair and cleanup. Distinguish atomic visibility from durable storage; prove that existing user data survives failures and that recoverable staging is not mistaken for a completed archive. |
| Filesystem fidelity | Check sparse files, alternate streams, case collisions, long paths, timestamps, read-only attributes, hard links and changing source identities against declared capabilities. Review NTFS/ReFS, removable filesystems and remote shares where supported; reject unsupported semantics explicitly rather than silently losing data. |
| Queue fairness and backpressure | Test simultaneous small/large jobs, cancel/retry during resource admission, slow destinations and UI progress. Bound retained memory and avoid starvation; a fast single-job benchmark does not establish multi-job service quality. |
| Reproducibility and interoperability | Investigate deterministic archive metadata/settings as an optional documented contract. Maintain golden old-version readers and independent structural oracles; do not change timestamps or compatibility silently to improve cache hits or ratios. |
| Accessibility and input | Audit keyboard-only operation, focus restoration, high-contrast/DPI behavior and read-only accessibility exposure through the native UI. Preserve the non-selectable/non-copyable product-text rule; custom drawing is not proof of accessibility. |
| Integration contracts | Review CLI exit codes, structured results, Unicode/error encoding, cancellation and schema compatibility for automation. A developer-only command succeeding is not proof that a customer's script can distinguish partial failure from success. |
| Release authenticity and support | Review signing strategy, source-to-binary traceability, software inventory, symbols and privacy-safe diagnostics. Detect embedded build paths and accidental sensitive logs; do not publish private crash data or presume a certificate or signing service is available. |
| Portability qualification | Test missing/outdated HIP runtimes, multiple adapters, low VRAM/RAM, non-admin users, clean installations and Windows/driver/compiler revisions within the actual support contract. Version numbers alone do not prove the loaded runtime identity or usable GPU capability. |
| Operational efficiency | Profile CPU setup for short files, many-file metadata costs, retained pool fragmentation, contention and thermal drift separately from sustained codec throughput. Consider energy per GiB only with defensible counters; utilization is not energy or achieved bandwidth. |
| Supply-chain and tool lifecycle | Review lock regeneration, source archive recovery, action pin/version consistency, compromised-cache refusal and offline bootstrap failure diagnostics. Keep local developer tools distinct from shipped dependencies, with a complete inventory and licensing obligations for both. |
| Enterprise feature discovery | Evaluate authenticated encryption, password UX, integrity policy, deployment policy and support/rollback needs against actual customer workflows. These are design candidates, not approved runtime dependencies or promises. Encryption or learned selection requires an explicit format/threat/compatibility design before production code. |

### Ranked Performance Research Candidates

Evaluate these against the existing shipping paths, not as benchmark-only codecs:

1. **Allocation/transfer lifetime:** use the retained size/direction trace to find
   avoidable reservations, staging copies and repeated submissions. Test bounded
   operation-local reuse or safe overlap only after identifying completion and
   ownership requirements. Earlier blanket allocator/cache approaches did not win.
2. **Full candidate and metadata cost:** quantify per-block tables, segment
   offsets, indexes and framing on small, many-file and heterogeneous inputs.
   Compact/shared tables or cross-block dictionaries could improve ratio, but
   require bounded decode dependencies, corruption tests and a wire-version design.
3. **GPU execution efficiency:** inspect occupancy, register/scratch pressure,
   memory access, dispatch count and wave-size assumptions on actual kernels.
   Preserve a valid device-generic path; one architecture's tuning is not a
   support claim for every compiled target. Profile without using trace overhead
   as the throughput result.
4. **CPU search and checksum work:** identify redundant lower-effort trials,
   table construction and copies while preserving the best complete payload.
   Prefer proved bounds or reusable intermediate state over unvalidated heuristics
   that skip a better candidate. Include tiny tails and incompressible inputs.
5. **Many-file and stream scheduling:** examine metadata traversal, source
   admission, per-file work and output publication separately from codec threads.
   Standard-format concurrency must preserve its framing and directory ordering,
   rather than treating arbitrary independently compressed chunks as compatible.
6. **Intelligent selection:** compare a deterministic cost selector with any
   learned selector on held-out workloads only after useful backend choices exist.
   Include inference, memory, model/package size and decode dependencies. No
   ordinary archive operation should require a cloud call or model download.

Rank actual candidates again after profiling; these are opportunities, not
promised gains. A ratio gain that loses acceptable throughput, or a speed gain
that materially grows output, needs an explicit user-visible tradeoff and review.

## Planning Rounds And Decision Gates

Three planning passes informed this checkpoint:

1. Reconciled the implementation evidence, exact GitHub state, imported bounds
   traces, prior diagnostics and open release obligations.
2. Reviewed independent risks beyond throughput and scanner counts, and checked
   existing documents for stale assumptions. This caught the already completed
   immutable local CodeQL pilot; it must not be described as a failed-only setup.
3. Measured the current CodeQL job's actual critical path and separated a
   removable scheduling barrier from inherent tracing/query work. Updated the
   next-batch order, evidence requirements and publication dependencies.

Each future proposal needs a current baseline, root-cause hypothesis, expected
benefit, affected contracts, resource/format/license risks, held-out controls and
an exit criterion. Review a non-win's cause once; repeat only if new evidence or
a materially changed candidate justifies it. Larger kernel rewrites remain
eligible when a concrete data-flow/algorithm change predicts substantial benefit,
not because another scheduling tweak failed.

The release critical path is findings/resource correctness, accepted CPU/GPU and
product changes, complete review/bug hunt, final licensed measurements, final
documents/source obligations, exact-SHA acceptance, then human approval. Research,
comparator adapter work and read-only audits can proceed while hosted checks run;
do not block those independent tasks on an unchanged poll.

## Performance Evidence And Avoided Rework

The application and benchmark must continue using the same production codec
and ownership paths. Recent diagnostics are not final commercial comparisons.
Useful prior reviews are linked from the implementation ledger and include:

- `docs/benchmarks/2026-09-30-runtime-allocation-review.md`: runtime identity,
  rejected stream-ordered/combined allocations and owned-output integration.
- `docs/benchmarks/2026-10-02-decoder-context-review.md`: rejected lazy heap
  context reuse; the existing decoder already used stack contexts.
- `docs/benchmarks/2026-10-02-cooperative-crc-review.md`: accepted ordered CRC
  changes, size preservation, host context and Mixed extraction limitations.
- `docs/benchmarks/2026-10-01-pinned-output-pool.md`: accepted bounded-output
  path, counter limits and profile-specific development results.

Do not rerun unchanged non-winning prototypes. Revisit them only with a new
root-cause hypothesis, a materially different implementation and clear controls.
The aligned combined decode workspace, blanket async allocation and recent
reversed-order input-lifetime cache probe did not establish reliable wins.
Their allocation-count reduction is not a speedup. The last cache probe showed
approximately 0.65-0.69 seconds, an overlapping/noisy range; it was not adopted.

The retained HIP API trace contains 7,144 calls. The accepted offline analyzer
groups allocation sizes and transfer directions; its ten regressions preserve
prior totals. Classification allocation/admission plus upload accounted for
about 72-75% of classification worker time in the diagnostic. Worker intervals
can overlap and are not percentages of whole-command wall time.

RAM-only runs with zero archive disk writes show that an observed GPU gap in
that lane cannot be attributed to archive SSD writes. They do not identify the
remaining cause or rule out memory bandwidth, transfers, scheduling or filesystem
bottlenecks in the other lane. Only `gfx1201` was exercised locally; compiling
the six configured targets is not six-device runtime qualification.

## Publication And Licensing Work

Reuse the established [research](benchmark-research.md),
[permissions register](benchmark-permissions.md),
[methodology](comparative-benchmark-methodology.md) and
[cache contract](comparative-benchmark-methodology.md#measurement-cache).
Refresh live stable versions and exact edition/component permissions before
running or publishing; versions recorded on October 1 are not permanent latest
versions. No paid, trial or unlicensed tool is admitted by this plan.

Finish real application adapters for Bandizip Standard and PeaZip, alongside
7-Zip and Zstd. Do not count PeaZip's renamed backend as another measured
application. Hyperfine is an independent timing check; selected licensed LZ4/Zstd
modules in lzbench provide codec context, not a desktop/GPU comparison. Review
additional useful products/tools rather than inventing popularity rankings.

Integrate `tools/benchmark_cache.py` into the comparison runner: metadata
import/lookup already exists, but automatic reuse is incomplete. Bind reusable
competitor evidence to the exact methodology, harness, input manifest, binary
and dependencies, settings, reader archive, permission scope and relevant host
configuration. A chart styling change alone must not invalidate good timings;
a new competitor version or changed input must. Preserve real collection dates.

Silesia is excluded from new headline runs because constituent permission is
not established. HIGGS is a reviewed CC-BY candidate but has not been downloaded;
it is numeric data, not a complete modern mixed-file corpus. Assemble additional
individually licensed inputs and immutable hashes without copying restricted
review-site archives. Size datasets for sustained useful timings; short-file
latency can be labeled separately, never inflated into a throughput claim.

Retain RAM and filesystem lanes with their actual timing/cache boundaries and
the repository's SSD-write limits. Review any inconsistent older instructions
before implementation; do not silently expand the wear budget. Alternate run
order, observe CPU/GPU/RAM/storage/thermal context, retain invalid observations,
and adjust short inter-run recovery only when measured trends justify it.

Collect at least five samples per final published case, after implementation is
stable, not after every minor edit. Compare software to software within a common
format and separately labeled application tradeoffs, including SUZIP versus
equivalent competitor archive choices. Show complete bytes, compression and
decompression times, meaningful effort settings and exact configurations.
Every published timing plot needs legible uncertainty; deterministic sizes
should be verified rather than given invented variation. Keep dots visible,
frontiers within the same workload, and CPU/GPU labels understandable.

`tools/render_benchmark_graph.py` and the graph workflow must use the reviewed
numeric records. Generated image concepts may guide presentation but must not
invent measured points or error bars. Visually inspect all charts and every
document embedding them; keep older results explicitly historical.

Licensing remains a release blocker beyond comparator permission: verify the
exact corresponding-source offering, especially wimlib, rebuild/relink obligations,
notices and asset inventory under [the release contract](release.md#validation).
Private/paid distribution needs a separate contributor/dependency rights review.
Native obfuscation is not source-theft prevention, cannot waive license duties,
and needs measured performance, compatibility and debugging costs before adoption.

## Efficient Resume And Verification

Start with a small read-only checkpoint, not a whole-suite rerun:

```powershell
git status --short
git branch --show-current
git rev-parse HEAD
git rev-parse origin/main
gh api repos/strmt7/SuperZip/branches/main --jq .commit.sha
tools\verification_plan.ps1 -IncludeUntracked
```

Validate the remote URL without exposing credentials. A stale `origin/main` is
not the authoritative remote branch; fetch only after preserving local changes.
Discover tools before assuming filenames or flags. Use existing build/toolchain
discovery and pinned bootstrap scripts; never copy this host's absolute paths.
Another host must regenerate build outputs and obtain its own permitted runtime.

CocoIndex was refreshed at the implementation baseline: 298 visible source
files, 4,907 chunks, no errors. It lives outside the checkout. The next conceptual
query can locate buffer-boundary test helpers; exact `rg`/source reads remain
authoritative. Refresh only if source identity changes or the index is missing.

Use the classifier for each real change. Focused checks during implementation
do not replace required full escalation. This handoff also changes CMake test
scheduling, so it needs the classifier-selected build/test/package checks; it
does not change the codec or GUI and does not justify new application benchmarks.
For heavy work use the shared memory admission and bounded command runner,
below-normal priority for this task's own processes, and no arbitrary CPU cap.

At intermediate pushes sample once and continue independent work. At final
review use the normative final waiter and post-push audit; pending, failed or
unavailable gates remain explicitly non-passing. Capture exact run/job/step
timings before changing CI. Never restore compiled output into a fresh traced
CodeQL build or reduce scanner scope for speed.

The immutable local CodeQL baseline pilot completed at `ef8a58c` using CLI
2.27.1 and C++ query pack 1.9.0. Its corrected 183-query union exported 201
results matching hosted primary rule/file/ranges; secondary traces and HIP
extraction were not proved equivalent. Database creation took 571.94 seconds;
the 17.53-second corrected query run reused a database and earlier query work,
so it is not a cold whole-suite speed comparison. The ignored
`out/codeql-preflight-2.27.1/` contains this baseline, not evidence for current
source. No canonical `tools/codeql_local.ps1` wrapper is implemented. See
[the pilot's full scope](workflow-performance-review.md#completed-baseline-pilot).
Use the current hosted manual build/query configuration, fresh tracing for
changed source, pinned provenance, bounded execution and SARIF identity checks.
Query-cache reuse is not assumed incremental source analysis.
The pilot's coverage diagnostic was 340 of 368 C/C++ files, not every tracked
line. Map those remaining files to HIP, dynamic test fixtures, fuzz targets or
other configurations and close unexplained gaps. The scheduling change preserves
the existing main-test source list; it is not proof of complete repository
extraction or a substitute for device-source review.
GitHub documents [compiled-language database creation](https://docs.github.com/en/code-security/tutorials/customize-code-scanning/prepare-code-for-analysis)
and [CLI licensing](https://docs.github.com/en/code-security/concepts/code-scanning/codeql/codeql-cli);
recheck entitlement before any private/proprietary transition.

The latest hosted CodeQL job's 21m26s splits into a 46s initialization, 14m54s
traced build and 5m30s analysis. Its queries already use four threads and
14,433 MiB. The CMake change moves native test sources into
`superzip_test_objects`, retaining the executable's CLI/GUI prerequisites,
compile definitions, core usage requirements and all test source files. This
removes their observed compile-start barrier; it does not establish a particular
speedup or guarantee less than 20 minutes. Compare the next hosted timings and
source coverage before accepting a performance claim. Do not drop either query
suite, vendor/test translation units, push checks or real tracing to hit a target.

The full installed Codex Security workflow remains an explicit final task,
separate from imported-alert triage and ordinary repository scans. Follow the
repo's on-demand, one-worker policy and installed phase contracts. Retain genuine
phase evidence; do not label a partial serial review a completed exhaustive scan.
Report a tool/eligibility/access error immediately and keep independent work
moving. No access entitlement or ability to prevent platform filtering is proved
by this handoff.

## Final Acceptance Checklist

- [ ] Finish the bounds batch, then every remaining finding disposition and
  root-cause repair with per-instance evidence. Refresh Dependabot/PRs and the
  complete code-scanning history; preserve scope and secret-history checks.
- [ ] Complete resource, CPU, GPU, format, frontend/backend and portability
  workstreams with demonstrated benefits, not benchmark-only alternatives.
- [ ] Finish a tracked-file review coverage record and the final bug hunt.
  Resolve useful conditional/commented code methodically; run all required
  changed-function contracts and regression/interop/fuzz gates.
- [ ] Complete licensed, latest-version final comparisons, cache integration,
  five-sample publication records and all visually inspected graphs.
- [ ] Rewrite README and all affected documents for end users, preserve the
  beta notice, separate developer guidance, remove stale/current claim ambiguity,
  and finalize factual release notes after code and data are stable.
- [ ] Validate exact x64 HIP artifacts, target images, missing-runtime behavior,
  portable/MSI installation/repair/uninstall, checksums, notices and corresponding
  source offering. Unpublished local package smoke is not a published release test.
- [ ] Push the coherent accepted range on `main`, wait for all relevant final
  workflows including fuzzing, and pass required post-push audits. Existing
  documented Scorecard residual policy is not permission for SAST exceptions.
- [ ] Present the ready application, measured benefits, remaining supported
  limits and reviewed documents. Obtain the maintainer's explicit green light
  before publishing 0.8.0. Do not promise zero bugs or universal speedups.

The next developer should update this checkpoint as work is verified, keeping
the ledger and evidence intact. Do not mark a workstream complete solely because
its plan, inventory, workflow or test harness was created.
