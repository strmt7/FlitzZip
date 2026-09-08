# Small-File GPU Batching

## Production Contract

Native SUZIP creation groups consecutive nonempty regular files into one HIP
submission when at least two fit. Each source must fit the selected block size.
A group contains at most 64 files and at most the smaller of the selected chunk
size and 8 MiB. Directories, empty files, larger files, and forced-CPU creation
retain the existing path. Optional-HIP failure retains explicit CPU fallback;
required-HIP never silently uses CPU compression.

This is submission batching, not solid compression. Files do not share a
dictionary or a compressed block. No padding, format version, block kind, or
reader change is introduced. Each file retains an independent descriptor,
payload window, and CRC-32. The source locks remain held through batch writing;
the outer publication transaction still owns final archive replacement.

The codec API admits at most 256 independent blocks with positive lengths that
cover its bounded input exactly. Production uses the smaller 64-file limit.
The GPU checksum stage splits each file at 64 KiB CRC segment boundaries and
computes all segments in one launch. Prefix planners and pattern verification
use cumulative actual source offsets instead of multiplying by a uniform block
size. Raw owned input moves into the result only after fallible HIP cleanup.

## Correctness Evidence

The regression suite checks:

- CPU and HIP batch encoding against separate per-block encoding at levels 1-9,
  comparing descriptor fields, exact payload bytes, source CRCs, and combined CRC.
- Independent CPU and HIP decoding, CRC segment tails, misleading sampled
  patterns, fill, periodic, low-alphabet, shifted-alphabet, and random bytes.
- Every production block-size option from 256 KiB through 16 MiB, empty input,
  the 256-block limit, invalid coverage, and rejection before GPU dispatch.
- Production grouping limits and transitions across directories, empty files,
  and larger files; complete serialized archive identity with a separate-file
  reference writer at levels 5 and 9.
- Interrupted source reading, preservation of an existing output, staging
  cleanup, released source locks, final progress totals, and extracted bytes.

The September 8, 2026 local HIP build compiled all six release targets. All 383
native tests passed on the available gfx1201 device. Compilation of another GPU
target is not execution on that hardware. The optional benchmark test does no
timed work in ordinary test runs.

## Focused Measurements

The opt-in `gpu_block_batch_benchmark_opt_in` test compares the production batch
API with 64 calls to the existing independent owned-chunk API in the same build.
Both include copying the same immutable source bytes into owned codec inputs.
It warms both modes, then runs six alternating AB/BA pairs for each case.
The table reports the median of paired speedups, not a ratio of independently
rounded median times.

All cases use required HIP, 64 unequal files, a 256 KiB block setting,
`memory_only=true`, and `disk_write_bytes=0`. File lengths are 4096-31+i or
65536-31+i for i=0..63. Input totals are 262,176 and 4,194,336 bytes. Ratios are
input bytes divided by payload bytes; metadata is unchanged and is excluded
from this codec timing. All 72 pairs had identical encoded sizes and CRCs.

| Input Bytes | Profile | Level | Payload Bytes | Separate Median ms | Batch Median ms | Median Paired Gain |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 262176 | Mixed | 5 | 117720 | 108.40 | 2.58 | 35.14x |
| 262176 | Mixed | 9 | 104666 | 116.06 | 3.34 | 34.62x |
| 262176 | Random | 5 | 262176 | 108.20 | 2.20 | 48.16x |
| 262176 | Random | 9 | 262176 | 114.40 | 2.60 | 44.02x |
| 262176 | Shifted alphabet | 5 | 262176 | 109.49 | 2.00 | 54.96x |
| 262176 | Shifted alphabet | 9 | 190408 | 129.86 | 3.20 | 40.77x |
| 4194336 | Mixed | 5 | 2357345 | 1514.95 | 11.19 | 132.26x |
| 4194336 | Mixed | 9 | 1962275 | 1490.78 | 13.44 | 110.25x |
| 4194336 | Random | 5 | 4194336 | 1503.17 | 10.48 | 143.34x |
| 4194336 | Random | 9 | 4194336 | 1529.49 | 12.54 | 119.51x |
| 4194336 | Shifted alphabet | 5 | 4194336 | 1512.15 | 10.75 | 140.78x |
| 4194336 | Shifted alphabet | 9 | 1896176 | 1561.91 | 13.88 | 108.61x |

Batching reduced encode submissions from 64 to one, and kernel launches from
97-256 to 2-6 depending on content and effort. It does not claim reduced total
allocation bytes: independent CRC range metadata adds a small bounded amount.
Compression ratio is preserved, not improved by this scheduling change.

The monitor collected 71 one-second samples: mean CPU 9.82%, peak CPU 20.28%,
at least 19,710 MiB available RAM, mean disk busy 1.25%, and peak aggregated GPU
engine utilization 1.91%. A transient disk queue reached 12 and page reads
peaked at 51.67/s. No workload payload was read or written on disk during timing.
Other host tasks were neither stopped nor reconfigured. These observations do
not support treating small timing differences as significant.

HIP event durations were unavailable in 109 of 144 samples. The table therefore
uses steady-clock host wall time, not fabricated kernel times. The result is
specific to this host/runtime, small-file shapes, and codec submission scope.
It is not a measured 35-143x kernel-throughput gain, filesystem compression
speed, sustained large-file throughput, or a promise for another GPU/runtime.

To repeat, build the HIP test target, collect CPU/GPU/RAM/storage counters before
and throughout the run, then opt in only for this process environment:

```powershell
$env:SUPERZIP_BATCH_BENCHMARK = '1'
build/Release/superzip_tests.exe gpu_block_batch_benchmark_opt_in
Remove-Item Env:SUPERZIP_BATCH_BENCHMARK
```

The seven-block-size 10 GiB RAM sweep remains a separate validation lane; this
small-file experiment does not replace it or the final release gates.

The September 8 attempt completed the level-5 Mixed CPU/HIP operations at
256 KiB, including verification and extraction, with 10,737,418,240 input bytes
per lane and zero payload disk writes. Output sizes were 4,715,827,690 bytes
(CPU) and 4,571,380,884 bytes (HIP). The harness then stopped with
`GPU memory benchmark reported no AMD HIP event time in the required-GPU lane.`
The HIP operation recorded 360 launches but `gpu_kernel_ms=nan`. The remaining
six block sizes were not run by this sweep, and the full sweep is **not passed**.
The event-timing gate was not relaxed. Further sustained-throughput claims and
release sign-off require usable event timing and the outstanding sweep lanes.
