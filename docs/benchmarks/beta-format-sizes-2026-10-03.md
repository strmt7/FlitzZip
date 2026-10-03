# Beta Writable-Format Size Coverage — 3 October 2026

All **132 archive/reader cases** completed on six deterministic small fixtures:
Text, SparseRecord and Incompressible at 4 KiB and 1 MiB. This covers all **13
writable formats** at SuperZip effort 5 where the format accepts effort, plus
available independent 7-Zip, Zstandard and libarchive writer cases. Native
SUZIP here is **forced CPU**, with 8 MiB blocks; it is not HIP size evidence.
Separately, the registry-driven correctness smoke passed for **36 registered
formats**, including extract-only fixture exports, aliases, overwrite refusal
and all nine accepted levels for level-aware writers.

These are synthetic size and interoperability checks. **No filesystem timing
was measured.** Small fixtures do not establish real-corpus rankings, general
compression superiority or GPU acceleration. Different container overhead,
metadata and codec defaults make cross-format sizes descriptive. Equal numeric
effort settings do not imply equal work across tools. Exact effective commands,
source/archive SHA-256 hashes, format-specific level mappings and verification
readers are preserved in the [complete numerical record](data/beta-format-sizes-l5-20261003.json).

## Method And Identity

Clean source `8cabac46fa306dbab0dbcb9591440f0b7c50c7ad`; frozen CLI SHA-256
`a2294790af46e0cf243c5012381c41760ba1bd52af517109004c60d9219a0b06`. The pinned 7-Zip
26.03 and Zstandard 1.5.7 tools were used unchanged. Windows' bsdtar banner
reports libarchive 3.8.8 and its bundled components; those components were not
individually changed. Tool binary hashes are in the record. Fixture timestamps
are fixed to 2000-01-01 UTC, with deterministic generator seed 912817.

An initial 1/5/9 plan was rejected **before executing size-comparison cases**
because its conservative total write estimate exceeded 512 MiB. It produced no
size-comparison record. The new level-5 plan stays within that guard, and every
fixture is below the 64 MiB per-case limit. The successful 36-format smoke was
not repeated after this admission failure. These archive/workload writes are
explicit filesystem correctness/size evidence; `disk_write_bytes=0` applies to
the separate native RAM study, not this table.

## Every Size Case

Values are complete archive bytes. An absent independent writer does not mean
that the format failed its SuperZip round trip. The raw record identifies each
independent test/list/decode path; the 36-format registry smoke is correctness
coverage, not a benchmark for extract-only formats.

