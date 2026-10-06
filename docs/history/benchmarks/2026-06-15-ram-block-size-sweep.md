# RAM-Only Block-Size Benchmark - 2026-06-15

This run validates the production SUZIP block-size options without writing the
10 GiB generated workload to storage.

This is a historical fastest-level run. Current release benchmarking uses
compression level 5 as the balanced baseline and reports compression ratio; see
`docs/compression-level-and-benchmark-suite.md`.

## Command

```powershell
tools\bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 1 -Iterations 1 -BlockSizeKiB 256,1024,4096,16384 -SampleIntervalMs 100
```

## Host And Runtime

| Field | Value |
| --- | --- |
| Workload | 10 GiB mixed, memory-only |
| HIP runtime | `amdhip64_7.dll` |
| GPU | AMD Radeon RX 9070 XT |
| GPU arch | `gfx1201` |
| Memory proof | `memory_only=true`, `disk_write_bytes=0` for every lane |

## Throughput

| Lane | Block size | Total seconds | Compress MiB/s | Speedup vs CPU |
| --- | ---: | ---: | ---: | ---: |
| CPU | 256 KiB | 12.60309 | 1208.98 | 1.00x |
| GPU | 256 KiB | 6.26321 | 3642.87 | 2.01x |
| CPU | 1 MiB | 9.57440 | 1570.29 | 1.00x |
| GPU | 1 MiB | 5.95908 | 3689.62 | 1.61x |
| CPU | 4 MiB | 9.08327 | 1606.88 | 1.00x |
| GPU | 4 MiB | 6.89499 | 3400.40 | 1.32x |
| CPU | 16 MiB | 9.00654 | 1613.28 | 1.00x |
| GPU | 16 MiB | 11.03000 | 2212.99 | 0.82x |

## GPU Proof Counters

| Block size | Encode chunks | Decode chunks | Kernel launches | Kernel ms | H2D MiB | D2H MiB | Device alloc MiB |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 KiB | 80 | 160 | 400 | 1616.23 | 20484.26 | 10243.75 | 40968.01 |
| 1 MiB | 80 | 160 | 400 | 1686.95 | 20481.06 | 10242.81 | 40963.88 |
| 4 MiB | 80 | 160 | 400 | 2705.20 | 20480.27 | 10242.58 | 40962.84 |
| 16 MiB | 80 | 160 | 400 | 7238.75 | 20480.07 | 10242.52 | 40962.59 |

## Interpretation

The required-AMD-HIP path is active for every block size. Backend telemetry
proves HIP kernel submission and device transfers even when Windows GPU engine
sampling under-reports short compute bursts.

The default 1 MiB block size remains justified for the current implementation:
it is the fastest GPU compression setting in this run and avoids the poor
end-to-end behavior of 16 MiB blocks. The 16 MiB option remains available for
experimentation but should be treated as an optimization target, not a better
default.
