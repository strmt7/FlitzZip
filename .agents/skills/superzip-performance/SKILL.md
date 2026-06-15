---
name: superzip-performance
description: Validate SuperZip CPU/GPU performance, RAM-only benchmarks, compression-level/block-size tuning, and AMD HIP telemetry without causing SSD wear. Use when changing benchmark scripts, compression levels, compression block sizes, worker allocation, GPU utilization, performance docs, or performance claims.
---

# SuperZip Performance Skill

Read `docs/performance-block-size-validation.md` and
`docs/compression-level-and-benchmark-suite.md` before editing performance code,
benchmark scripts, compression-level UI, block-size UI, or benchmark
documentation.

Required rules:

- CPU/GPU comparison benchmarks must use `tools/bench.ps1` in default
  memory-only mode.
- The benchmark must report `memory_only=true` and `disk_write_bytes=0` for
  every forced-CPU and required-AMD-HIP lane.
- Sweep all production block sizes when validating performance:
  `-BlockSizeKiB 256,1024,4096,16384`.
- Compare CPU and GPU at the same compression level and report compression
  ratio. Level 5 is the standard balanced release baseline.
- Do not run or reintroduce multi-GB filesystem benchmarks during development.
  Use `tools/storage_smoke.ps1` or the 64 MiB-capped filesystem smoke only for
  archive write/read correctness.
- Required-GPU benchmark lanes must fail if HIP is unavailable. Do not count CPU
  fallback telemetry as GPU work.
- Propagate verified benchmark improvements into production code paths. Do not
  keep benchmark-only optimizations.

Standard command after correctness tests pass:

```powershell
tools/bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1 -BlockSizeKiB 256,1024,4096,16384
build/Release/superzip_cli.exe benchmark-suite --profile Mixed --compression-level 5 --tune
```

Record CPU/GPU throughput, worker counts, HIP chunk counts, HIP kernel launches,
HIP event time, transfer bytes, allocation bytes, compression ratio, and
benchmark score for every block size.
