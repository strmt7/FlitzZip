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
