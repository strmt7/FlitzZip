# Household Power CPU/HIP Diagnostic

Three authenticated empirical excerpts completed the scientific RAM controller
on October 3, 2026. CPU archives were smaller on every excerpt. All paired
timing comparisons remain inconclusive under the declared policy. These results
identify investigation targets; they establish no speedup or general superiority.

## Source And Protocol

The [source, attribution and exact selection method](corpora.md) describe the
UCI household-power measurements by Georges Hebrail and Alice Berard, licensed
under CC BY 4.0. These are three disjoint, literal complete-row excerpts from
one numeric time series, with original missing-value markers and line endings.
The first excerpt includes the header. They represent neither independent
households nor mixed documents, binaries, images or many-file folders.

The clean source commit was
[`5c1101f`](https://github.com/strmt7/SuperZip/commit/5c1101ff1a6667e751b4479f7eb16cc2dc48b129).
The unchanged HIP Release build had native input identity
`c0488c38bbd7c9481078bd16f3be66505fe8208342ff0a9d8845d949671395dc`
and successful receipt identity
`ac603c4e3fb1025a7c2c41f9f4446ee2db1a1accc40305708721426ebbe09338`.
Observed hardware was an AMD Ryzen 9 9950X and AMD Radeon RX 9070 XT, with
HIP runtime `10.0.3679.0`. Other hardware was not exercised by this study.

Every case used native `.suzip`, compression level 5 and an 8192 KiB block.
Forced CPU and required HIP received identical exact source bytes. The default
three-observation pilot froze the paired confirmation count at 64 per lane.
Uncapped requests were 248, 257 and 264 for first, middle and last respectively.
All pilots and confirmations remain retained, with zero discarded samples or
implicit warm-up. Lane order alternated. The duration floor remained 30 seconds
of combined measured compression, verification and extraction per lane;
maximum phase/total CV remained 5%, with the 2% independent-mean RSE diagnostic.
The [sampling policy](../performance-block-size-validation.md#scientific-sample-planning)
states its assumptions and operational limits.

Each observation started a new CLI process and authenticated its resident
source before timing. Product phase time includes HIP readiness and allocation
work performed in the encode path. File preload, source authentication,
independent byte validation and controller guards remain outside phase times.
These measurements therefore do not establish the cost of an already warmed
HIP application context or filesystem archive operations.

All 18 pilots and 384 confirmations completed with independent bytewise source
validation. Every observation reported `memory_only=true` and
`disk_write_bytes=0`; required-HIP observations reported actual encode/decode
chunks and kernel launches. Those fields concern archive/workload writes;
source acquisition and small diagnostic metadata have separately recorded writes.

## Retained Observations

Archive bytes include the native benchmark's modeled archive overhead. Ratio
means archive bytes divided by exact input bytes; lower is smaller. The reported
size was identical across every pilot and confirmation within each lane/case.
Time means cover all 64 confirmations and exclude pilots. Millisecond values
are rounded for display; raw reports retain the original precision.

| Excerpt | Input bytes | Lane | Archive bytes | Ratio | Mean combined time (ms) | Quality |
| --- | ---: | --- | ---: | ---: | ---: | --- |
| First | 20,971,465 | CPU | 3,172,667 | 0.151285 | 118.300 | Inconclusive |
| First | 20,971,465 | HIP | 5,210,241 | 0.248444 | 461.685 | Inconclusive |
| Middle | 20,971,454 | CPU | 3,142,476 | 0.149845 | 114.067 | Inconclusive |
| Middle | 20,971,454 | HIP | 5,193,569 | 0.247649 | 466.527 | Inconclusive |
| Last | 20,971,499 | CPU | 3,229,062 | 0.153974 | 114.698 | Inconclusive |
| Last | 20,971,499 | HIP | 5,342,289 | 0.254740 | 469.731 | Descriptively stable |

CPU combined measured time was 7.571, 7.300 and 7.341 seconds, below the
unchanged 30-second floor. First/middle CPU maximum phase CV was 5.55%/7.36%,
also exceeding the declared limit. Last CPU maximum CV was 3.29% but its duration
still failed. HIP combined measured time was 29.548, 29.858 and 30.063 seconds;
maximum CV was 2.18%, 2.22% and 2.21%. Only last HIP met every descriptive lane
requirement. A passing HIP lane does not qualify its failed paired comparison.
No threshold was relaxed, no slow sample removed, and no capped experiment
extended in response to its observed results.

## Investigation Priorities

The recorded HIP input-allocation stage averaged 230.861, 230.260 and
230.316 milliseconds of worker time. Mean encode phase time was 439.473,
443.923 and 447.104 milliseconds. Stage counters sum worker time and must not
be interpreted as device utilization or a parallel critical-path decomposition.

Exact source review locates this region in `encode_chunk_hip_impl`: it includes
`HipDeviceMemoryReservation` followed by `HipDeviceBuffer` construction. The
reservation queries the selected device and free VRAM before aggregate admission;
the buffer then calls `hipMalloc`. These combined counters do not identify which
API, driver initialization or allocation mechanism causes the cost. The
instrumented follow-up below separates those API intervals before choosing
bounded workspace reuse or another allocator;
preserve aggregate admission, ownership and failure cleanup. Earlier rejected
[allocation experiments](../modernization-audit-2026-10-02.md#round-one-input-allocation-experiment)
remain relevant evidence.

All three HIP excerpts used dictionary blocks. Their larger archives make
dictionary match coverage and candidate selection a ratio priority. Evaluate
those production mechanisms with independent readers and exact bytes, rather
than substituting a CPU codec inside a required-HIP lane. More workload classes,
all seven blocks, broader effort levels and eligible external comparators remain
outstanding. Small numeric excerpts do not replace those qualifications.

## Instrumented API Follow-Up

One normal-priority production invocation on source commit
`16d06ec04799110daf7e4d5dda13df08fbf56248` traced the same first excerpt,
level 5 and 8192 KiB blocks. The unchanged HIP-enabled binary and its build
receipt passed before/after identity checks. The invocation completed bytewise
validation with required HIP, `memory_only=true` and `disk_write_bytes=0`.
Child-scoped `AMD_LOG_LEVEL=4`, `AMD_LOG_MASK=1` and `AMD_LOG_LEVEL_FILE`
produced a completed 251,663-byte log. Logging perturbs execution: these
intervals are diagnostic observations, not new benchmark samples or a speedup.

The original strict analyzer rejected this valid capture because
`hipGetLastError` entries lacked logged returns. AMD's
[status-query implementation](https://github.com/ROCm/clr/blob/develop/hipamd/src/hip_error.cpp)
initializes API logging but returns directly for `hipGetLastError` and
`hipExtGetLastError`; `hipPeekAtLastError` uses the normal logged return.
This source pattern explains the observed layout but does not prove that the
develop branch is identical to the installed driver binary.

The analyzer now offers `--allow-entry-only-status-queries`. Its strict default
is unchanged. The opt-in retains only unpaired, empty-argument entries for
those two APIs, reporting unknown duration, status and unique-call count.
Repeated entry-shaped records are not arbitrarily paired. Missing allocation,
transfer, other status-query or internal launch-configuration returns still
fail. Paired queries retain their actual timing and success/failure evidence.
Internal `__hip` configuration APIs now receive the same strict parsing.

All 1,005 entry records are accounted for: 905 complete calls and 100 untimed
`hipGetLastError` records, with 905 logged returns. No record was silently
discarded. Full-trace results include:

| API | Complete calls | Total host interval, ms | Maximum host interval, ms |
| --- | ---: | ---: | ---: |
| `hipMemGetInfo` | 17 | 245.904 | 245.747 |
| `hipEventSynchronize` | 23 | 138.217 | 23.341 |
| `hipMemcpyWithStream` | 36 | 23.092 | 8.398 |
| `hipMalloc` | 52 | 6.726 | 1.295 |
| `hipHostMalloc` | 2 | 2.768 | 2.755 |

The first `hipMemGetInfo` interval was 245.747 ms; its other 16 intervals
totalled 0.157 ms. The four device allocation calls requesting exactly
20,971,465 bytes totalled 0.977 ms. These observations locate the large
input-admission cost at the first memory query, rather than at the logged
device allocation calls. First-use runtime initialization is a hypothesis to
check; moving a query before the encode timer would not remove end-to-end cost.
Do not remove VRAM admission or reinstate an allocator experiment on this
evidence. Synchronization intervals include waiting and are not independent
device execution measurements. API intervals may nest or overlap across
threads; their totals are not additive wall-clock fractions.

Fourteen focused parser contracts cover opt-in accounting, strict rejection,
internal APIs, privacy, time windows and the actual CLI. The canonical verifier
selects these offline contracts for analyzer changes. Their former lint-side
invocation was removed to avoid running the same suite twice in one plan.
The retained capture and sanitized analysis are local evidence under
`out/household-hip-api-trace-capture-20261003.json` and
`out/household-hip-api-trace-analysis-20261003.json`. Reanalyze a completed
trace without launching another workload:

```powershell
py -3 tools/analyze_hip_trace.py <completed-trace-path> --allow-entry-only-status-queries
```

## Rejected Dictionary Sort Candidates

The next batch evaluated two changes to the production dictionary key sort.
Keys encode segment, exact four-byte prefix and source position. ROCm Core SDK
10 includes rocPRIM 4.6.0; its installed headers and
[official sort documentation](https://rocm.docs.amd.com/projects/rocPRIM/en/latest/device_ops/sort.html)
describe stable sorting and configurable bit ranges. The SDK distribution was
kept intact.

The first candidate sorted bits 16 through 63, intending to preserve source
order within equal prefixes through stability. It reproducibly failed
`dictionary_dispatch_bound_stage_timing`: an independently checked match did
not equal the original substring. The preserved full-key control passed that
same case. The candidate passed the maximum-prefix/sentinel fixture, which
demonstrates why a uniform-value boundary test cannot replace mixed-data
validation. It was rejected before timing. The specific cause of the partial
range failure is unresolved; these observations do not establish an upstream
rocPRIM defect.

The second candidate retained all source-position bits and sorted bits 0
through 54. This includes every valid key bit plus a sentinel-discrimination
bit. It passed all 48 selected native cases, including independent readers,
exhaustive match/reference packing, concurrency and archive publication.
The unchanged source-controlled full-key binary and app-local dependencies
were copied and hash-verified before editing. No control binary was rebuilt
from a guessed historical dependency state.

The corrected RAM-only comparison used the authenticated first excerpt,
level 5, 8192 KiB blocks and identical geometry: 32 requested workers, 32
encode/decode codec workers, encode depth 32 and decode depth 4. Fresh native
processes ran at normal priority, alternating control/candidate order each
round, with bytewise validation and CPU/GPU resource samples. Three pilots per
variant froze 66 confirmations per variant. A predeclared ceiling of 128
allowed the 30-second combined-time floor to fit these short invocations;
the prior corpus studies and the production controller's defaults were not
retrospectively changed. All six pilots and 132 confirmations were retained,
with no warm-up removal, trimming or post hoc extension. Both confirmation
lanes met the descriptive 30-second, 5% CV and 2% RSE criteria.

| Confirmation metric | Full-key control | Upper-bit candidate |
| --- | ---: | ---: |
| Mean encode time, ms | 435.699 | 435.660 |
| Mean combined phase time, ms | 457.856 | 457.743 |
| Combined sample SD, ms | 5.568 | 5.854 |
| Combined measured time, s | 30.219 | 30.211 |
| Complete archive bytes | 5,210,241 | 5,210,241 |
| Recorded device allocation requests, bytes | 693,577,943 | 693,572,823 |

The mean paired candidate-minus-control difference was -0.114 ms, an observed
0.025% reduction. The paired difference sample SD was 8.583 ms; its mean
standard error under an independent-observation assumption was 1.057 ms.
The difference is too small to support a speed claim. Descriptively stable
lanes do not establish a beneficial comparison or eliminate serial dependence
and host effects. The 5,120-byte reduction in cumulative allocation requests
is not a measured peak-VRAM saving and does not justify extra production
complexity. This candidate was also rejected; production sorting remains
the original full 64-bit range.

The first comparison-harness attempt failed after journalling one native
observation because a Boolean parameter was invoked without its value. Its
failed exit and raw journal remain separate. The corrected study uses new
destinations and discloses that setup failure; no timing-driven exclusion
was applied to its observations.

Local evidence is preserved in:

- `out/dictionary-radix-component-verification-20261003.json` and
  `out/dictionary-radix-stage-failure-reproduction-20261003.json` for the rejected
  position-bit candidate.
- `out/dictionary-radix-control-20261003/identity.json` for the verified control.
- `out/dictionary-radix-upper-bits-verification-20261003.json` for candidate
  correctness, and `out/dictionary-radix-upper-bit-candidate-20261003.hip.cpp`
  for its retained source.
- `out/dictionary-radix-paired-first-excerpt-corrected-20261003.json`, its
  `.samples.jsonl` and `.plan.json` for the completed comparison.
- `out/dictionary-radix-independent-inspection-20261003.json` for independent
  raw/report, byte, geometry, HIP and statistical checks of all 138 observations.

The lasting changes are a maximum-prefix/partial-segment regression and
component routing for future dictionary codec work. That production path selects
40 reviewed cases; a changed matcher test file selects all its registrations,
yielding 48 unique cases with direct consumers in this batch. No GUI, unrelated
format matrix or full product suite is selected merely for these changes.

## Local Evidence And Reproduction

The complete schema-four reports and corresponding `.samples.jsonl` journals
remain in ignored `out/benchmarks/`:

- `household-first-l5-b8192-repaired-20261003.json`
- `household-middle-l5-b8192-20261003.json`
- `household-last-l5-b8192-20261003.json`

Independent standard-library inspection checked all 402 native observations
against raw journals, source digests, counts, modeled archive bytes and actual
HIP telemetry, then recomputed unfiltered mean, median, sample SD, CV and RSE.
Its local summary is `out/empirical-household-three-study-inspection-20261003.json`.
The bounded runner shortened human-readable console output; complete structured
reports and journals were inspected independently. Missing local artifacts on
another checkout require reproduction, not an assumption that they exist.

After the documented acquisition, select each filename separately and preserve
new output destinations:

```powershell
tools/bench.ps1 -CorpusManifest out/benchmarks/corpora/household-power-20261003/manifest.json -CorpusRoot out/benchmarks/corpora/household-power-20261003 -CorpusFile corpus-first.txt -CompressionLevel 5 -BlockSizeKiB 8192 -JsonOutput out/benchmarks/household-first-new-study.json
```

Repeat with `corpus-middle.txt` or `corpus-last.txt` and a distinct JSON name.
Use the repository's admitted, bounded command runner and normal-priority native
timing producers. Initial host context showed 2% CPU, approximately 32 GiB
available RAM and no disk queue; a later sample showed 3% CPU and no disk queue.
GPU engine counters and per-observation CPU/GPU counters were retained locally.
These samples do not prove continuous absence of contention or available memory
bandwidth. No benchmark process remained after all three studies completed.
Generated-profile publication still refuses corpus schema 4; this diagnostic
does not change that boundary or authorize release/performance marketing.
