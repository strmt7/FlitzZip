# Fresh Modernization Audit

This review starts from `6d0ba974a28e37327591b4ab870cf60096510ac6`
on 2 October 2026. It is an ongoing implementation record, not final acceptance.
Historical findings are leads to recheck against current source and execution.

## Baseline And Review Coverage

The working tree was clean. Local and remote `main` matched. All seven hosted
workflows for this SHA completed successfully. The open-alert API returned
210 records across its three pages: 182 CodeQL, 26 DevSkim and two Scorecard.
These are scanner observations, not 210 independently confirmed vulnerabilities.
The installed HIP build reports a working gfx1201 device and eligible
stream-ordered allocation. Eligibility alone does not prove an allocation
strategy's correctness or performance.

The first read-only refactor audit reports six large files: the main-window
header, HIP codec, two native test files, benchmark script and GUI smoke script.
It reports no oversized functions under its default thresholds. File size is
a navigation/refactoring signal, not sufficient reason to rewrite working code.

Inventory and review are distinct. The initial tracked-domain counts are
189 source files, 78 test files, 89 tools, 69 documents and 29 GitHub files.
They do not establish that every file has been reviewed. Generated Zstandard
source and pinned upstream archives require separate provenance-aware review.

| Domain | Current review and next implementation criteria |
| --- | --- |
| Native GPU codec | Exact input-allocation/classification path inspected. Stream-ordered input allocation was tested and rejected below. Inspect bounded input reuse, entropy, dictionary and decode kernels separately. |
| Native CPU codec | Nested Deflate/Zstandard effort selection inspected. Preserve earlier smaller candidates and independent readers while profiling repeated search, context ownership and copies. |
| Resource accounting | Host snapshot admission, aggregate worker shares, device reservations and pinned-output policy located. Complete workspace/pool accounting remains open. |
| Compatibility adapters | All 36 registry rows inventoried through the support contract. Review each parser/filter/publication path; distinguish create, extract-only and recognized-only support. Format-matrix success is not a timing comparison. |
| GUI and CLI | Existing parity, settings, queue and telemetry regression contracts identified. Reproduce each proposed behavior change before patching; retain native rendering and all-page smoke. |
| Security and dependencies | Hosted state refreshed. Dictionary-training bounds and first-party harness findings remain open; use real dependency code, exact-capacity tests and per-caller analysis. |
| Build and workflows | Current test-object scheduling change and hosted status checked. Compare exact job/step timings and traced coverage before further CI changes. |
| Skills and MCP | Repository skills loaded; local search index refreshed. Review routing, containment and cancellation without adding workers or unapproved dependencies. |
| Documentation and release | Current handoff and capability/benchmark contracts read. Update measured claims only after verification; release readiness and publication remain separate gates. |
| Comparisons | Existing comparison harness and publication criteria located. Inspect available pinned tools/corpora; expand compatible format cases before collecting final repeated application comparisons. |

## Iteration Plan

1. Reproduce the clean HIP correctness baseline and preserve baseline binaries
   outside source control. Collect RAM-only timing diagnostics with resource
   context before changing the input-allocation mechanism.
2. Evaluate stream-ordered input allocation in a narrow production experiment.
   The current per-thread copy/kernel stream must order every use; frees must
   finish before aggregate reservation release. Unsupported configurations
   retain the existing HIP allocation path. No pool retention policy is changed.
3. Run native corruption, lifetime and roundtrip regressions, then matched
   repeated diagnostics. Retain an implementation only with evidence; a non-win
   becomes a recorded experiment, not a performance claim.
4. Complete the dictionary/compression bounds regressions and first-party
   harness contract review. Continue through resource accounting, codec search,
   compatibility streams, GUI journeys and build/tool findings in coherent
   batches, applying the classifier to each actual change set.
5. Finish file-level coverage and final bug-hunt rounds. Collect eligible
   repeated comparisons and inspect charts only after code and data stabilize.
   Preserve remaining failures and unsupported features in the issue ledger.

GPU experiments follow AMD's [memory-management guidance](https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/hip_runtime_api/memory_management.html)
and the repository's stricter Windows runtime/pool admission rules. General
modernization follows the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines):
explicit ownership and measured performance, without changing the approved
C++20/HIP boundary.

## Round One: Input Allocation Experiment

The candidate changed only input allocation to the admitted per-thread
stream-ordered allocator, retaining legacy allocation elsewhere and completing
frees before reservation release. No runtime, driver or pool policy was changed.
All four CTest targets passed for the baseline and candidate, including concurrent
128 MiB allocation/roundtrip and zero-live-pool checks.

Each diagnostic used 10 GiB of generated RAM-only input, effort 5, 8 MiB blocks
and three required-HIP repeats. The return-to-baseline series reversed profile
order. Every run verified bytes and reported zero archive disk writes. Complete
archive sizes stayed identical across baseline, candidate and return runs.
These GPU-only diagnostic batches were sequential rather than interleaved
binary pairs; resource records and observed dispersion remain necessary context.

| Profile | Baseline compression median (s) | Candidate median (s) | Return median (s) | Complete archive bytes |
| --- | ---: | ---: | ---: | ---: |
| Mixed | 3.45229 | 3.28492 | 3.00564 | 4,527,857,731 |
| Compressible | 1.66677 | 1.75236 | 1.66187 | 1,073,857,115 |
| Incompressible | 2.01751 | 2.20165 | 1.87529 | 10,737,441,371 |

The Mixed baseline contained a 4.91-second first run, followed by 3.15 and
3.45 seconds. Candidate results do not establish a reliable gain. Compressible
and Incompressible compression were slower than both controls. Input upload
worker time fell while allocation worker time rose, so the apparent transfer
improvement moved waiting between stages rather than proving faster completion.
The candidate was removed from production; exact candidate source/binaries and
all nine diagnostic records remain under ignored `out/fresh-audit-*` paths.
This rejects this mechanism on the tested setup, not all reuse/overlap strategies.

## Round Two: Executable Bounds Contracts

The new `superzip_zstd_bounds_tests` target links the production `libzstd_shared`
DLL rather than copying a codec. Its three regression groups cover:

- 74 dictionary-content lengths, including 0-65 for hash remainders/alignment
  and larger suffix boundaries, with byte-identical paired final dictionaries
  and successful dictionary compression/read-back.
- 96 COVER/FASTCOVER capacity/width/shrink/method combinations, including both
  direct and optimized builders, with real successful read-back controls.
- 99 source-family/length/effort combinations, each at 12 destination capacities:
  empty, RLE, incompressible and low-alphabet inputs, block/tail boundaries,
  exact final-frame size and full `compressBound` controls.

Every guarded extent ends immediately before an OS-protected page. The test
verifies that page is unreadable and checks prefix canaries after success and
failure. All data stays in RAM. CTest bounds the target to 60 seconds.

