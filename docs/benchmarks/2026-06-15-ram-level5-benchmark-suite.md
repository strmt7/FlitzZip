# RAM-Only Level-5 Benchmark And Suite - 2026-06-15

This run validates the balanced compression baseline after adding the
compression-level dropdown and built-in benchmark suite. The generated 10 GiB
workload stayed in RAM; no multi-GB benchmark data was written to storage.

## Commands

```powershell
tools\bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1 -BlockSizeKiB 256,1024,4096,16384 -SampleIntervalMs 100
build\Release\superzip_cli.exe benchmark-suite --profile Mixed --compression-level 5 --tune
```

## Host And Runtime

| Field | Value |
| --- | --- |
| Workload | 10 GiB mixed, memory-only |
| Compression level | 5 |
| HIP runtime | `amdhip64_7.dll` |
| GPU | AMD Radeon RX 9070 XT |
| GPU arch | `gfx1201` |
| Memory proof | `memory_only=true`, `disk_write_bytes=0` for every lane |

## Throughput

| Lane | Block size | Total seconds | Compression ratio | Compress MiB/s | Speedup vs CPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| CPU | 256 KiB | 10.94202 | 0.500954 | 1,208.98 | 1.00x |
| GPU | 256 KiB | 5.74498 | 0.500086 | 3,642.87 | 1.90x |
| CPU | 1 MiB | 9.94474 | 0.500874 | 1,570.29 | 1.00x |
| GPU | 1 MiB | 5.91961 | 0.500021 | 3,689.62 | 1.68x |
| CPU | 4 MiB | 9.78961 | 0.500855 | 1,606.88 | 1.00x |
| GPU | 4 MiB | 7.03124 | 0.500005 | 3,400.40 | 1.39x |
| CPU | 16 MiB | 9.62842 | 0.500850 | 1,613.28 | 1.00x |
| GPU | 16 MiB | 13.07889 | 0.500001 | 2,212.99 | 0.74x |

## Benchmark-Suite Scores

| Block size | CPU score | GPU score | Speedup vs CPU | Compression ratio |
| ---: | ---: | ---: | ---: | ---: |
| 256 KiB | 41,010 | 55,916 | 1.67790x | 0.500086 |
| 1 MiB | 41,529 | 56,475 | 1.68109x | 0.500021 |
| 4 MiB | 42,427 | 45,965 | 1.41672x | 0.500005 |
| 16 MiB | 42,879 | 24,473 | 0.74877x | 0.500001 |

Suite recommendation:

```text
compression_level=5 block_size_kib=1024 gpu_score=56475 speedup_vs_cpu=1.68109 compression_ratio=0.500021 memory_only=true disk_write_bytes=0
```

## GPU Proof Counters

| Block size | Encode chunks | Decode chunks | Kernel launches | Kernel ms | H2D MiB | D2H MiB | Device alloc MiB |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 KiB | 80 | 160 | 400 | 1,523.68 | 20,484.26 | 10,243.75 | 40,968.01 |
| 1 MiB | 80 | 160 | 400 | 1,562.40 | 20,481.06 | 10,242.81 | 40,963.88 |
| 4 MiB | 80 | 160 | 400 | 2,741.42 | 20,480.27 | 10,242.58 | 40,962.84 |
| 16 MiB | 80 | 160 | 400 | 9,588.92 | 20,480.07 | 10,242.52 | 40,962.59 |

## Interpretation

The required-AMD-HIP path is active for every production block size. Windows GPU
engine sampling under-reports short HIP bursts on some runs, but backend
telemetry proves kernel submissions, HIP event time, host/device transfers, and
device allocations.

The built-in suite selected 1 MiB blocks at compression level 5. The 16 MiB
option remains slower end-to-end on this implementation and is an optimization
target, not a better default.
