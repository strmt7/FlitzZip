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