The initial frame test incorrectly assumed that a capacity equal to the final
frame size must succeed. For empty input at effort 1, a nine-byte final frame
still requires greater capacity in the writer. Zstandard's
[1.5.7 API contract](https://facebook.github.io/zstd/doc/api_manual_v1.5.7.html)
guarantees the sufficient bound rather than that minimal capacity. Assertions
now require only destination-size errors below that bound, bounded successful
writes and exact decompression. Full-bound controls must succeed and match the
reference bytes. The original failed test output is retained as diagnostic
evidence rather than represented as a library defect.

The header inclusion fixture now hashes a fixed byte array with an explicit
extent. Runtime executable-name string parsing served no header/link contract;
it was removed while preserving staged and repeated C/C++ inclusion checks.
No archive behavior or upstream provenance was changed.

All three new bounds groups, language lint and changed-function contracts pass
locally. The subsequent full-profile verification passed all 29 selected
commands: five CTest targets, the 36-format matrix, independent interoperability,
security policy checks, bounded sanitizer smoke, GUI smoke and package validation.
All eight page screenshots were reviewed. A final five-command targeted rerun
also passed after the unavailable-parent assertion was added. Pinned DevSkim
reported no findings in the four changed native/fixture test files.
Exact-SHA hosted assessment remains pending.
These finite regressions do not complete the 21-flow static review or establish
that every input, vendor component or scanner observation is safe.

## Round Three: Reproduced Timing-Test Failure

The first full-profile attempt stopped in the existing classification timing
test. A diagnostic preserving its original assertion reproduced the failure
on repeat 18: all nested counters summed to **11,191 microseconds**, exactly
equal to the enclosing counter. Summing the separately converted seconds
exceeded the converted parent by **1.734723e-18 seconds**.

The direct production regression now compares exact integer counters while
retaining finite, positive reported-time checks and byte/CRC oracles. The
archive-level check has only an eight-machine-epsilon relative allowance for
its four converted values; it does not tolerate a microsecond of extra work.
Production telemetry and codec behavior are unchanged. This resolves a
demonstrated validation flake, not an application performance defect. The
original full-profile failure and diagnostic output remain in ignored evidence.
The corrected full profile and final targeted rerun passed, as recorded above.

## Round Four: Development Context And Mandatory Skills

The [development-context index](development-context.md) routes to existing
sources instead of copying policy or volatile current-state values. A new
standard-library helper supplies exact heading/line windows, explicit per-context
read reuse, and short cited notes with source hashes and expiry. Partial reads
retain exact unread ranges and cannot enter the cache. Context resets require a
new session or forced source output. Stale notes withhold their synthesis;
unchanged citations do not establish correctness or acceptance.

Caveman and CocoIndex are explicitly mandatory in AGENTS.md. Startup delivers
their current full instructions; successful CocoIndex searches record local
source/config/query hashes and completion metadata. Search checks source
freshness again after retrieval. Neither installation nor a stale receipt is
proof of actual current routing, and instruction delivery cannot prove model
behavior. The existing exact-search boundary and single-agent rule remain.

On the current operating guide, the complete Git Workflow section emitted
4,464 numbered source characters from a 70,021-character file. Its JSON response
was 4,733 characters; a repeated fully read window in the same explicit context
was 206 characters. These are emitted-character measurements, not token billing,
latency, correctness or whole-development speedup measurements. Raw records and
note/read demonstrations remain in ignored `out/fresh-audit-context-*` artifacts.

The source-thread review retrieved 1,132 comments across 12 pages and screened
every record before targeted technical reading. It did not load all 1.75 million
characters into the model. Current primary sources and conflicting 2026 agent
evaluation results are linked in the context guide; forum product performance
claims were not adopted as evidence.

The frozen latest-source full profile passed all 32 required commands, including
25 context/search contracts, 24 bounded-child contracts, selector regressions,
HIP-enabled native tests, the format matrix, independent interoperability,
security checks, sanitizer smoke and packaging. The eight current GUI page
screenshots were reviewed without an observed layout defect. Language lint and
changed-function contracts passed. Exact-SHA hosted assessment remains pending;
this is an intermediate development checkpoint, not final product acceptance.

At the subsequent hosted boundary, all six workflows triggered by application
checkpoint `91c38ac` had completed successfully. The fresh post-push history
audit nevertheless failed: 208 unapproved open scanner records remain, including
six Zstandard pointer-bounds alerts. Deployment count was zero. The inventory
and failure log are retained under ignored `out/fresh-audit-post-push-app-*`;
neither test success nor green workflows resolves that backlog.

## Round Five: Stable Semantic Index Identity

The next actual search refused the index after staging/committing new context
sources. A real temporary Git checkout reproduced the cause: Git emitted
untracked `z.py` before tracked `a.py`, then reversed that inventory order after
staging without changing either file's bytes. The previous digest hashed the
enumeration order, creating a false freshness mismatch.

The digest now sorts repository paths before hashing their names and exact
bytes. A real-Git regression proves unchanged staging preserves the fingerprint
and a subsequent content mutation invalidates it. This removes a demonstrated
unnecessary refresh; it does not bypass actual source/config freshness checks.
All 26 context/search contracts and the changed-function gate pass. The frozen
follow-up full profile passed all 32 required commands; its eight current GUI
page screenshots were reviewed without an observed layout defect. All seven
hosted workflows for `549091a` passed when refreshed during the next round.
This is checkpoint validation, not whole-repository security acceptance.

## Codec Workspace Observation And Next Admission Work

A bounded, below-normal-priority diagnostic loaded the identity-verified
production `libzstd.dll` (`10507`) and exercised its real reusable-context ABI.
Two seeded fixtures (random bytes and a repeated 4 KiB random record), eight
block lengths (4 KiB plus all seven production choices), and efforts 1 through
9 produced 144 exact readbacks. Archive data stayed in RAM; JSON evidence stayed
in ignored output. This was a memory observation, not a throughput benchmark.

| Input block | Largest observed retained context | Three input-sized buffers |
| --- | --- | --- |
| 4 KiB | 106,520 bytes | 12,288 bytes |
| 256 KiB | 3,138,584 bytes | 786,432 bytes |
| 1 MiB | 11,002,904 bytes | 3,145,728 bytes |
| 16 MiB | 11,002,904 bytes | 50,331,648 bytes |

The existing documentation's small-window workspace gap is therefore reproduced.
These context observations exclude caller buffers and metadata and are not
worst-case bounds on arbitrary input. The pinned DLL also exports the one-shot
`ZSTD_estimateCCtxSize` API; its level-nine estimate is 13,100,056 bytes under the
upstream header contract. That contract excludes streaming and does not bound
total process memory. The subsequent implementation is recorded in round six.

Keep the four-block cap while deriving aggregate worker/context, candidate-buffer
excess, metadata, queue and optional-fallback admission. Production and RAM
validation must share the eventual policy. Source-hashed, expiring local notes
preserve this lead without promoting a hypothesis to acceptance. The raw
observations and estimate records are `out/fresh-audit-workspace-*-20261002.json`.

## Round Six: Native CPU Workspace Admission

Production native compression and RAM validation now share a CPU workspace
estimate in addition to the existing three-buffer policy. It accounts for
candidate/final block metadata, aggregate retained contexts, trial bytes beyond
raw and short Deflate tails that can coexist with Zstandard context/trial
storage. The four-worker window ceiling now has one shared constant used by
both admission and dispatch. The pinned one-shot context estimator covers all
effort levels through the selected maximum without changing encoded data.
Queue arithmetic avoids unsigned allocation-cost multiplication overflow.

Required-HIP compression preserves its GPU-only contract. Optional-HIP fallback
includes CPU storage even when the current device is available. Small-file CPU
fallback reserves bounded output capacity once rather than growing it during
member concatenation; its extra member/output overlap and metadata are admitted.
This does not reserve memory against other processes or account for all process,
manifest, streaming, thread-stack and GPU-pool memory.

The HIP build and all 11 focused memory tests pass, including 16,384
synthetic concurrency geometries, exact boundaries and actual runtime context
size/readback checks across nine effort levels. The frozen full profile passed
all 30 selected commands, including 570 native tests, production-DLL bounds and
fault groups, the 36-format matrix, independent readers, sanitizer smoke,
unpublished packaging and 26 context/search tests. The bounded result retains
output tails, not a complete log; it reports no timeout or output-limit breach.
Eight current GUI page captures were reviewed without an observed layout defect.
The final seven-size CPU/HIP sweep also passed exact readbacks with zero
archive-data disk writes, and every CPU archive size matches its baseline.
The seven-size baseline
CPU sweep used 10 GiB Mixed input at level five, verified readback and reported
zero archive-data disk writes. Its recorded executable hash identifies the
pre-change binary; source edits began during the sweep, so its JSON correctly
marks the source tree dirty. This is diagnostic baseline evidence, not a
clean-source published comparison or a speedup claim. Timing changes have both
signs, including slower large-block CPU observations; this single sequential
baseline/candidate pair cannot attribute the differences to the admission change.
Adaptive repetitions and matched controls are the next measurement refinement.

## Round Seven: Scientific Sampling And Configuration Control

The RAM controller now uses retained pilot observations to prescribe a fixed
confirmation count before confirmation begins. Duration and phase/total sample
variation determine the request; a declared count ceiling bounds the work.
All observations receive equal weight. Sample SD uses `n - 1`; the RSE
diagnostic assumes independence and is not a confidence or significance claim.
No timing outliers are deleted, and noisy or incomplete evidence remains
inconclusive. Process/output/suite deadlines and an exclusive append-only
journal retain terminal failures without silently retrying observations.

Two diagnostic experiments exposed controller defects before publication.
The first mutated a shared plan array while reversing even-round order; a
cloned plan and three-round regression correct it. The second retained 42
pilot and 144 confirmation observations before intentional termination because
per-case queue depths varied with available RAM. All 186 completed observations
remain in its journal with explicit failure/abort records; no final comparison
was published. These are invalid fixed-configuration timing comparisons,
not evidence of a codec failure or speedup.

Native planning and execution now share admission resolution. The controller
freezes encode/decode geometry before the pilot and preflights exact depths
before each observation. Current memory limits remain mandatory: an unsafe
depth fails instead of being silently reduced. Cross-stage geometry or exact
size changes abort the entire experiment at the first detected mismatch.
The graph validator independently checks frozen plans, retained samples and
quality limits, and current charts expose full observed timing ranges without
presenting them as confidence intervals.

An integration smoke caught a second parser that still required measured
`seconds` on planning output, before any workload ran. The parser now accepts
explicit planning records; a real subprocess reader/parser regression covers
both diagnostic-output settings. The corrected single-pass sweep completed all
14 CPU/HIP cases across seven block sizes with 10 GiB Mixed input at level five,
exact readback and zero archive-data disk writes. All CPU and GPU archive sizes
match the prior retained observations. Encode depth 25 was predeclared from
the prior trace's minimum CPU admission, retaining the 32-worker budget;
decode depth remained 25 for CPU and 4 for HIP. It is a configuration-controller
correctness diagnostic, not a maximum-throughput benchmark. Every timing
result remains inconclusive and is refused by the publication validator.

Complete released-tool updates are scoped to
[Ruff 0.16.10](https://pypi.org/project/ruff/0.16.10/),
[clang-format 23.1.2](https://pypi.org/project/clang-format/23.1.2/),
[CI Python 3.14.8](https://www.python.org/downloads/release/python-3148/)
and the provenance-pinned
[Vulnetix 3.108.4 action](https://github.com/Vulnetix/cli/releases/tag/v3.108.4).
Their official release metadata was checked on October 2. CocoIndex Code remains at its current
0.2.41 release, with its application dependency graph preserved. A proposed
standalone transitive-library update was withdrawn without applying it.
No app dependency graph is overridden to force individually newer libraries.
The Codex app update check returned `unavailable` because of
`missing current windows package family`; that is not confirmation of the
latest installed release. Host-wide application, OS and driver upgrades remain
unverified and have not been applied during unrelated host work.

The initial configuration-controlled full profile passed all 32 selected
commands, including the 36-format matrix, independent readers, packaging,
sanitizer smoke and all-page GUI checks. All eight current page captures were
reviewed without an observed layout defect. The frozen rerun after the parser
correction also passed all 32 selected commands, with no timeout or output-limit
breach. Its stored output is bounded tails, not a complete console transcript.
Exact-SHA hosted assessment remains pending.
The whole-repository audit, hosted security findings, controlled speed
comparisons and release acceptance remain open.

## Round Eight: Shared HIP Build Dependencies

The GPU hot-path review found an incremental-build correctness gap before
introducing allocation reuse. The five HIP custom commands used an incomplete
shared header list, omitting transitive inputs such as the device-memory budget
and allocation policy. They also shared unrelated header dependencies, causing
unnecessary GPU recompilation. Runtime codec behavior is unchanged in this round.

All five translation units now use the same compiler-derived dependency path.
CMake consumes a per-object Make-format dependency file through its documented
[DEPFILE support](https://cmake.org/cmake/help/latest/command/add_custom_command.html).
A real Windows test exposed that this HIP compiler's ordinary dependency output
omits device-only inputs. LLVM documents the
[host-only dependency-file change](https://lists.llvm.org/pipermail/all-commits/Week-of-Mon-20250203/202575.html)
that avoids multi-architecture writers colliding. The shared wrapper therefore
combines host dependencies with line-marker inputs from each requested device
preprocessor pass, using identical language, optimization, define and include
options. It discards the preprocessed code as it streams. No SDK files or compiler
packages are modified, and the build does not initialize GPU compute.

Artifact cleanup is centralized. Missing dependency data, device-scan failures,
native exceptions and unsuccessful compilation invalidate the attempted object
and dependency file rather than leaving apparently current output. Path escaping,
the six declared release architectures, shared compiler flags and seven failure/
success scenarios have automated wrapper regressions. A retained real Windows
HIP/CMake fixture verifies no-op builds, selective transitive-header invalidation,
unrelated-header exclusion, new includes, failure/recovery, and device-only and
architecture-only inputs with two compiled architectures. It includes a build
path containing spaces and confirms edited dependencies change emitted code.
These are dependency-contract diagnostics, not archive throughput benchmarks or
hardware compatibility claims. The targeted run caught three helper naming/
command conventions in lint. Those were corrected, and the subsequent frozen
full profile passed all 30 selected commands, including 570 native tests,
independent interoperability, the 36-format matrix, sanitizer smoke and packaging.
All eight current GUI page captures were reviewed without an observed layout
defect. The retained verifier output is bounded tails, not a complete transcript.

The rebuilt binary also passes all 14 CPU/HIP RAM readback cases across seven
block sizes using the prior fixed geometry, 10 GiB Mixed input and level five.
Every exact archive size and geometry field matches the prior retained sweep;
archive-data disk writes remain zero and all seven GPU cases record real HIP
work. The source remains explicitly dirty in this development diagnostic.
Thirty-six host samples retain unavailable-counter and collection-error records.
One-sample timing attribution remains inconclusive, and the independent graph
validator refuses publication even when dirty-source preview is allowed.
All seven selected hosted workflows pass on `25ff5ae`, including the explicitly
dispatched benchmark graph, both Windows test lanes, security and sanitizer
fuzzing. This establishes hosted workflow completion for the build-dependency
round, not closure of the existing security-alert backlog or release acceptance.

The broader modernization now explicitly includes review of previous special
cases for consolidation into shared contracts and ownership/resource policies.
Format semantics, required-HIP boundaries and measured correctness remain the
criteria for retaining necessary exceptions.

## Round Nine: Bounded Operation-Owned HIP Input Reuse

The next candidate moves the existing HIP allocation/reservation helpers into
one shared header and adds a bounded operation-owned input cache. Production
large-file encoding, small-file batches and RAM benchmarks use the same factory,
lease owner and admission policy. Required HIP enables reuse; forced CPU and
optional fallback retain the existing direct path. No external runtime dependency,
archive format, input upload or compression selection policy changes.

Active, idle and pending buffers share the admitted queue cap; each allocation
fits the native chunk limit. Idle storage retains its global device reservation
until a checked physical free succeeds. Returns validate the pointer, exact
extent, unique lease identity, selected device and borrowing thread before
synchronizing the per-thread stream. Checked operation closure precedes final
publication, and failed destructor cleanup retains conservative admission.
These contracts follow AMD's
[stream synchronization documentation](https://rocm.docs.amd.com/projects/HIP/en/latest/doxygen/html/group___stream.html).
They do not establish validation on every GPU or driver failure mode.

The initial HIP build and real ownership/concurrent-readback tests pass. One
policy test incorrectly treated default codec options as optional HIP; the
default is required HIP. Correcting that test is followed by all 30 full-profile
checks and 573 passing native tests. Further review preserves hardware-free
construction/closure for empty operations in CPU-only validation; an actual
MSVC fixture compiles the production pool with HIP disabled and requires an
explicit error on nonempty GPU acquisition. The refined product suite has 574
passing native tests, and all 12 selected product checks pass, including
interoperability, the format matrix, security policy, sanitizer smoke and
unpublished packaging. Changed-code lint and function contracts also pass.

The candidate is rejected and removed from production after diagnostic timing
failed to establish a useful improvement. Its implementation and application
bundle remain under ignored local evidence, with hashes and a source patch.
The checked-in production paths are restored to `25ff5ae`; the experiment adds
no runtime cache, allocator header or format behavior to the accepted product.

The comparison preserves an immutable application baseline and the candidate's
binary identity. It uses canonical parsing, counters, deadlines, geometry and
statistics, with 10 GiB Mixed input, level five and all seven block sizes.
Encode/decode geometry is fixed at 32 workers, 25 encode windows with one codec
worker each, and four decode windows with eight codec workers each. Three pilot
rounds produce 42 observations. A private controller fails after recording its
fixed plan because PowerShell cannot measure an ordered dictionary's key as a
property. The recovery projects the numeric property explicitly and normalizes
JSON geometry to a dictionary. Admission, retained identities, recomputed plans,
all 88 scheduled confirmations and serialization are checked before resumption.
The original aborted journal stays byte-identical; a separate recovery journal
references its digest. Every pilot and confirmation observation remains retained.

All 130 measured operations complete byte-exact HIP readback, unchanged exact
archive sizes, `memory_only=true` and `disk_write_bytes=0`. The following means
are diagnostic descriptions of the complete confirmation sample. Positive
changes mean longer candidate compression time; they are not significance or
regression claims.

| Block KiB | Pairs | Baseline mean seconds | Candidate mean seconds | Candidate time change | Exact archive bytes |
| --- | --- | --- | --- | --- | --- |
| 256 | 8 | 4.770969 | 5.010114 | +5.01% | 4572118255 |
| 512 | 6 | 4.847045 | 4.937897 | +1.87% | 4567776791 |
| 1024 | 6 | 4.583490 | 4.608620 | +0.55% | 4546487579 |
| 2048 | 6 | 4.507455 | 4.441810 | -1.46% | 4535842123 |
| 4096 | 6 | 4.088757 | 4.135212 | +1.14% | 4530520631 |
| 8192 | 6 | 3.982880 | 4.062240 | +1.99% | 4527857731 |
| 16384 | 6 | 3.960428 | 4.013427 | +1.34% | 4526528555 |

Reanalysis exposes a production statistics defect: `Math.Max(0, m2)` selects an
integer overload on Windows PowerShell and rounds small Welford variance sums
to zero. Consequently the original standard deviations, quality diagnostics and
pilot requests are invalid. The separate corrected analysis changes no raw data
and removes no samples. Correct pilot requests are 62 at 256 KiB and seven at
512 KiB, rather than 55 and six; the actual confirmation schedule therefore
fails the corrected plan at 512 KiB. No post hoc samples are appended. Correct
compression CVs include 9.58% for the 256 KiB candidate and 8.24%/6.68% for the
2048 KiB baseline/candidate. These data support no acceptance claim.

The monitor retains 146 host snapshots without a terminal failure, but 69 contain
counter errors or invalid entries. Available RAM stays above 26 GiB. GPU engine
aggregation sometimes exceeds 100%; the private controller does not timestamp
each observation, so exact alignment with background host activity is unavailable.
Per-operation counters and HIP telemetry remain recorded. Allocation telemetry
falls by 7040 MiB of cumulative traffic per operation, while the nested allocation
worker interval remains approximately seven seconds. Cumulative allocation bytes
are not peak VRAM, and worker intervals are not elapsed time. This is insufficient
benefit to retain the additional ownership implementation. Broader operation-owned
scratch reuse remains a profiling hypothesis, not a validated improvement.

Local evidence: `out/gpu-input-comparison-20261002.samples.jsonl` (original
pilot and abort), `out/gpu-input-comparison-resumed-20261002.samples.jsonl`,
`out/gpu-input-comparison-resumed-20261002.json` (original faulty analysis),
`out/gpu-input-comparison-corrected-analysis-20261002.json` (corrected descriptive
analysis), and `out/gpu-input-rejected-candidate-20261002/` (preserved candidate).
None is a published benchmark or release artifact.

The initial whole-tool freshness review found Windows HIP SDK 7.2 on
[AMD's official download page](https://www.amd.com/en/developer/resources/rocm-hub/hip-sdk.html).
That legacy download stream is superseded for this migration by the maintainer's
explicit ROCm Core SDK 10 request and AMD's native Windows tarball distribution.
The comparison above kept the previous compiler/runtime unchanged. Bundled
components are never individually overwritten.

## Round Ten: Fractional Benchmark Variance

The shared Welford calculation now uses `Math.Max(0.0, m2)`, preserving double
precision through the nonnegative variance clamp. A focused regression first
reproduces the old zero-variance failure, then passes after the correction.
Tests cover fractional timings at three unit scales, CV/RSE invariance and a
fractional pilot that must request 21 confirmations rather than three. Existing
slow-tail retention, single-sample uncertainty, invalid-input rejection, count
capping and frozen scheduling checks continue to pass. The first new fixture
needed parentheses around arithmetic array elements before it reached the
intended variance assertion; that fixture correction is retained in the record.
The production input-cache experiment is removed before the final frozen checks.
The HIP rebuild and all 30 commands in full intermediate verification pass,
including 570 native tests, the 36-format matrix, independent interoperability,
bounded sanitizer fuzzing, packaging and all-page GUI smoke. Current normal and
compact screenshots for all eight pages were reviewed. All seven selected hosted
workflows pass for the statistics repair at `cf4d124`. Final acceptance and the
wider audit remain open. No timing advantage is claimed.

## Round Eleven: ROCm Core SDK 10 Distribution Validation

The maintainer explicitly authorizes migration to the complete ROCm 10
distribution. AMD's [release notes](https://rocm.docs.amd.com/en/docs-10.0.0/about/release-notes.html)
and [installation guide](https://rocmdocs.amd.com/en/latest/install/rocm.html)
were checked before provisioning. The complete official Windows multiarchitecture
tarball downloaded over HTTPS and extracted into an isolated ignored workspace
directory. No existing SDK, driver, security setting or global environment was
changed. All 14,990 archive entries were checked for unsafe Windows paths,
collisions, links and bounded sizes before extraction.

The downloaded archive is 4,796,456,804 bytes with locally observed SHA-256
`ebe454fe9ad663655177462187a4c86c72fd0537638f6cbea34660ddebf40056`.
This digest is not an independently published AMD checksum. Its manifest reports
`rocm_version=10.0.0` and `rocm_package_version=10.0.0rc4`, while its compiler
reports HIP `7.15.26333-6b0e43f341` and AMD Clang `23.0.0git`; the bundled runtime
reports `10.0.3581.0`. The release notes instead list HIP 10 and LLVM 24. This
distribution discrepancy requires investigation before feature/version gates or
new performance claims can rely on the documented versions. Component files
remain exactly as supplied by AMD; no component substitution is authorized.

Local evidence includes `out/rocm10-archive-inventory-20261002.json`,
`out/rocm10-extraction-result-20261002.json` and the extracted upstream
`share/therock/therock_manifest.json`.

The repository now shares one whole-distribution lock and provisioner across
local development and hosted releases. Roots derive from the checkout, external
installation configuration or explicit `-HipPath`/`--parent` arguments. No
personal installation path is committed. The provisioner supports safe fresh
extraction, byte-exact manual-install adoption, critical-input cache checks,
collision preservation and bounded HTTPS download/inventory/extraction. Actual
fresh provisioning also passes at a different directory containing spaces.

CMake passes its selected root explicitly to each HIP compiler/dependency pass.
The build script refreshes that root on configure, removing the old environment
versus CMake-cache mismatch. Compiler-only AMD settings are scoped and restored
on success and failure. An isolated probe compiles and preprocesses all six
release targets with MSVC 14.44 and returns byte-exact output on the available
`gfx1201`. MSVC 14.51 instead reports conflicting host/device math declarations.
No AMD or Microsoft package/header is patched to force compatibility.

Runtime isolation identifies a concrete environment hazard: the same fixture
loads the same System32 runtime, but crashes with `0xC0000005` when the new SDK's
`LLVM_PATH` remains present and passes when it is unset. The helper confines that
setting to compilation. This observation covers one host/driver configuration,
not every AMD system. Current driver runtime file version is `10.0.3679.0` and
its HIP runtime API reports `70260201`; these are distinct version namespaces.

The migrated product build and 570 native tests pass. The first full check stops
at the retired installer's timeout-message guard; the policy is then migrated to
the shared provisioner's finite download/inventory/extraction and environment
isolation guarantees. Offline regressions cover timeouts, excessive/short data,
hash mismatches, changed SDK bytes, path collisions and preservation. The security
scan and lint pass after this correction. The second full verification passes
all 33 commands, including the 36-format matrix, independent interoperability,
bounded sanitizer fuzzing and packaging. Current normal and compact screenshots
for all eight pages were reviewed. Exact-SHA hosted assessment remains pending.
Existing HIP events measure elapsed telemetry;
disabling their timing blindly would invalidate those measurements. No new SDK
speedup or archive-ratio advantage is claimed.

Additional local evidence is `out/rocm10-adoption-result-20261002.json`,
`out/rocm10-portable-provisioning-result-20261002.json` and
`out/rocm10-compatibility-20261002/result-scoped-six-targets.json`. The first full
failure remains recorded in `out/rocm10-full-verification-result-20261002.json`.
The corrected pass is recorded separately in
`out/rocm10-full-verification-corrected-result-20261002.json`.
All five production HIP objects and dependency scans also compiled for all six
release targets using separate SDK and build directories containing spaces;
`out/rocm10-production-six-targets-result-20261002.json` records the bounded pass.
The independent scientific comparison using the corrected statistics remains open.

## Round Twelve: Installation Path Discovery

A fresh CocoIndex search routed installation discovery to the WiX helper and
HIP compiler wrapper; exact source inspection found remaining `C:` drive
assumptions in their Program Files fallbacks. Both now use Windows' known
Program Files folders. Visual Studio's configured root and `vswhere` discovery,
and .NET's PATH candidates and SDK-capability probe, remain intact. No machine
installation or global setting changes. Actual .NET SDK discovery succeeds
without installing WiX, and all 33 full verification commands pass; evidence is
`out/portable-discovery-full-verification-result-20261002.json`. Exact-SHA hosted
assessment remains pending; alternate-machine qualification is not established
by this local change. Regular hosted workflows use CPU-only builds, so fresh
hosted whole-SDK provisioning and HIP compilation still require separate
qualification before release.

## Round Thirteen: Cross-Shell Environment Restoration

Hosted Windows CI fails for `08519a5` on both runner images with
`Compiler scope leaked its HIP_PATH setting into runtime work.` The failure
reproduces locally under PowerShell 7.6.5: passing a previously absent value to
the .NET setter through PowerShell leaves an empty environment value. Windows
PowerShell 5.1's passing result does not establish PowerShell 7 compatibility.

One shared process-environment helper now explicitly removes absent values and
restores populated or host-supported empty values. HIP compilation, parallel
build scheduling and GUI-smoke cleanup use it. Regression checks cover absent,
empty and populated caller states, nested scopes, action output and exception
preservation, and invalid-name rejection before mutation. These checks pass
under Windows PowerShell and PowerShell 7; the original ROCm and build scheduling
tests also pass under PowerShell 7. All 33 full verification commands pass,
including the native tests, all-page GUI smoke, format matrix, interoperability,
bounded fuzzing and packaging. The result is recorded separately in
`out/cross-shell-full-verification-result-20261002.json`. The next exact-SHA
hosted assessment remains pending. The failed hosted run remains recorded;
the first migration commit is not final acceptance.

Windows CI subsequently passes on both hosted runner images for `8acf383`.
Security and fuzzing are still running at its intermediate snapshot; neither
the first migration commit nor this snapshot is final acceptance.

## Round Fourteen: Benchmark Build-Input Provenance

The benchmark clean-source check omitted SDK pins and build helpers even though
they affect the compiled HIP binary and its configuration. It now includes the
whole-SDK lock and provisioning, compiler and architecture configuration,
process-environment restoration, CMake discovery, version resolution, and
resource-derived build scheduling. Unrelated untracked skills still do not
invalidate native measurements.

Real temporary Git repositories exercise untracked inputs and every newly
covered helper in staged and unstaged states, followed by clean restoration of
fixture contents. The reporting regressions pass under Windows PowerShell 5.1
and PowerShell 7.6.5; targeted hygiene/lint/reporting verification and the changed
function contract gate also pass. Evidence is recorded in
`out/benchmark-build-inputs-verification-result-20261002.json`. This tightens
provenance rather than changing product execution or establishing a speedup.
Fresh hosted whole-SDK provisioning and HIP compilation remain separate release
qualification work, and the corrected scientific timing rounds remain open.

## Round Fifteen: Corrected Whole Mixed-Profile Sampling

The ROCm 10 build completes the 10 GiB RAM-only Mixed level-five sweep across all
seven production block sizes. All 42 pilots and 156 preplanned confirmations
are retained, with alternating lane order and reversed case order. Every
observation passes CRC read-back verification, reports zero payload disk writes,
and retains identical exact archive bytes within its case. Independent Python
recomputation matches every confirmation mean, sample SD and CV reported by the
PowerShell controller. The complete bounded run exits successfully without a
timeout or output-limit violation.

| Block KiB | Confirmations per lane | CPU archive bytes | HIP archive bytes | Paired descriptive quality |
| ---: | ---: | ---: | ---: | --- |
| 256 | 15 | 4,518,088,166 | 4,572,118,255 | Inconclusive |
| 512 | 15 | 4,517,025,225 | 4,567,776,791 | Descriptively stable |
| 1024 | 6 | 4,516,534,055 | 4,546,487,579 | Descriptively stable |
| 2048 | 10 | 4,516,280,994 | 4,535,842,123 | Inconclusive |
| 4096 | 7 | 4,516,153,879 | 4,530,520,631 | Inconclusive |
| 8192 | 15 | 4,516,089,518 | 4,527,857,731 | Inconclusive |
| 16384 | 10 | 4,516,058,195 | 4,526,528,555 | Inconclusive |

At 512 KiB, CPU/HIP mean compress/verify/extract totals are 9.0803/6.0761 seconds
with sample SDs of 0.2587/0.0978 seconds; at 1 MiB they are 9.5176/5.7299 seconds
with sample SDs of 0.3421/0.1845 seconds. These are descriptive observations,
not confidence or significance statements. HIP produces larger archives on
this synthetic profile. The five other paired cases retain their variability
or precision failures; no slow observation is removed. The graph validator
refuses the full sweep with `confirmation measurements remain inconclusive`.

The separate host trace has 298 snapshots, a measured mean interval of 5.67
seconds and maximum gap of 13.97 seconds, despite its nominal one-second pause.
Eighty-seven snapshots report counter errors, with 696 invalid counter entries.
GPU values therefore reflect available engine counters rather than complete
contention evidence. Available RAM stays above 24.98 GB, but paging spikes and
sparse host sampling prevent fine-grained timing attribution. These limits must
remain visible rather than treating counter availability as proof of isolation.

Evidence is retained in `out/rocm10-scientific-mixed-20261002.json`, its complete
`.samples.jsonl` journal, `out/rocm10-scientific-mixed-result-20261002.json`, and
`out/rocm10-scientific-mixed-analysis-20261002.json`. The measurement checkout is
`47423ab`; the binary hash begins `f7f26e3c`. These records do not establish an
SDK-caused improvement: there is no independent matched SDK baseline, and the
binary still lacks a verified build-input manifest. That provenance gap is the
next shared-tooling hardening task. Exact source inspection also confirms that
the current verifier/extractor compares decoded CRCs rather than every byte.
An earlier description of these observations as byte-exact was incorrect;
the observations establish CRC-checked integrity, not bytewise equality. Add
separately identified bytewise reference validation with explicit resource and
timing costs before stronger integrity claims. The benchmark and owned monitor
have exited.

## Round Sixteen: Fresh Hosted HIP Qualification

A separate manually dispatched `rocm-qualification` workflow provisions the
complete pinned SDK on a selected fresh Windows runner, then compiles and links
the application for the repository's complete release target set. It reuses
the existing provisioner, compiler configuration and memory-derived build
scheduling. It requires actual storage admission rather than assuming a fixed
drive or deleting installed runner software to make space. Both existing
GitHub action pins still match their official latest release tags on October 2.

The workflow has read-only repository permission and no publication, driver
installation or deployment step. Its shared provisioning-policy checks cover
both the release action and qualification workflow, including regressions that
reject a substituted provisioner, global environment changes and leaked
compiler-only settings. All 33 full local verification commands pass, with
bounded evidence in `out/rocm-qualification-full-verification-result-20261002.json`.
The retained response tail is truncated intentionally; the command does not hit
its total output limit or deadline. The late CRC-only documentation correction
receives an additional documentation hygiene/lint check. Exact-SHA hosted
qualification passes for `9aa2a6607a51ec549635d9399940927410609be0` on the fresh
VS 2026 runner: [run 37058607674](https://github.com/strmt7/SuperZip/actions/runs/37058607674).
Its complete SDK provisioning and release-target compile/link steps succeed.
Successful compilation alone does not qualify GPU execution on other hardware
or authorize release.

## Round Seventeen: Independent Bytewise Benchmark Validation

The native RAM benchmark now decodes every archived window again after the
ordinary timed compression, CRC verification and extraction phases. It compares
every byte with independently regenerated source, using the existing production
owned decoder and required-HIP policy. Source generation moves unchanged into a
shared testable module. Additional output/reference memory is included in
admission, and the reference buffer is bounded to 64 KiB.

The new `bytewise-regenerated-v1` measurement protocol records the validated
byte count, separate validation cost and complete wall time. Its HIP telemetry
does not inflate the timed operation counters. Resource averages cover the
whole process, including validation; validation between windows can change
cache and thermal conditions. Graph validation prevents combining this protocol
with historical CRC-only evidence and rejects missing pilot/confirmation
coverage. Historical raw observations remain unchanged.

Tests cover all seven profiles, unaligned offsets, reference-buffer tails,
production CPU/HIP owned decoding and unequal byte arrays with the same known
CRC32. Initial formatting, a moved-helper diagnostic reference and a test's
incorrect default GPU policy were caught during verification and corrected.
All 33 selected full verification commands pass, including 574 native tests,
all-profile CPU and actual required-HIP bytewise fixtures, format interoperability,
GUI smoke, packaging and bounded fuzzing. Evidence is retained in
`out/byte-validation-full-verification-result-20261002.json`; its response tail
is bounded, with no command timeout or total output-limit failure. The complete
10 GiB RAM block-size validation sweep also passes: all 14 forced-CPU/required-HIP
cases compare every byte, totaling 140 GiB of validated source across the seven
production block sizes. Their exact archive sizes match the earlier CRC-only
sweep. Timed GPU decode counters remain at 160 chunks per case, excluding the
additional validation decodes. The source-dirty, single-observation sweep is
integrity evidence, not a speed claim; the graph validator correctly refuses it
as insufficient confirmation evidence.

Its separate serial validation costs 24.60–26.49 seconds per observation,
dominating total development workload time. Preserve this baseline and evaluate
bounded worker-parallel reference generation in a separately identified protocol
before longer statistical runs. Raw observations and independent checks remain
in `out/byte-validation-ram-20261002.json`, its complete `.json.samples.jsonl`
journal and `out/byte-validation-ram-analysis-20261002.json`. After the maintainer
dedicated the host, brief checks show approximately 3.25% external CPU activity
and 0.83% external GPU engine activity, with no invalid GPU counters in that
sample. Those checks do not establish isolation throughout a future run.

The earlier hosted security workflow at `9aa2a66` fails its Semgrep job with
[finding 1745](https://github.com/strmt7/SuperZip/security/code-scanning/1745)
in the SDK provisioner's test fixture. The archive is created within that test;
this is not evidence of an exploitable product extractor. The fixture now writes
its independently declared paths/bytes with exclusive creation instead of
calling `extractall()`. Production archive inspection, installation preservation
and the fixture's actual provisioning calls remain exercised. Same-SHA hosted
confirmation now marks that finding fixed; no scanner exclusion is added.
Windows CI and lint pass at `65e40ef`, while the broader hosted security and
fuzzing runs remain pending at the intermediate snapshot. The post-push audit
rejects 214 unapproved open code-scanning alerts. A fixed fixture finding does
not resolve that backlog or establish final security acceptance.
A verified binary-build receipt, broader corpus comparisons and final release
acceptance remain open.

The next protocol, `bytewise-regenerated-v2`, retains the complete byte comparison
but partitions reference generation across the admitted CPU worker budget.
Partitions contain at least 8 MiB, matching the existing parallel CRC policy;
each owns at most 64 KiB scratch. One decoded chunk remains live, and all readers
join before its storage is released, including exceptional exits. Decode and
comparison run consecutively, with separate telemetry from the timed phases.
The reserve covers the maximum 64 worker-local buffers. Raw observations record
`validation_worker_limit`, and graph validation rejects missing/changed budgets
and mixed v1/v2 protocols. A reporting fixture initially omitted the new worker
metadata and was corrected. The follow-up verifier passes all seven selected
commands, including the HIP Release build, 576 native tests, reporting, graph
contracts and function documentation. Automatic full escalation also completes
format interoperability, GUI smoke, bounded fuzzing and packaging; its original
nonzero wrapper status remains retained rather than relabeled as a clean run.
All eight main GUI screenshots were reviewed, and PowerShell 7 reporting passes.
Evidence remains in `out/parallel-validation-followup-result-20261002.json` and
`out/parallel-validation-verification-result-20261002.json`. New RAM measurements
are pending; this refinement is not a measured product codec speedup.

## Round Eighteen: Complete Observation Lifetimes And Artifact Guards

The clean `0025ce9` Mixed level-five diagnostic completed 42 pilots and 23
confirmations before an administrative stop. Its predeclared maximum of 15
confirmations capped five of seven plans; the pilot requested up to 34. Every
completed observation remains in the original journal, totaling 650 GiB of
independently byte-checked source with exact archive sizes matching the serial
validation baseline. No completed sample was discarded or promoted into a
replacement experiment. This incomplete diagnostic is ineligible for a final
comparison or publication graph. Validation costs were 3.95–5.79 seconds,
compared with 24.60–26.49 seconds in the separate serial diagnostic; these are
development overhead observations, not a controlled product codec speedup.
The abort receipt and raw evidence remain in ignored
`out/parallel-validation-scientific-mixed-abort-receipt-20261002.json` and
`out/parallel-validation-scientific-mixed-20261002.json.samples.jsonl`.

The controller already froze starting commit and CLI hashes and checked them
before final JSON. The next refinement extends those checks around each native
observation, includes relevant-source dirty state and app-local DLL hashes, and
enforces the final check without JSON output. Raw observations carry the frozen
identity and complete controller wall duration. Graph validation keeps this
evidence separate from historical endpoint-only records. This is not a verified
build-input receipt: uncommitted content hashes, externally loaded runtimes and
drivers remain outside its scope.

The default confirmation ceiling rises to 64. Frozen plans are checked against
remaining time using complete pilot observation lifetimes, including independent
validation and controller costs, plus configured pauses. An estimated overrun
retains the pilots and refuses confirmation without changing counts. Observed
maxima are estimates, not statistical upper bounds; existing deadlines still
apply. A replacement study must have a new predeclared policy and new paths.

All seven hosted workflows pass for `0025ce9`; its intermediate snapshot does
not establish final acceptance. The post-push audit still rejects 213 unapproved
open alerts. Broader source review, a verified build-input receipt, independent
corpus comparisons and release gates remain open.

Corrected full verification passes all 31 selected local commands, including
576 native tests, the 36-format matrix, interoperability, fresh GUI smoke and
screenshot review, bounded fuzzing and packaging. The initial overlong Python
contract comment and unused test-mock argument were fixed without weakening
lint; both failed results remain retained. The successful bounded result is
`out/observation-guards-full-corrected-result-20261003.json`; its response tail
is truncated, with no timeout or total output-limit failure. A subsequent
focused refinement journals returned native statistics before the
post-observation artifact check, preserving raw evidence if provenance fails
without converting it into a valid observation.

## Bounds Review Progress

The six baseline pointer-bounds results contain 21 reported flow variants.
Seven relevant generated dependency files were compared byte-for-byte with the
original source archive; all match, and the archive hash matches recorded
provenance. The separate documented downstream patches remain outside this set.

| Reported variants | Inspected boundary |
| --- | --- |
| Ten xxHash reads | COVER/FASTCOVER builders retain `0 <= tail <= capacity`; their initial content span ends at allocation end. Shrink iterations bound their span by the successfully finalized dictionary size. xxHash consumes 32-byte stripes and only reads 8/4/1-byte tails when that many bytes remain. |
| Eight header writes | The ordinary frame path rejects capacity below six bytes; raw blocks check payload plus the three-byte header; split compressed blocks check the three-byte header before entropy encoding. Error returns propagate before advancing output. |
| Three RLE writes | The target-size and both split routes reach the same helper, whose explicit capacity-below-four rejection precedes the header and `op[3]` write. |

The current product runtime resolves compression/decompression ABI functions,
not dictionary-training exports. That observation limits product reachability;
it does not establish safety of every exported dependency API. The added guarded
production-DLL regressions are finite dynamic evidence, not complete proof of
all internal branches or a substitute for static triage. These six alerts remain
open; no finding was dismissed or scanner coverage narrowed by this review.

## Round Nineteen: Completed Measurements And Report Export Repair

The clean `d2a44ca` Mixed level-five study completed all 42 pilots and 134
frozen confirmations across both lanes and all seven block sizes. Counts were
10/11/9/6/6/15/10 per lane, respectively; none reached the 64-observation cap.
Every observation compared all 10,737,418,240 bytes against the independently
regenerated reference, for 1,889,785,610,240 validated bytes in total. All sizes
match the preceding byte-validation baseline, with RAM-only execution and zero
workload disk writes. No completed observation was discarded.

Final JSON export failed with `Cannot bind argument to parameter 'BinarySha256'
because it is an empty string.` A stale variable remained in the pilot export
branch. The original controller exit remains a failure. A shared study exporter
now supplies the same frozen identity to both stages; regressions exercise
pilot-based and fixed-count exports and verify the production caller uses it.
The recovered report labels its origin, original failed exit, journal hash and
missing CPU model. PowerShell 7 recovery preserves the journal's numeric values;
independent Python recalculation matches every retained phase timing, exact size,
identity, native transfer/allocation counter and all descriptive statistics.
It does not constitute successful completion of the original controller.

| Block KiB | Confirmations per lane | CPU mean phase-total seconds | HIP mean phase-total seconds | CPU archive bytes | HIP archive bytes | Comparison status |
| --- | --- | --- | --- | --- | --- | --- |
| 256 | 10 | 8.8433 | 6.0456 | 4,518,088,166 | 4,572,118,255 | Descriptive thresholds met |
| 512 | 11 | 8.4452 | 5.9513 | 4,517,025,225 | 4,567,776,791 | Descriptive thresholds met |
| 1024 | 9 | 8.7336 | 5.5565 | 4,516,534,055 | 4,546,487,579 | Descriptive thresholds met |
| 2048 | 6 | 9.0832 | 5.2852 | 4,516,280,994 | 4,535,842,123 | Descriptive thresholds met |
| 4096 | 6 | 9.5151 | 5.1552 | 4,516,153,879 | 4,530,520,631 | Inconclusive |
| 8192 | 15 | 9.6103 | 5.0376 | 4,516,089,518 | 4,527,857,731 | Inconclusive |
| 16384 | 10 | 9.7605 | 5.0774 | 4,516,058,195 | 4,526,528,555 | Inconclusive |

These are generated Mixed data on one RX 9070 XT with ROCm runtime 10.0.3679.0,
32 admitted workers and the recorded frozen pipeline geometry. Sizes differ
between CPU and HIP; this is not an equal-output-size comparison. Phase total
sums compression, verification and extraction, excluding independent bytewise
validation, admission and controller overhead. Compression includes source
generation inside owned encode tasks. Separate generation/encoding worker
intervals overlap and cannot be subtracted from wall time. No confidence,
significance, SDK-caused speedup, filesystem throughput or other-hardware claim
follows from this study.

GPU verification/extraction variability exceeded the declared thresholds at
4096 KiB, and extraction variability exceeded them at 8192/16384 KiB. The graph
validator refuses the complete record with `confirmation measurements remain
inconclusive`; no successful subset was selected for publication. The separate
host monitor retained 486 readings, including 498 invalid counters across 64
readings and 62 collection errors. GPU values are unavailable in two readings.
Its actual cadence was 3.58–7.36 seconds. Minimum free RAM was 24,001,335,296
bytes; page reads peaked at 1579.36 per second and disk queue at 0.7834.
Those limits prevent a claim of perfect host isolation; their causes are not
established, and affected timing samples remain retained.

Ignored evidence: `out/observation-guards-scientific-mixed-20261003.json.samples.jsonl`,
`out/observation-guards-scientific-mixed-recovered-20261003.json`,
`out/observation-guards-scientific-independent-audit-20261003.json`, the original
bounded failure receipt and complete host-context journal. All seven selected
hosted workflows now pass at `d2a44ca`; the 213-alert post-push audit remains
failed. This is an intermediate checkpoint, not release acceptance.

## Round Twenty: Portable Native Build Defaults

Ordinary PowerShell builds, standalone HIP object compilation and fresh direct
CMake configuration now select the existing six-target `release` preset rather
than `gfx1201`. Explicit target overrides remain available. Direct CMake calls
the same bounded PowerShell resolver at configure time and records concrete
targets in runtime metadata. Invalid selections fail before HIP compilation.
Existing direct CMake cache choices are preserved; selecting `release` explicitly
changes an older single-target cache. No installed SDK component is modified.

The preset remains `gfx1100,gfx1101,gfx1102,gfx1151,gfx1200,gfx1201`. It does not
claim every architecture in AMD's ROCm 10 Windows matrix. The separate vendor
matrix review identified five additional targets requiring application compile
and execution qualification. Compiler acceptance alone cannot establish runtime
correctness on unavailable GPUs.

Production resolver tests cover the release preset and explicit lists through
actual CMake invocation, including duplicate, native and shell-separator
rejection. Default regressions inspect the wrapper parameter definitions.
The initial full attempt rejected three overlong CMake comments; after those
were shortened, all 33 full-profile commands passed. The HIP-enabled runtime
manifest records all six targets. All 576 native tests, five CTests, format and
independent interoperability smoke, bounded Docker sanitizer/fuzz smoke, GUI
controls, brand checks and portable packaging passed. All eight fresh GUI page
screenshots were reviewed after its owned process exited. Existing About-page
speed wording remains an audit item, not a proven claim.

Ignored receipts: `out/portable-default-full-result-20261003.json` retains the
lint failure, and `out/portable-default-full-corrected-result-20261003.json`
retains the successful terminal result. Its 128,000-byte response tail is
truncated; it is not a complete raw verification log. The separate current
CTest log remains under `build/Testing/Temporary/LastTest.log`. Successful
build-input receipts, larger-matrix qualification, corpus comparisons, security
backlog and release acceptance remain open.

## Round Twenty-One: Successful Native Invocation Identity

The build wrapper now serializes the CMake tree with an actual Windows sharing
lock and records incomplete, configure-only, failed and successful attempts.
A shared content manifest includes native source, tests, CMake, pinned upstream
inputs, compiled resources and build/header-generation tools, including staged,
unstaged and nonignored added contents. Atomic successful receipts bind this
manifest to configured options, a precisely limited observed toolchain scope,
GUI/CLI outputs, runtime manifest and app-local DLLs. Prior evidence is retained.
The first receipt and changed configuration/toolchain/output bytes require a
fresh build; ordinary source edits retain incremental compilation.

RAM observations now require current receipt/input hashes before and after each
operation and at completion. Final export embeds the portable receipt once;
the graph recomputes its shared schema/digests and checks CLI/DLL and HIP scope
under a distinct observation policy. Historical evidence remains historical.
These guards do not authenticate every compiler input, loaded GPU drivers,
ignored/external substitutions or transient mutations restored between checks.
They are not cryptographic build attestation or measured codec speedups.

Real-file regressions cover failed/incomplete attempts, stale source and output
bytes, configuration/compiler mutations, fresh versus incremental decisions,
history collisions and required-HIP fallback. The Windows lock test uses an
actual exclusive handle. Graph fixtures are explicit test data. A focused
PowerShell test exposed function-only loading's missing script-root scope; a
shared explicit tool binding fixes it without bypassing receipt validation.
All 34 frozen full-profile commands pass, including the fresh six-target HIP
build, 576 native tests, five CTests, format/interoperability smoke, bounded
Docker sanitizer/fuzz smoke, GUI controls, brand checks and portable packaging.
All eight fresh main-page screenshots were reviewed after the owned GUI exited;
existing About-page speed wording remains an open audit item. The actual HIP
receipt validates 454 native input files and all five required output artifacts.

Ignored evidence: `out/native-receipt-full-result-20261003.json` retains exit zero
without timeout or output-limit failure, plus the successful 34-command terminal
summary. Its response tail is truncated, not a complete raw verification log.
The CTest log remains under `build/Testing/Temporary/LastTest.log`. Dedicated-host
receipt-backed RAM export smoke, exact-SHA hosted assessment, security backlog,
larger GPU qualification and release acceptance remain pending at this checkpoint.

The clean `8967c09` RAM export smoke completes all fourteen fixed-count CPU/HIP
observations across seven blocks, byte-validating 140 GiB with zero payload disk
writes and unchanged archive sizes. Independent hashes of all 454 inputs and
five outputs match the embedded receipt; exported counters/timings exactly match
the journal. Shared graph identity validation passes, while publication correctly
refuses the insufficient single-observation confirmation sample. Host context
retains 47 readings, 72 invalid counters and nine collection errors; timing is
inconclusive. No observations are discarded.

Hosted Windows CI then fails both CPU-only lanes during receipt finalization:
CMake emitted the runtime dependency manifest only for HIP configurations.
Local fixtures had created it for both, so they missed that production gap.
The corrected shared CMake writer emits truthful manifests for every Windows
configuration, with HIP disabled and no driver prerequisites in CPU validation.
Direct configure-time regressions cover both branches, and hosted CI validates
actual successful CPU build receipts. The corrected batch passes all 32 commands
selected by its full verification plan, including 576 native tests, six CTests,
format/interoperability smoke, bounded sanitizer/fuzz smoke and packaging.
All eight fresh main-page screenshots were reviewed after the owned GUI exited.
The current HIP receipt validates 457 native inputs and five output artifacts.
Ignored evidence is retained in
`out/native-receipt-cpu-repair-full-result-20261003.json`; its response tail is
truncated, without timeout or output-limit failure. Corrected hosted verification
remains pending; local HIP success is not CPU-only hosted acceptance.

This follow-up also widens both test-oracle worker products before multiplication
for owned alert 1741 and names the ROCm qualification job for alert 1746. These
are bounded test arithmetic and workflow quality repairs, not proof of an
exploitable product defect. The 213-alert audit still fails; no suppression or
dismissal is used. Individual hosted fix confirmation remains pending.

At `f2a3455`, hosted Windows CI passes on both Windows 2022 and Windows
2025/VS 2026, including actual CPU-only receipt validation. Lint, the dispatched
benchmark graph check, Scorecard and the Greenbone workflow also pass. Security
and fuzzing remain running at this intermediate observation. The fresh audit
still rejects 213 unapproved alerts; workflow success is not final security
acceptance or proof of live scanner coverage.

## Round Twenty-Two: Focused Production Cycles

The maintainer explicitly requests batched component-specific verification.
The runner's first repair stops immediately on failure, preserving the original
error instead of launching unrelated full-profile commands. Six controlled
real-runner cases cover early/later/manual failures, success, explicit full
selection and the legacy no-escalation switch. Runner/selector contracts,
changed-file lint/hygiene and function contracts form this focused batch;
the blanket full profile is explicitly deferred by that maintainer direction.
Product binaries are unchanged by this runner batch, and no benchmark,
product rebuild, GUI smoke or package rebuild is repeated for it.

The companion routing batch replaces blanket tooling and path-count expansion
with component/dependency contracts. Unknown paths fail planning for explicit
classification. Receipt producers require builds, while receipt fixture edits
select regressions without unrelated codec tests. Compiler helpers are tested
directly and are not repeated when the native test driver already invokes them.
MCP and agent-tool changes select their own containment/context contracts.

The new hosted component lane projects offline contracts from that same planner
over the push/PR range. Native Windows CI is restricted to native inputs and its
own workflow; actual build receipt validation remains mandatory. Tool regression
steps move to the component lane rather than disappearing. GitHub path-filter
limits and future required-check configuration still require explicit review.
The first integrated local run passes all ten selected commands without product
rebuild, GUI/package smoke or timed workloads. CI projection regressions also
pass under Windows PowerShell 5.1 and PowerShell 7. An empty-array fixture parsing
issue was corrected; the actual lane successfully executes the runner contracts
as its sole selected command. The final frozen source passes all ten selected
commands; the complete ignored log is retained at
`out/component-routing-frozen-verification-20261003.log`. All seven workflows
for the preceding `f2a3455` checkpoint pass, and GitHub confirms alerts 1741 and
1746 fixed. Hosted assessment of the new routing lane and a fresh remaining-alert
count are still pending at this source checkpoint.

## Round Twenty-Three: Remaining Unconditional Hosted Work

The `7b729ca` component lane passes on GitHub, including the real planner and
Windows command execution. Its fresh post-push audit rejects 211 open alerts;
the two-alert reduction is confirmed, not suppressed. Native CI and security
analysis remain pending at this intermediate observation.

That push also reveals unconditional fuzzing and offline Greenbone runs even
though neither mechanism changed. Push/PR filters now admit the actual fuzz
build source families, vendored codec inputs, harnesses, container recipe and
own workflow; the offline audit admits its scripts/configuration, pinned
requirements and offline/live workflow contracts. Weekly/manual fuzzing, both
sanitizers and their 300/600-second budgets remain intact; stateful live scanning
is unchanged. Planner/trigger parity regressions cover admitted and excluded
mechanisms and every literal source dependency declared in the fuzz build.
The component lane also observes changed filter contracts. This batch changes
no product code and requires no local native build, GUI or package smoke.
The frozen source passes all ten selected local commands, including filter
ownership/parity and broker contracts. Complete bounded evidence is retained in
`out/workflow-input-routing-verification-20261003.json`, without timeout or output
truncation. A console-only Unicode display failure after the successful verifier
was corrected by reading that retained JSON; the checks were not repeated.
Hosted qualification of the new filters remains pending.

## Round Twenty-Four: Prefix Decode Metadata Allocation

The host planner no longer grows its vector once per coded block. A bounded
prepass counts static/adaptive/Huffman segments and reserves once, preserving
all plan fields. [Component evidence](gpu-prefix-plan-validation.md) records
allocation traffic, every raw timing sample and the independent noisy-case
confirmation. Generated-descriptor MSVC microtimings are explicitly separate
from in-app HIP and archive throughput; no compression-ratio or kernel-speed
claim follows from them. The full RAM-only archive sweep remains a release
claim gate, not a repeated unrelated workload in this host-metadata cycle.

The final HIP Release build, all 14 selected prefix/Huffman tests and changed-code
quality gates pass under the maintainer's focused-test direction. The successful
build receipt also validates the final native input/output contents. Both frozen
host-study series pass without dropping observations. No unchanged GUI, package
or compatibility-format smoke is repeated. Exact-commit hosted qualification
remains pending.
