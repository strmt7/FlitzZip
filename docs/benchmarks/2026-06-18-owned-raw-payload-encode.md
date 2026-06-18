# 2026-06-18 Owned Raw-Payload Encode Iteration

Purpose: measure a production SUZIP HIP encode optimization that lets callers
with owned chunk memory move all-raw HIP payloads into the encoded chunk instead
of copying the 128 MiB host buffer after GPU classification. The optimization is
used by both the RAM-only benchmark path and the real SUZIP compression
pipeline.

## Change

- Added `encode_owned_chunk(...)` for callers that already own the source chunk.
- Added an owned HIP encode path that moves the source vector into
  `EncodedChunk::payload` only after HIP has successfully copied the bytes to
  VRAM, computed the source CRC, and classified every block as raw.
- Kept borrowed `encode_chunk(...)` behavior unchanged for callers that cannot
  transfer ownership.
- Preserved optional GPU fallback semantics. The owned buffer is not moved if a
  non-required HIP attempt throws and falls back to CPU.

## Validation Commands

```powershell
tools\verify_changes.ps1 -IncludeUntracked -Full
tools\bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1 -BlockSizeKiB 256,1024,4096,16384
build\Release\superzip_cli.exe benchmark-suite --profile Mixed --compression-level 5 --tune
build\Release\superzip_cli.exe benchmark-suite --profile Compressible --compression-level 5 --tune
build\Release\superzip_cli.exe benchmark-suite --profile Incompressible --compression-level 5 --tune
```

A first set of `benchmark-suite` commands was accidentally run concurrently and
was discarded as invalid performance evidence because the runs contended for the
same CPU and GPU. The table below uses only the sequential rerun.

## Host GPU

```text
hip_compiled=true
hip_runtime_loadable=true
hip_runtime_name=amdhip64_7.dll
device_name=AMD Radeon RX 9070 XT
gcn_arch=gfx1201
```

## Mixed RAM-Only Sweep

| Block size | CPU seconds | GPU seconds | End-to-end speedup | Compression ratio | GPU kernel launches | GPU kernel ms | H2D MiB | D2H MiB | Memory-only | Disk writes |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: |
| 256 KiB | 11.986 | 5.485 | 2.19x | 0.500086 | 320 | 1448.77 | 20483.63 | 10243.75 | true | 0 |
| 1 MiB | 11.331 | 5.649 | 2.01x | 0.500021 | 320 | 1481.54 | 20480.91 | 10242.81 | true | 0 |
| 4 MiB | 11.091 | 6.210 | 1.79x | 0.500005 | 320 | 2248.67 | 20480.23 | 10242.58 | true | 0 |
| 16 MiB | 10.972 | 10.458 | 1.05x | 0.500001 | 320 | 6920.94 | 20480.06 | 10242.52 | true | 0 |

Every lane reported `memory_only=true` and `disk_write_bytes=0`. Logical disk
counters were disabled by the memory-only benchmark mode.

## Sequential Built-In Suite

Baseline values are from `2026-06-18-repeat-ram-gpu-benchmark.md`, the latest
all-profile RAM-only benchmark before this codec change.

| Profile | Baseline recommended GPU score | New recommended GPU score | Change | New speedup vs CPU | Compression ratio | Recommended block size |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Mixed | 59109 | 62769 | +6.19% | 2.15277x | 0.500021 | 1 MiB |
| Compressible | 65085 | 68018 | +4.51% | 1.72813x | 0.100069 | 1 MiB |
| Incompressible | 47778 | 55112 | +15.35% | 2.64704x | 1.000000 | 1 MiB |

Per-candidate GPU scores:

| Profile | Block size | GPU score | Speedup vs CPU | Compression ratio | Kernel launches | Kernel ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Mixed | 256 KiB | 61498 | 2.10786x | 0.500086 | 320 | 1453.48 |
| Mixed | 1 MiB | 62769 | 2.15277x | 0.500021 | 320 | 1470.96 |
| Mixed | 4 MiB | 58570 | 1.90572x | 0.500005 | 320 | 2211.64 |
| Mixed | 16 MiB | 44306 | 1.09081x | 0.500001 | 320 | 6706.35 |
| Compressible | 256 KiB | 65097 | 2.69869x | 0.100275 | 320 | 1604.57 |
| Compressible | 1 MiB | 68018 | 1.72813x | 0.100069 | 320 | 1667.61 |
| Compressible | 4 MiB | 60796 | 1.47251x | 0.100017 | 320 | 2556.56 |
| Compressible | 16 MiB | 45932 | 0.754627x | 0.100004 | 320 | 7953.07 |
| Incompressible | 256 KiB | 52523 | 2.49268x | 1.000000 | 320 | 1517.80 |
| Incompressible | 1 MiB | 55112 | 2.64704x | 1.000000 | 320 | 1527.06 |
| Incompressible | 4 MiB | 50744 | 2.28255x | 1.000000 | 320 | 2278.14 |
| Incompressible | 16 MiB | 37376 | 1.42425x | 1.000000 | 320 | 6999.99 |

## Conclusion

The owned raw-payload encode path improved the recommended 1 MiB GPU score on
Mixed, Compressible, and Incompressible 10 GiB RAM-only workloads with unchanged
compression ratios and nonzero HIP proof fields. The 16 MiB option is still not
the recommended profile; its lower score remains consistent with the existing
kernel occupancy problem where only a small number of large SUZIP blocks feed
the per-block kernels.
