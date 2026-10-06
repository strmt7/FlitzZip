# Archive Size Versus Creation Time: Silesia Effort Sweep

This is a measured Windows application comparison, not a codec-only or native
GPU benchmark. The [methodology](../../comparative-benchmark-methodology.md)
defines the corpus, timing boundary, correctness checks, and publication gate.
The [graph](../../../resources/benchmarks/effort-tradeoff.svg) is generated from
five [reviewed raw records](../../benchmarks/data) and byte-checked in CI. Every point is a
different explicit **tool-local** effort setting; equal numeric settings do
not mean equal work across tools.

## System And Procedure

| Component | Configuration |
| --- | --- |
| OS | Microsoft Windows 11 Pro, build 26200 |
| CPU | AMD Ryzen 9 9950X, 16 cores / 32 threads |
| GPU | AMD Radeon RX 9070 XT, driver 32.0.31041.1004; not used for these ZIP/ZST compatibility commands |
| RAM | 68,305,387,520 bytes installed, two modules at configured 5200 MT/s |
| Storage | Lexar SSD NM790 4TB, NVMe; source and output on the same volume |
| SuperZip | 0.5.0 x64 Release/HIP build, source `33f71ee38ee3d3c394dcac2d25cb41d3610f1e67` |
| Comparators | Official 7-Zip 26.03 x64 CLI and Zstandard 1.5.7 x64 CLI |

The SuperZip executable SHA-256 is
`1fb2432498fef27aad008d2641d4b8e85a88961f4ef210476525326f456d90b6`.
The records include every comparator binary SHA-256, CMake cache digest,
source/download hash, full portable command vector, and pre/post host sample.
Each case used one warmup, ten timed runs per tool and direction, alternating
tool order, warm input cache, and a 250 ms pause after each paired filesystem
round outside the timer. The timer covers the full archive command, not
hashing, cleanup, or independent decoding. The multi-file ZIP source has
51,770,558 bytes; `mozilla` has 51,220,480 bytes; `nci` has 33,553,445 bytes.
These are three selected Silesia cases, not the complete corpus.

Every generated archive was decoded by the other application and checked for
exact paths, sizes, and SHA-256 hashes. Extraction runs used one common
comparator-produced reference per case and effort. All checks passed. Archive
bytes include container overhead. The graph uses exact archive bytes versus
median full-command creation time; the vertical time axis is logarithmic and
the adjacent table shows the observed min/max range. No ZIP, ZST, and SUZIP
bytes are pooled into a single ranking.

## Measured Points

Times below are median seconds of ten independent whole-command runs. The
graph and JSON retain every individual timing and its observed range.

| Input / format | Tool | Level | Archive bytes | Create s |
| --- | --- | ---: | ---: | ---: |
| mixed-files / ZIP | SuperZip | 1 | 21,891,618 | 0.449 |
| mixed-files / ZIP | SuperZip | 3 | 19,573,311 | 0.700 |
| mixed-files / ZIP | SuperZip | 5 | 19,263,206 | 0.952 |
| mixed-files / ZIP | SuperZip | 7 | 19,048,811 | 1.628 |
| mixed-files / ZIP | SuperZip | 9 | 19,030,687 | 1.910 |
| mixed-files / ZIP | 7-Zip | 1 | 19,521,739 | 0.212 |
| mixed-files / ZIP | 7-Zip | 3 | 19,521,739 | 0.212 |
| mixed-files / ZIP | 7-Zip | 5 | 18,318,370 | 1.005 |
| mixed-files / ZIP | 7-Zip | 7 | 18,212,231 | 2.553 |
| mixed-files / ZIP | 7-Zip | 9 | 18,177,358 | 6.006 |
| mozilla / ZST | SuperZip | 1 | 19,968,243 | 0.142 |
| mozilla / ZST | SuperZip | 3 | 18,387,142 | 0.188 |
| mozilla / ZST | SuperZip | 5 | 17,776,792 | 0.286 |
| mozilla / ZST | SuperZip | 7 | 16,294,414 | 4.845 |
| mozilla / ZST | SuperZip | 9 | 14,950,836 | 11.564 |
| mozilla / ZST | Zstd | 1 | 19,983,517 | 0.035 |
| mozilla / ZST | Zstd | 3 | 18,281,292 | 0.054 |
| mozilla / ZST | Zstd | 5 | 17,697,993 | 0.091 |
| mozilla / ZST | Zstd | 7 | 17,073,567 | 0.138 |
| mozilla / ZST | Zstd | 9 | 16,735,963 | 0.221 |
| nci / ZST | SuperZip | 1 | 2,848,726 | 0.078 |
| nci / ZST | SuperZip | 3 | 2,852,239 | 0.082 |
| nci / ZST | SuperZip | 5 | 2,578,603 | 0.128 |
| nci / ZST | SuperZip | 7 | 1,838,951 | 1.924 |
| nci / ZST | SuperZip | 9 | 1,586,505 | 13.625 |
| nci / ZST | Zstd | 1 | 2,853,982 | 0.022 |
| nci / ZST | Zstd | 3 | 2,835,400 | 0.027 |
| nci / ZST | Zstd | 5 | 2,583,327 | 0.039 |
| nci / ZST | Zstd | 7 | 2,357,794 | 0.053 |
| nci / ZST | Zstd | 9 | 2,247,493 | 0.100 |

