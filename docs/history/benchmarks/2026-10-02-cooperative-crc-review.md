# Cooperative HIP CRC Review

## Decision And Scope

Retain the ordered cooperative CRC implementation as a targeted GPU verification
improvement, with the acceptance limits below. It changes production source-CRC,
independent-range CRC, and simple decoded-CRC kernels. There is no benchmark-only
codec, new runtime dependency, or format change. This is not a completed GPU
performance overhaul or release comparison.

The review also found and fixed a correctness defect: empty HIP jobs reported
GPU work without dispatch, and empty borrowed decode/verification returned
before validating malformed block metadata. Empty jobs now report no GPU work
and reject invalid layouts. Required-GPU readiness and nonempty fallback policy
are unchanged.

## Mechanism And Bounds

The control assigned an entire 8 or 32 KiB CRC segment to one thread. Each byte
depended on the preceding table lookup. The candidate assigns one 256-thread
block to each existing segment and divides it into contiguous ranges. An
adjacent-pair reduction combines finalized IEEE CRCs in source order using the
exact following length. CRC concatenation with lengths is associative but not
commutative; an unordered reduction would be wrong.

The immutable device table occupies 2 KiB. Each block uses 2 KiB of shared
checksum/length storage and nine uniform barriers; empty tail threads contribute
CRC zero and length zero. Compact result geometry, host combination, allocation
admission, transfers and dispatch count remain unchanged; launch geometry
changes. CPU/HIP operator generation shares one private implementation, retaining
the fixed 8 KiB CPU table. [AMD reduction guidance](https://rocm.docs.amd.com/projects/HIP/en/latest/tutorial/reduction.html)
and the [zlib CRC implementation](https://github.com/madler/zlib/blob/develop/crc32.c)
inform the design, not a product dependency or measured speedup claim.

## Method And Evidence

Production RAM-benchmark runs processed 10 GiB at effort five with 32 aggregate
workers. Control/candidate alternated AB/BA order with a 500 ms pause outside
timing. The 8 MiB-block study used three pairs per profile/lane, both CPU and GPU:
36 runs. A separate two-pair GPU-only follow-up added counter-failure
classification: 12 runs. Failed invocations were not counted as passing runs.

The host was Windows 11 Pro build 26300, Ryzen 9 9950X (16 cores/32 threads),
approximately 63.61 GiB OS-visible RAM, and Radeon RX 9070 XT (`gfx1201`). The
HIP-enabled Release build used CMake 4.4.3, host MSVC 19.51.36260.0, and loaded
HIP runtime 10.0.3679.0. The HIP helper selected MSVC toolset 14.44.

- Control commit: `64a3142789ea0a8435b4e3a86fc6db0a8fbc2cb7`.
- Control CLI SHA-256: `271a17d5ac8e5f21d859a37dfc462db271bca1901e1a4ae6833ebdb189b7a058`.
- Candidate CLI SHA-256: `e088f9985266b11ddd1891ebc8c023adabf5d0ed5ee52c01b8e486986e899b5b`.

The [review record](../../benchmarks/data/cooperative-crc-diagnostic-2026-10-02.json) preserves
individual phase times, exact sizes, nine dirty-source file hashes and original
local-record hashes. The ignored local comparison driver selects a CLI for the
unchanged production RAM benchmark/sampler; it adds no timing-only codec path.
Raw host observations and driver remain under `out/`. This is not the final
portable comparison pipeline or five-repeat-per-point release data.

## Results And Limits

Default-block medians follow, in seconds; lower is faster. Every control/candidate
pair has identical complete archive size within its profile/lane. CPU and GPU
sizes differ and are not equal-ratio comparisons.

| Profile / Lane | Archive Bytes | Compress Control / Candidate | Verify Control / Candidate | Extract Control / Candidate |
| --- | ---: | ---: | ---: | ---: |
| Mixed CPU | 4,516,089,518 | 8.28738 / 7.91954 | 0.938344 / 0.911250 | 0.933922 / 0.932056 |
| Mixed GPU | 4,527,857,731 | 3.13215 / 3.06285 | 0.643198 / 0.600517 | 1.06105 / 1.07131 |
| Compressible CPU | 1,074,642,523 | 2.40516 / 2.37259 | 0.922751 / 0.918123 | 0.890579 / 0.859466 |
| Compressible GPU | 1,073,857,115 | 1.75665 / 1.70604 | 0.156375 / 0.097987 | 0.577180 / 0.601762 |
| Incompressible CPU | 10,737,441,371 | 5.69768 / 5.82514 | 0.944067 / 0.933349 | 0.810396 / 0.818169 |
| Incompressible GPU | 10,737,441,371 | 2.02483 / 1.94624 | 0.590073 / 0.549658 | 0.862320 / 0.866566 |

GPU verification improved in every original pair: 1.057-1.092x on Mixed,
1.586-1.638x on Compressible, and 1.071-1.084x on Incompressible. Compression
gains were modest/variable; the first Mixed pair included a control outlier.
CPU negative controls varied too, and Incompressible CPU compression was
slightly slower in every pair. Not all timing changes are CRC-kernel gains.

GPU extraction was slower in all original Mixed/Compressible pairs. Follow-up
Compressible and Incompressible extraction overlapped the control, but Mixed
remained slower (speedups 0.9585x and 0.9823x). Verification improved in every
follow-up pair. Nonempty extraction calls `decode_owned_chunk` and checks host
output with the unchanged CPU CRC backend, not the new device CRC kernels.
This rules out a direct new-kernel call in that phase, not its timing difference.
Prior-phase state, scheduling and transfers remain hypotheses. Mixed extraction
needs stage-level investigation before final performance acceptance.

The candidate also passed all three profiles at all seven block sizes in both
lanes: 42 RAM-only cases with finite positive HIP timing where applicable.
Every run recorded `memory_only=true`, `disk_write_bytes=0`, and successful
read-back validation. These single-iteration sweeps establish configuration
coverage, not repeatable gains at every size. No README graph was regenerated.

## Resource Observations

The original 136 sparse observations had at least 23,122 MiB available RAM,
CPU at most 78.46%, disk busy time at most 9.47%, and observed competing GPU
load at most 6.06%. One 3,743.18 pages/s observation occurred after a Compressible
candidate run. There were 96 invalid samples and 12 query errors; none are hidden.

The follow-up's 47 observations had at least 25,951 MiB available RAM, CPU at
most 78.15%, disk at most 5.30%, paging at most 47.27 pages/s and competing GPU
load at most 11.90%. Each of seven errored queries had eight invalid GPU samples
belonging to the benchmark's own child, which had exited by the subsequent
liveness check. Status `0xC0000BC6` is
[`PDH_INVALID_DATA`](https://learn.microsoft.com/en-us/windows/win32/perfctrs/pdh-error-codes).
This identifies the follow-up failure scope, not every older error's cause.
Errors/unavailable values remain explicit. Sparse observations do not prove
exclusive hardware or thermal stability. Zero archive writes is not zero system
or diagnostic-log writes.

## Validation And Remaining Work

The first new empty-input test failed and exposed the work-identity bug; it was
not weakened. After repair, all 28 full-profile commands passed, including 555
native tests, independent interoperability, the 36-format matrix, 14 ASan/UBSan
parser targets with 16 runs each, eight GUI pages with screenshot review, and
unpublished portable packaging. Changed-function contracts passed. The source
fingerprint remained unchanged throughout timing.

The changed HIP unit compiled for `gfx1100`, `gfx1101`, `gfx1102`, `gfx1151`,
`gfx1200` and `gfx1201`; only `gfx1201` ran on hardware. Compilation was serial,
below-normal priority, within a 15,815 MiB planning budget. This is not a hard
process-memory cap or six-device runtime proof. Independent bitwise CPU oracles
cover unaligned source spans, lane/segment tails, unequal ranges, raw/fill/pattern
mixtures and corruption. Existing all-level/all-block batch identity checks pass.

Mixed extraction, classification/API profiling, broader hardware runtime
validation, final clean-source five-repeat comparisons, remaining security
findings and installer acceptance remain open. Parser fuzzing does not sanitize
these Windows HIP kernels. Release publication still requires human approval.
