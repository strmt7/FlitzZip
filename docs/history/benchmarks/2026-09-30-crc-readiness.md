# CRC Combination And Device Readiness Diagnostic

This is a development diagnostic, not a commercial comparison, release
performance guarantee, or replacement for the
[comparison methodology](../../comparative-benchmark-methodology.md).

## Scope

The baseline is `2fdcc5b99366d1a78434ad85e6d3293561e4ca47`.
The candidate was its uncommitted CRC-operator/readiness patch, with identical
codec policy, archive framing, and GPU kernels. Measurements used the
HIP-enabled Release build on a Ryzen 9 9950X and Radeon RX 9070 XT
(`gfx1201`), driver 32.0.31041.1004. Other architectures are not runtime-proven
by this experiment. Generated payloads stayed in RAM and reported zero disk
writes; unrelated host processes were not stopped or reconfigured.

## Isolated Combination

Two alternating pairs combined 327,680 finalized CRCs with 32 KiB following
segments, corresponding to the segment count of a 10 GiB large-chunk checksum.
The old implementation took 1.21216 and 1.20741 seconds; the reusable-operator
implementation took 0.0257524 and 0.025707 seconds, including first use.
Both returned `543525795`, independently reproduced by upstream zlib 1.3.2.
This is about 47x faster **for host CRC combination only**, not for an archive
operation. The combined logical stream was not materialized.

## Whole Operations

Each profile used two alternating pairs in baseline/candidate then
candidate/baseline order, with a 500 ms gap between children. Each native
`memory-benchmark` processed 10 GiB with level 5, 8 MiB blocks, 32 workers,
required HIP, decoded CRC-32 checking, and backend telemetry.
Source allocation and generation are included in the reported compression
phase. The separately reported generation and codec worker times sum concurrent
tasks; neither can be subtracted from wall time to derive codec-only throughput.
Ratios below are paired baseline time divided by candidate time; they are not ratios of
rounded throughput values.

| Profile | Complete Archive Bytes | Compression Paired Speedup | Verification Paired Speedup | Extraction Paired Speedup |
| --- | ---: | --- | --- | --- |
| Mixed | 4,527,858,919 | 0.982-1.006x | 1.143-1.151x | 0.974-1.027x |
| Compressible | 1,073,857,115 | 1.009-1.041x | 1.244-1.301x | 0.956-0.976x |
| Incompressible | 10,737,441,371 | 1.001-1.066x | 1.307-1.321x | 1.039-1.527x |

Every paired archive size was identical. The stable verification reductions
are encouraging but are limited to these diagnostic runs. Compression and
extraction changes are too small or variable for a broad speed claim. In
particular, the first incompressible extraction improvement was not repeated
at the same magnitude. A short independent interoperability correctness check
also ran during the compressible series; that series was not an exclusive-host
measurement.

Host observations captured CPU, available RAM, storage activity, and unrelated
GPU engine use. Mixed pairs observed external engine peaks of 65-79%; the
other profile pairs peaked at 3-11%. GPU engine peaks are not proof of sustained
whole-device load, but they preclude assuming an exclusive GPU. Requested
observation cadence was one second and actual intervals included counter-query
latency. Small timing differences must not be interpreted as a hardware-neutral
win or regression. These short RAM-only series showed no uniform downward
performance trend across all measured phases; they do not diagnose SSD cache
behavior.

## Root Cause And Reproduction

Previously every CRC combination squared its polynomial matrices anew.
Reusing fixed operators removes that work generically for chunk and segment
aggregation. Separately, full device diagnostics repeated a memory query before
allocation admission repeated it again. The readiness-only experiment reduced
that stage, but some waiting moved into classification. Thus stage reductions
must be assessed against whole-operation time, not added together as independent
speedups. Allocation/transfer/runtime serialization remains a candidate for
further profiling, not a proven kernel bottleneck.

Preserve the baseline executable and app-local DLLs before rebuilding. Invoke
both executables with identical arguments and alternate their order:

```text
memory-benchmark --size-mib 10240 --profile Mixed --require-gpu --workers 32 --block-size-kib 8192 --compression-level 5
```

Repeat with `Compressible` and `Incompressible`; collect complete raw statistics
and concurrent resource context. The ordinary CPU/GPU block-size check remains:

```powershell
tools/bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1 -BlockSizeKiB 256,512,1024,2048,4096,8192,16384
```

Use clean-source, repeated, resource-observed measurements before updating
public comparison graphs or release speed claims. This patch does not establish
a compression-ratio improvement, nine unique sizes on arbitrary data, faster
standard-format compression, or complete production/release validation.
