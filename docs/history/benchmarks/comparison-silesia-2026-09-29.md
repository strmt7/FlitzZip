# Archive Application Comparison: Silesia Subset

This is a bounded Windows application comparison measured on 2026-09-29.
The [methodology](../../comparative-benchmark-methodology.md) defines the input,
commands, timing boundary, correctness checks, and limitations. The
[reviewed JSON](../../benchmarks/data/comparison-silesia-current.json) contains each individual
timing, archive byte count/hash, input hash, executable identity, command,
and pre/post resource sample. The [chart](../../../resources/benchmarks/application-comparison.svg)
is regenerated from that JSON in CI.

## Test System

| Component | Measured configuration |
| --- | --- |
| OS | Microsoft Windows 11 Pro, build 26200 |
| CPU | AMD Ryzen 9 9950X, 16 physical cores / 32 logical threads |
| GPU | AMD Radeon RX 9070 XT, driver 32.0.31041.1004 |
| RAM | 68,305,387,520 installed bytes, two modules, configured 5200 MT/s |
| Storage | Lexar SSD NM790 4TB, NVMe, same source/output volume |
| SuperZip build | 0.5.0, x64 Release, Visual Studio 17 2022, HIP enabled, gfx1201 |
| Comparators | 7-Zip 26.03 x64 CLI; Zstandard CLI 1.5.7 x64 |
| Source | `c9ca58278512bb6d4d47591c63b2d1a6535af4a0` |

The SuperZip binary SHA-256 was
`1fb2432498fef27aad008d2641d4b8e85a88961f4ef210476525326f456d90b6`.
The record includes the comparator executable hashes and the CMake cache
digest. HIP being compiled in does not mean these compatibility-format
commands used the GPU. No GPU acceleration is claimed for this study.
SuperZip used its normal CLI thread behavior; 7-Zip was invoked with
`-mmt=on`; Zstd had no explicit thread-count flag. The Zstd 1.5.7 CLI help
states that its default is one compression thread, with separate I/O unless
`--single-thread` is selected. These process pipelines are not identical.
All tools used a stated numeric level 5, which is **not** equal algorithmic
effort across products.

## Results

Times are the median of five full-process, warm-cache wall-clock runs in
seconds. Brackets give the observed minimum and maximum, not confidence
intervals. The extraction pair in each case reads one identical
comparator-created reference archive; every created archive was also decoded
independently and checked against exact input hashes.

| Format / case | Tool | Archive bytes | Create s, median [range] | Extract s, median [range] |
| --- | --- | ---: | ---: | ---: |
| ZIP / mixed-files (51,770,558 input B) | SuperZip | 19,263,206 | 0.965 [0.956-0.980] | 0.327 [0.316-0.339] |
| ZIP / mixed-files | 7-Zip | 18,318,370 | 1.021 [1.011-1.066] | 0.174 [0.172-0.177] |
| ZST / mozilla (51,220,480 input B) | SuperZip | 17,776,792 | 0.284 [0.281-0.300] | 0.105 [0.098-0.126] |
| ZST / mozilla | Zstd | 17,697,993 | 0.095 [0.092-0.097] | 0.044 [0.044-0.045] |
| ZST / nci (33,553,445 input B) | SuperZip | 2,578,603 | 0.127 [0.123-0.131] | 0.065 [0.059-0.077] |
| ZST / nci | Zstd | 2,583,327 | 0.041 [0.039-0.043] | 0.025 [0.024-0.026] |

Within this host and corpus, SuperZip made the mixed ZIP slightly faster but
larger than 7-Zip; 7-Zip extracted the common ZIP faster. The official Zstd
CLI compressed and extracted faster in both `.zst` cases. SuperZip's `nci`
archive was 4,724 bytes smaller, while its `mozilla` archive was 78,799
bytes larger. These are observed tradeoffs, not an overall ranking. Archive
sizes include container overhead.

## Measurement Limits

The six pre/post case samples and two overall samples showed 10-24% CPU load,
33.9-34.7 GB free RAM, 0-3 pages/s, 0-2% disk busy time, and 0.65-0.78%
maximum sampled GPU-engine utilization. These are coarse snapshots, not a
continuous contention trace. The host was not reserved exclusively for this
study. Power plan, thermal behavior, and competitor internal thread counts
were not measured. Slight differences therefore need independent repetition
before being called portable wins. The Silesia subset is not a modern
document/media workload or the full corpus; the three cases do not represent
all supported formats, other hardware, or energy use. The common reference
archive isolates the reader timing from archive-size differences, but a broad
decoder ranking would also require crossed writer-produced references.
