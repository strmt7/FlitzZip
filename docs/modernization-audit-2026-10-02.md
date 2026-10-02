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
Exact-SHA hosted assessment for this new round remains pending.

The broader modernization now explicitly includes review of previous special
cases for consolidation into shared contracts and ownership/resource policies.
Format semantics, required-HIP boundaries and measured correctness remain the
criteria for retaining necessary exceptions.

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