The ZIP comparison has a genuine size/time tradeoff: 7-Zip made the smallest
ZIP at its highest effort, while SuperZip was faster than 7-Zip at levels
5-9 on this input but produced larger archives. At settings 1 and 3, 7-Zip
produced exactly the same archive byte count, showing that a setting change
need not imply a different size. On `nci`, SuperZip level 3 was slightly
larger than level 1; higher effort does not mathematically guarantee a
smaller result for every file. For ZST, SuperZip's high product levels mapped
to much higher backend effort: they produced smaller archives than the
official Zstd CLI's tested levels but took far longer. The official Zstd CLI
was faster for all tested ZST points. These are observations on these files
and this host, not universal rankings or statistically significant Pareto
wins.

## Independent Tool Checks

Official Hyperfine 1.20.0 (portable Windows x64 executable SHA-256
`fbad9dfc44e98e47aaaf352f83f9c5273fc5008658a2f38fb5844adc9fa9918b`;
official ZIP SHA-256
`2508c549b049b1d4342d08edc1cb42bfac169082b6e3069431b5bab9822dbb32`)
timed the same level-5 `mozilla` creation commands without an intermediate
shell. Each run had a 250 ms preparation pause and removed only its dedicated
benchmark output before timing. With one warmup, SuperZip's ten runs had
median 0.285 s and range 0.277-0.292 s. Zstd's thirty runs had median
0.091 s and range 0.089-0.098 s. These agree closely with the primary
AB/BA ten-run medians (0.286 and 0.091 s). Hyperfine runs each command in a
separate series, so they are a cross-check, not pooled statistical samples.
The final output of each series decoded independently to the input SHA-256
`657fc3764b0c75ac9de9623125705831ebbfbe08fed248df73bc2dc66e2a963b`.

Zstd's official in-memory `-b5 -i10 -S` benchmark on `mozilla` reported
17,697,989 compressed bytes, about 705 MB/s compression and 1,986 MB/s
decompression in its final update. Its size differs by four bytes from the
actual 17,697,993-byte `.zst` archive, demonstrating why codec-buffer size
must not replace whole-archive bytes. Official 7-Zip 26.03 `b -mmt=on` ran
its built-in 32-thread synthetic test and reported average compression and
decompression ratings of 164,300 and 231,242 MIPS respectively. Neither
built-in mode timed SuperZip or the filesystem application workflow; those
numbers are **not** plotted as comparable SuperZip performance. The
[OpenBenchmarking 7-Zip profile](https://openbenchmarking.org/test/pts/compress-7zip-1.8.0)
and [Zstd profile](https://openbenchmarking.org/innhold/8a321b74f5a88fc2e6fa0f9d04430ac555313bb0)
use these kinds of built-in ratings for processor comparisons, which is a
different question from this application tradeoff study.

## Stability And Limits

Across the thirty case-level-tool pairs and both directions, all 60 ten-run
series were checked for a sustained late-run slowdown. None was strictly
monotonic slower; none combined a >5% last-half median increase with a strong
positive Kendall trend (>0.6). This is an exploratory drift diagnostic, not
a guarantee that caches or clock rates were constant. Short ZST extraction
commands had some alternating spikes and are less precise in relative terms.
The 30 case pre/post resource samples showed 0-13% total CPU load, 0-6% disk
busy time, 0-63 paging pages/s, 36.7-37.8 billion free RAM bytes, and
0.63-0.79% maximum sampled GPU-engine utilization. These are coarse snapshots
outside timed commands, not continuous contention telemetry. Power plan,
thermal throttling, and internal thread scheduling were not measured. No
claim is made about other hardware, cold-cache startup, energy use, or every
supported format. Native CPU/GPU mode remains a separate RAM-only study.
