# Native Decoder Context Review

## Decision

Do not promote the worker-local Zstandard decoder-context experiment. It passed
correctness checks but did not demonstrate a repeatable speed benefit. The
production decoder and its exact-frame validation remain unchanged. Retain the
new regressions for independent-frame error handling and selective CPU decoding.
There is no new CPU or GPU speedup claim from this investigation.

## Mechanism And Build Configuration

[Zstandard 1.5.7's stable API](https://github.com/facebook/zstd/blob/v1.5.7/lib/zstd.h)
recommends an exclusive reusable context for repeated decoding. However, the
bundled SuperZip DLL is compiled with `ZSTD_HEAPMODE=0`.
[The pinned implementation](https://github.com/facebook/zstd/blob/v1.5.7/lib/decompress/zstd_decompress.c)
therefore initializes its one-shot context on the stack, rather than allocating
and freeing a heap context for every native block. The initial heap-allocation
rationale was incorrect for this build and was corrected before acceptance.

The candidate instead created one lazy heap context per CPU worker range using
the stable `ZSTD_createDCtx`, `ZSTD_decompressDCtx`, and `ZSTD_freeDCtx` exports.
It retained frame-boundary and declared-size admission, exclusive ownership,
RAII cleanup, and independent jobs after decode errors. It added no global pool,
new dependency, compression-policy change, or GPU fallback.

Removing repeated stack initialization is a different hypothesis from removing
per-block heap allocation. The source proves that distinction; these aggregate
measurements do not isolate initialization, memory bandwidth, CRC, or scheduler
costs sufficiently to assign a percentage to any one of them. Raw-block negative
controls also vary without entering the changed Zstandard path.

## Diagnostic Method

The comparison used the existing production RAM benchmark and sampler through
the local alternating-build diagnostic driver. No alternate product algorithm
or timing-only code was added to the CLI. Each run processed 10 GiB at effort
five with 32 aggregate workers, asserted exact read-back checksums, reported
`memory_only=true`, and wrote zero archive bytes to storage.

- Control: clean source `c916064759b4529778fa90320008f3de69077fcc`.
- Control CLI SHA-256: `077e6fe4a70523554e558758eab959e3d6c29497462ee49637b62226c716099a`.
- Experimental CLI SHA-256: `d4a42535d31e496851ce4188caf32ffe7ebe3ae7f36410fb2602a31d12f5c565`.
- Default-block study: Mixed, Compressible, and Incompressible; 8 MiB blocks;
  three alternating pairs per profile, 18 total runs.
- Follow-up: the existing RepeatedRecord workload; 256 KiB blocks; AB/BA order,
  two pairs, four total runs. This tests greater independent-frame density.
- Pause: 500 ms outside each timed run. No large filesystem benchmark was run.

The host was Windows 11 Pro build 26300, Ryzen 9 9950X with 16 cores/32 logical
processors, approximately 63.61 GiB OS-visible RAM, and Radeon RX 9070 XT.
The HIP-enabled Release build used CMake 4.4.3 and MSVC 19.51.36260.0; the loaded
HIP runtime was 10.0.3679.0. Only the CPU lane was timed for this experiment.
These are diagnostic measurements, not five-repeat commercial comparisons or
release graphs, and not evidence for other host architectures.

## Results

Each default-block control/candidate pair produced the same complete archive
size. The table reports medians in seconds; lower is faster.

| Profile | Archive Bytes | Compress Control / Candidate | Verify Control / Candidate | Extract Control / Candidate |
| --- | ---: | ---: | ---: | ---: |
| Mixed | 4,516,089,518 | 8.34125 / 8.27609 | 0.948657 / 0.949982 | 0.967928 / 0.950659 |
| Compressible | 1,074,642,523 | 2.31592 / 2.32261 | 0.959729 / 0.968441 | 0.893435 / 0.877798 |
| Incompressible | 10,737,441,371 | 5.90186 / 5.93806 | 0.995285 / 0.994008 | 0.862194 / 0.850406 |

Paired extraction speedups span 1.000-1.073x for Mixed, 0.969-1.052x for
Compressible, and 1.001-1.025x for Incompressible. Improvements in the raw-block
negative control and mixed verification outliers must not be attributed to
decoder-context reuse. In the smaller-block follow-up, both archives were
673,341,531 bytes; extraction speedups were 1.016x and 0.942x, while verification
speedups were 1.004x and 0.980x. The follow-up did not establish a repeatable win.
No result was deleted or smoothed to manufacture a trend.

The default-block record contains 69 sparse host observations: available RAM
stayed at or above 23,969 MiB, observed total CPU peaked at 73.13%, disk busy time
at 7.71%, and paging at 31.54 pages/s. The 15 follow-up observations had at least
31,409 MiB available RAM. Both records have zero invalid counter samples and
zero reported query errors. Short per-process CPU peaks were higher than the
sparse host samples; these observations do not prove exclusive hardware,
thermal stability, or absence of brief contention. Archive writes were zero,
not all system or diagnostic-log writes.

## Coverage And Retained Evidence

The experimental binary passed the full 28-command local profile, including
552 native tests, independent interoperability, the 36-format matrix, 14
instrumented parser fuzz targets, eight-page GUI smoke and screenshot review,
and unpublished portable packaging. Parser fuzzing is not a sanitizer claim
for the Windows Zstandard DLL wrapper. After removing the experimental
production code, the final retained-test build separately passed its six
classifier-selected checks, including 552 native tests, changed-function
contracts, lint, hygiene, and offline benchmark-tooling regressions.

Ignored local evidence:

- `out/benchmarks/decoder-context-paired-2026-10-01T231925-5693774Z-n18.json`
- `out/benchmarks/decoder-context-small-block-2026-10-01T232409-9581657Z-n4.json`
- `out/decoder-context-full-verification-20261002.log`
- `out/research/decoder-context-rejected.patch`
- `out/controls/c916-decoder/` and `out/controls/decoder-context-rejected/`

The 18-run JSON SHA-256 is
`f65371d1f847cd85cf3ecd0bc84fff215b38586f8ba0ac72e7cdb68d3ebe6ea9`;
the four-run JSON SHA-256 is
`d8c070a2bcb110f39f1f0f90160e0d8fbc1902288c09dab7a644a8ee8102ebb9`.
The rejected patch SHA-256 is
`0dc502f8033ff13990a82623a1286a1086f6e317ced771c66406f4c6ac36c642`.

Reuse this conclusion while the pinned backend configuration, production
decode policy, workload definitions, and diagnostic method remain unchanged.
A backend upgrade, heap-mode change, or independent stage evidence is a reason
to reconsider it; an unchanged aggregate rerun alone is not.

## Next HIP Investigation

Prior stage evidence identifies device classification as a large overlapping
worker-time category, not pure kernel time. Source review also finds that the
current resident CRC kernel assigns up to 32 KiB of serial table-dependent work
to each thread. These facts motivate investigating shorter per-thread ranges
with an ordered cooperative CRC reduction, not assuming CRC alone explains the
whole category. Merely shrinking segments would increase metadata and CPU
combination work and is not an adequate design.

[AMD's reduction guidance](https://rocm.docs.amd.com/projects/HIP/en/latest/tutorial/reduction.html)
supports investigating cooperative work, bounded scratch storage, and reduced
synchronization costs. A CRC design must preserve byte order, finalized IEEE
CRC semantics, arbitrary tails, independent block boundaries, real HIP timing,
and existing resource admission. It needs independent CPU-oracle tests and
alternating CPU/GPU measurements before promotion. No replacement CRC kernel
or new GPU gain has been implemented or established by this review.
