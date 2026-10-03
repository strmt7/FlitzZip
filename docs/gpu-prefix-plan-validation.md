# GPU Prefix Decode Plan Preparation

The GPU decoder now reserves its complete prefix segment table once. Previously
it called `reserve(current_size + next_block_segments)` for every coded block,
repeatedly allocating and copying the growing table. One shared count function
handles static prefix, adaptive prefix and Huffman blocks. The validated decoded
chunk bound also bounds the segment total; segment counts are not byte sizes.
The plan order, offsets, codebook selection and tail lengths are preserved.

## October 3 Component Evidence

An isolated C++20 study compiles the exact before/after host function bodies
with MSVC 19.51 in Release mode. It uses generated descriptor tables describing
128 MiB minus 1,357 bytes, alternating static/adaptive/Huffman/raw blocks, at
all seven production block sizes. It compares every plan field, including
offsets and mode flags. This fixture's shortened last block is raw; actual HIP
boundary tests separately exercise partial coded segments.

Every case has 24,576 plans and an identical 1,179,648-byte logical table.
Allocation probes report these observed counts:

| Block KiB | Before allocations | After allocations |
| ---: | ---: | ---: |
| 256 | 384 | 1 |
| 512 | 192 | 1 |
| 1024 | 96 | 1 |
| 2048 | 48 | 1 |
| 4096 | 24 | 1 |
| 8192 | 12 | 1 |
| 16384 | 6 | 1 |

At 256 KiB, cumulative allocated bytes fall from 227,097,177 to 1,179,687.
These are allocation traffic, not peak retained RAM. The observed allocator
overhead is included. Raw-only preparation still performs zero allocations.

## Timing Method And Limits

Each case warms both implementations three times. A separate calibration
selects fixed inner repetition counts targeting 50 ms per timed batch, capped
at 10,000 repetitions. The study takes 19 alternating-order pairs per case and
normalizes each batch by its repetition count. All observations remain in the
raw journal; no trimming, outlier removal or significance-based stopping occurs.

The first prototype's 256 KiB baseline CV was 18.75%. Before confirmation, its
variance selected 88 fresh pairs using `ceil((CV / 0.02)^2)`, with a minimum of
30 and maximum of 500. Final source validation retains that fixed plan and its
separate initial and confirmation series. Shared repository distribution code
reports descriptive statistics without a confidence claim:

| Block KiB / phase | Pairs | Before mean ms | After mean ms | Before CV % | After CV % |
| --- | ---: | ---: | ---: | ---: | ---: |
| 256 initial | 19 | 17.190 | 0.198 | 18.32 | 2.31 |
| 256 independent confirmation | 88 | 12.759 | 0.184 | 17.49 | 1.81 |
| 512 initial | 19 | 8.316 | 0.203 | 4.23 | 5.71 |
| 1024 initial | 19 | 3.005 | 0.194 | 1.94 | 0.61 |
| 2048 initial | 19 | 1.720 | 0.188 | 1.22 | 1.53 |
| 4096 initial | 19 | 0.856 | 0.187 | 2.66 | 1.33 |
| 8192 initial | 19 | 0.367 | 0.189 | 1.19 | 1.12 |
| 16384 initial | 19 | 0.210 | 0.191 | 1.06 | 2.41 |

The confirmation baseline's independence-assuming RSE is 1.86%; its CV remains
17.49%. More observations reduce estimated mean uncertainty, not the underlying
variation. Calibration, heap state, compilation, cache effects and within-process
dependence limit inference. Normalized sample sums are not controller wall time.
The CPU is a Ryzen 9 9950X; the initial host checks show 0–3% CPU utilization and
about 30 GiB free RAM, with no native builds or archive benchmarks running.

This is an instrumented host preparation microbenchmark with generated metadata,
not an in-app HIP timing or a whole-archive speed comparison. It does not measure
compression ratio, device allocations, transfers or kernel speed. Before release
throughput claims, run the relevant RAM-only archive comparisons on the accepted
source batch; these numbers cannot substitute for that qualification.

## Production Verification And Reproduction

The ordinary HIP Release build and the 14 existing affected prefix/Huffman
tests validate the production path, including reference packing, mixed-block
selection, all nine effort levels, CPU/HIP restoration and CRC, segment tails,
archive integration and malformed payload/table rejection. GPU readiness must
be checked before and after selecting that cohort; unavailable HIP is not a pass.
The maintainer's component-only direction replaces the unrelated blanket native
test driver for this batch. Unchanged GUI, packaging, compatibility adapters and
hosted fuzz targets are not requalified by these checks.

The isolated study files are ignored local artifacts, not an installed runtime
dependency or a permanent product benchmark. Reproduce it by compiling the exact
old/new planner bodies against their matching block types and format constants,
using the fixture layout and fixed sampling method above. Preserve compiler flags,
source hashes, warmup/calibration records, every pair and allocator instrumentation;
do not compare separately edited or differently configured implementations.

Complete local evidence is retained in:

- `out/prefix-plan-final-timings-20261003.jsonl`
- `out/prefix-plan-final-confirm256-20261003.jsonl`
- `out/prefix-plan-confirmation-plan-20261003.json`
- `out/prefix-plan-final-shared-statistics-20261003.json`
- `out/prefix-plan-final-study-generation-20261003.log`
- `out/prefix-plan-final-component-verification-20261003.json`

The prototype observations are retained separately. During fixture preparation,
an initial allocation assertion incorrectly omitted MSVC's allocator overhead;
it was corrected to inspect logical vector capacity, allocation count and actual
requested bytes. No product defect or measured sample was hidden by that correction.
