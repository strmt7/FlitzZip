# 2026-06-18 System Monitor UI And Mixed Baseline

Purpose: record the first benchmark baseline taken after the System page
performance-monitor graph polish. This iteration changed UI rendering only; it
did not change SUZIP codec scheduling, HIP kernels, block layout, or benchmark
logic. Any throughput movement from earlier runs is treated as host/runtime
variance unless a later codec change proves causality.

## Command

```powershell
build\Release\superzip_cli.exe benchmark-suite --profile Mixed --compression-level 5 --tune
```

The command uses the built-in RAM-only 10 GiB benchmark suite. No generated
workload was written to storage.

## Mixed 10 GiB Level-5 Results

| Block size | CPU score | GPU score | Speedup vs CPU | Compression ratio | GPU encode chunks | GPU decode chunks | GPU kernel launches | GPU kernel ms | Memory-only | Disk writes |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: |
| 256 KiB | 35113 | 55062 | 2.06051x | 0.500086 | 80 | 160 | 320 | 1434.90 | true | 0 |
| 1 MiB | 40239 | 59660 | 2.07026x | 0.500021 | 80 | 160 | 320 | 1503.22 | true | 0 |
| 4 MiB | 42709 | 54768 | 1.77435x | 0.500005 | 80 | 160 | 320 | 2279.84 | true | 0 |
| 16 MiB | 43615 | 42420 | 1.08945x | 0.500001 | 80 | 160 | 320 | 6683.26 | true | 0 |

## Recommendation

```text
suite_recommendation compression_level=5 block_size_kib=1024 gpu_score=59660 speedup_vs_cpu=2.07026 compression_ratio=0.500021 memory_only=true disk_write_bytes=0
```

The current Mixed recommendation remains 1 MiB blocks at compression level 5.
The required GPU path was active: every candidate reported nonzero encode
chunks, decode chunks, HIP kernel launches, and HIP event time. The benchmark
also confirmed `memory_only=true` and `disk_write_bytes=0`.

## Next Speed Work

The next codec iteration should profile why 4 MiB and 16 MiB candidates lose
GPU score despite fewer nominal block-size windows. The likely areas to inspect
are HIP kernel occupancy, host/device copy overlap, chunk queue depth, and
per-chunk temporary allocation behavior. Do not claim improvement until the
same RAM-only command shows a repeatable gain with matching compression ratio
and nonzero GPU proof fields.