| Synthetic profile | Input bytes | Format | SuperZip archive bytes | Independent archive bytes |
| --- | ---: | --- | ---: | --- |
| Text | 4,096 | suzip | 202 | No independent writer case |
| Text | 4,096 | zip | 250 | 7-Zip: 269 |
| Text | 4,096 | tar | 5,632 | 7-Zip: 5,632; libarchive bsdtar: 5,632 |
| Text | 4,096 | tar.gz | 194 | libarchive bsdtar: 201 |
| Text | 4,096 | tar.bz2 | 312 | libarchive bsdtar: 307 |
| Text | 4,096 | tar.zst | 163 | libarchive bsdtar: 173 |
| Text | 4,096 | gz | 134 | 7-Zip: 144 |
| Text | 4,096 | z | 1,002 | No independent writer case |
| Text | 4,096 | bz2 | 224 | 7-Zip: 218 |
| Text | 4,096 | zst | 106 | Zstandard CLI: 107 |
| Text | 4,096 | cpio | 4,344 | No independent writer case |
| Text | 4,096 | cpio.gz | 201 | No independent writer case |
| Text | 4,096 | ar | 4,174 | No independent writer case |
| SparseRecord | 4,096 | suzip | 4,195 | No independent writer case |
| SparseRecord | 4,096 | zip | 4,235 | 7-Zip: 4,250 |
| SparseRecord | 4,096 | tar | 5,632 | 7-Zip: 5,632; libarchive bsdtar: 5,632 |
| SparseRecord | 4,096 | tar.gz | 4,256 | libarchive bsdtar: 4,265 |
| SparseRecord | 4,096 | tar.bz2 | 4,687 | libarchive bsdtar: 4,676 |
| SparseRecord | 4,096 | tar.zst | 4,201 | libarchive bsdtar: 4,203 |
| SparseRecord | 4,096 | gz | 4,119 | 7-Zip: 4,130 |
| SparseRecord | 4,096 | z | 5,627 | No independent writer case |
| SparseRecord | 4,096 | bz2 | 4,587 | 7-Zip: 4,580 |
| SparseRecord | 4,096 | zst | 4,109 | Zstandard CLI: 4,110 |
| SparseRecord | 4,096 | cpio | 4,344 | No independent writer case |
| SparseRecord | 4,096 | cpio.gz | 4,249 | No independent writer case |
| SparseRecord | 4,096 | ar | 4,174 | No independent writer case |
| Incompressible | 4,096 | suzip | 4,195 | No independent writer case |
| Incompressible | 4,096 | zip | 4,235 | 7-Zip: 4,250 |
| Incompressible | 4,096 | tar | 5,632 | 7-Zip: 5,632; libarchive bsdtar: 5,632 |
| Incompressible | 4,096 | tar.gz | 4,256 | libarchive bsdtar: 4,265 |
| Incompressible | 4,096 | tar.bz2 | 4,678 | libarchive bsdtar: 4,672 |
| Incompressible | 4,096 | tar.zst | 4,201 | libarchive bsdtar: 4,203 |
| Incompressible | 4,096 | gz | 4,119 | 7-Zip: 4,130 |
| Incompressible | 4,096 | z | 5,627 | No independent writer case |
| Incompressible | 4,096 | bz2 | 4,594 | 7-Zip: 4,580 |
| Incompressible | 4,096 | zst | 4,109 | Zstandard CLI: 4,110 |
| Incompressible | 4,096 | cpio | 4,344 | No independent writer case |
| Incompressible | 4,096 | cpio.gz | 4,248 | No independent writer case |
| Incompressible | 4,096 | ar | 4,174 | No independent writer case |
| Text | 1,048,576 | suzip | 288 | No independent writer case |
| Text | 1,048,576 | zip | 3,796 | 7-Zip: 4,044 |
| Text | 1,048,576 | tar | 1,050,112 | 7-Zip: 1,050,112; libarchive bsdtar: 1,050,112 |
| Text | 1,048,576 | tar.gz | 3,747 | libarchive bsdtar: 3,751 |
| Text | 1,048,576 | tar.bz2 | 962 | libarchive bsdtar: 737 |
| Text | 1,048,576 | tar.zst | 264 | libarchive bsdtar: 271 |
| Text | 1,048,576 | gz | 3,680 | 7-Zip: 3,919 |
| Text | 1,048,576 | z | 22,549 | No independent writer case |
| Text | 1,048,576 | bz2 | 869 | 7-Zip: 635 |
| Text | 1,048,576 | zst | 193 | Zstandard CLI: 193 |
| Text | 1,048,576 | cpio | 1,048,824 | No independent writer case |
| Text | 1,048,576 | cpio.gz | 3,750 | No independent writer case |
| Text | 1,048,576 | ar | 1,048,654 | No independent writer case |
| SparseRecord | 1,048,576 | suzip | 16,805 | No independent writer case |
| SparseRecord | 1,048,576 | zip | 25,241 | 7-Zip: 24,168 |
| SparseRecord | 1,048,576 | tar | 1,050,112 | 7-Zip: 1,050,112; libarchive bsdtar: 1,050,112 |
| SparseRecord | 1,048,576 | tar.gz | 25,218 | libarchive bsdtar: 23,750 |
| SparseRecord | 1,048,576 | tar.bz2 | 81,308 | libarchive bsdtar: 62,591 |
| SparseRecord | 1,048,576 | tar.zst | 16,804 | libarchive bsdtar: 16,863 |
| SparseRecord | 1,048,576 | gz | 25,125 | 7-Zip: 24,043 |
| SparseRecord | 1,048,576 | z | 479,092 | No independent writer case |
| SparseRecord | 1,048,576 | bz2 | 81,252 | 7-Zip: 62,459 |
| SparseRecord | 1,048,576 | zst | 16,710 | Zstandard CLI: 16,710 |
| SparseRecord | 1,048,576 | cpio | 1,048,824 | No independent writer case |
| SparseRecord | 1,048,576 | cpio.gz | 25,215 | No independent writer case |
| SparseRecord | 1,048,576 | ar | 1,048,654 | No independent writer case |
| Incompressible | 1,048,576 | suzip | 1,048,675 | No independent writer case |
| Incompressible | 1,048,576 | zip | 1,048,880 | 7-Zip: 1,048,730 |
| Incompressible | 1,048,576 | tar | 1,050,112 | 7-Zip: 1,050,112; libarchive bsdtar: 1,050,112 |
| Incompressible | 1,048,576 | tar.gz | 1,048,928 | libarchive bsdtar: 1,049,073 |
| Incompressible | 1,048,576 | tar.bz2 | 1,054,685 | libarchive bsdtar: 1,053,810 |
| Incompressible | 1,048,576 | tar.zst | 1,049,135 | libarchive bsdtar: 1,049,135 |
| Incompressible | 1,048,576 | gz | 1,048,764 | 7-Zip: 1,049,075 |
| Incompressible | 1,048,576 | z | 1,297,478 | No independent writer case |
| Incompressible | 1,048,576 | bz2 | 1,054,326 | 7-Zip: 1,053,809 |
| Incompressible | 1,048,576 | zst | 1,048,613 | Zstandard CLI: 1,048,613 |
| Incompressible | 1,048,576 | cpio | 1,048,824 | No independent writer case |
| Incompressible | 1,048,576 | cpio.gz | 1,048,937 | No independent writer case |
| Incompressible | 1,048,576 | ar | 1,048,654 | No independent writer case |

## Reproduce

```powershell
tools/format_matrix_smoke.ps1 -Configuration Release
tools/compare_format_sizes.ps1 -Configuration Release -OutputJson out/benchmarks/new-format-sizes-l5.json
```

Both commands require a current frozen Release build. The size tool requires
the pinned comparison tools, bounds filesystem work before case execution and
refuses existing report paths. Its default cases match this study.

Representative licensed corpora, comparator timings and unavailable independent
writer cells remain separate beta/release research work. This record does not
qualify beta publication or remove unresolved security/installer gates.
