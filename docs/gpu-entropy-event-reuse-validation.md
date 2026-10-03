# Operation-Owned HIP Entropy Timing Events

## Change And Ownership

Static-prefix and adaptive/Huffman encoding each retain one RAII timing-event
pair across their length and packing passes. The length pass synchronizes its
stop event and collects elapsed time before the packing pass borrows that pair.
Each invocation owns its pair; concurrent chunks share no event handles. Early
returns and exceptions still release successfully created handles. Empty length
plans create no events.

This removes the second pair only when packing follows length computation.
Codec kernels, archive bytes, winner selection, memory admission, device-buffer
allocations and transfer directions are unchanged. Device event ownership is
explicit in the helper contracts; there is no persistent cache or pool setting.

## Focused Correctness

The HIP Release build and 17 selected native cases pass on ROCm Core SDK 10:
static reference packing, per-block winners, adaptive/Huffman selection,
compression levels, segment boundaries, corruption rejection, required-HIP
archive round trips, entropy/dictionary integration and GPU identity. HIP was
compiled and available before and after this cohort. Changed-source lint,
function contracts and repository security-policy checks also pass. Unrelated
GUI, installer and compatibility-format tests were not repeated.

An initial compile failed because the adaptive dispatch helper still accepted a
mutable pair reference. Its const borrowed reference was corrected before the
successful build and tests. The fallback compiler's separate HIP/MSVC header
diagnostic did not justify modifying the SDK or installed application packages.
The successful native receipt is
`2c595b0a118e69fb3c7f3c897ae6748c99e1079748a1448bf9f5bbaf99926f58`,
with native input digest
`58d3c714b940023701f94a9c147bea1b9c92585d25f23a64abbfeb94b41577f1`.

## Whole-Operation Diagnostic

The clean baseline is `aff76430371f0ae472da869cf8120aab019df7d1`.
Baseline and candidate each use three prospectively fixed observations of a
10 GiB RAM-only Mixed workload, level 5, 8192 KiB blocks, 32 workers, encode
depth 32 and decode depth 4. Both periods record host CPU, RAM, paging, storage
and GPU context. All six observations remain in the evidence; no outliers are
removed or selectively repeated.

| Metric | Baseline observations | Candidate observations | Baseline/Candidate mean |
| --- | --- | --- | --- |
| Compression seconds | 3.06517, 3.11191, 3.09971 | 3.18255, 3.10343, 3.14976 | 3.09226 / 3.14525 |
| Verification seconds | 0.757935, 0.747720, 0.787934 | 0.782299, 0.759647, 0.719814 | 0.764530 / 0.753920 |
| Extraction seconds | 1.01352, 1.10151, 1.14940 | 1.09045, 1.14178, 1.05968 | 1.08814 / 1.09730 |
| Summed GPU kernel milliseconds | 4007.73, 4841.75, 4037.04 | 4808.34, 4243.61, 3885.51 | 4295.51 / 4312.49 |

Compression sample coefficients of variation are 0.784% and 1.264%; the
candidate's observed mean is approximately 1.7% higher. Separate build periods,
three observations and concurrent worker timing do not establish a causal
speed improvement or regression. Timing remains inconclusive. The change is
retained for its verified reduction in operation-owned event lifecycles,
without an archive-throughput claim.

All 60 GiB are bytewise validated. Each observation has 4,527,857,731 archive
bytes, 400 recorded product kernel launches, 19,930,628,816 H2D bytes and
14,484,276,988 D2H bytes. Geometry, block-kind counts and reported device/pinned
allocation bytes match. Every observation reports `memory_only=true` and
`disk_write_bytes=0`; collected GPU elapsed time is finite and positive.

Ignored raw journals, host context and comparison are retained under
`out/entropy-event-{baseline,candidate}-mixed-20261003.json`, their
`.samples.jsonl` companions and `out/entropy-event-comparison-20261003.json`.

## Instrumented API Evidence

One bounded whole-process trace per variant records the same workload,
including independent byte-validation work outside product phase counters.
Both complete with successful byte validation and no failed parsed HIP calls.
The candidate contains 9,320 complete calls across 33 threads versus 9,480 for
the baseline. Only these API call counts change:

| API | Baseline | Candidate |
| --- | --- | --- |
| `hipEventCreate` | 960 | 880 |
| `hipEventDestroy` | 960 | 880 |

There are still 480 traced kernel launches, synchronizations and elapsed-time
queries, including validation. All other API counts, transfer bytes and
allocation request sizes/counts match. Savings depend on which chunks reach
packing; this case removes 80 creations and 80 destructions, not a fixed number
for every input. Instrumented worker durations overlap and are not benchmark
wall time. They are not added together to predict speedups.

The baseline runtime appended a process suffix to its configured trace path;
the first private launcher failed to locate the completed file. The actual
bounded trace was recovered and analyzed without rerunning that workload. The
candidate capture handles the observed suffix. A comparison initially included
variable allocation timing fields in its equality assertion; the corrected
comparison checks request sizes and counts on the original retained traces.

Sanitized reports remain at
`out/hip-classification-api-trace-report-20261003.json`,
`out/entropy-event-api-trace-report-20261003.json` and
`out/entropy-event-trace-comparison-20261003.json`. These diagnostics qualify
event ownership and lifecycle reduction on one available GPU, not other devices,
formats, inputs or an overall product performance advantage.
