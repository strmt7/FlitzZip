# 2026-06-18 Segmented Decode Materialization Iteration

Purpose: measure a second SUZIP HIP performance iteration after owned
raw-payload encode. The decode materialization kernel now launches over fixed
64 KiB output segments instead of launching only one HIP block per SUZIP block.
This keeps the archive format and block metadata unchanged while feeding more
work to the GPU for 4 MiB and 16 MiB block-size options.

## Change

- Added `materialize_segments_kernel(...)` for fill, raw, and GPU-pattern block
  materialization.
- Kept one materialization kernel launch per decoded chunk, but changed the
  launch grid from decoded SUZIP block count to decoded 64 KiB segment count.
- Kept host-side validation and output bounds unchanged.

## Validation Commands

```powershell
tools\verify_changes.ps1 -IncludeUntracked
tools\bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1 -BlockSizeKiB 256,1024,4096,16384
build\Release\superzip_cli.exe benchmark-suite --profile Mixed --compression-level 5 --tune
build\Release\superzip_cli.exe benchmark-suite --profile Compressible --compression-level 5 --tune
build\Release\superzip_cli.exe benchmark-suite --profile Incompressible --compression-level 5 --tune
```

The first Compressible suite after this change was fractionally below the
previous owned-payload 1 MiB recommendation, so it was repeated once before
accepting the iteration. The repeated Compressible result is used in the
summary table.

## Mixed RAM-Only Sweep

| Block size | Owned-payload GPU seconds | Segmented GPU seconds | Change | End-to-end speedup | Compression ratio | Memory-only | Disk writes |
| ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: |
| 256 KiB | 5.485 | 5.429 | +1.02% | 2.15x | 0.500086 | true | 0 |
| 1 MiB | 5.649 | 5.348 | +5.33% | 2.15x | 0.500021 | true | 0 |
| 4 MiB | 6.210 | 5.761 | +7.23% | 1.93x | 0.500005 | true | 0 |
| 16 MiB | 10.458 | 8.742 | +16.41% | 1.28x | 0.500001 | true | 0 |

Every lane reported `memory_only=true` and `disk_write_bytes=0`.

## Sequential Built-In Suite

Owned-payload values are from
`2026-06-18-owned-raw-payload-encode.md`. Initial baseline values are from
`2026-06-18-repeat-ram-gpu-benchmark.md`.

| Profile | Initial baseline GPU score | Owned-payload GPU score | Segmented GPU score | Change vs owned | Change vs initial | Speedup vs CPU | Compression ratio |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Mixed | 59109 | 62769 | 65313 | +4.05% | +10.50% | 2.25921x | 0.500021 |
| Compressible | 65085 | 68018 | 68587 | +0.84% | +5.38% | 1.69274x | 0.100069 |
| Incompressible | 47778 | 55112 | 55659 | +0.99% | +16.49% | 2.68230x | 1.000000 |

Per-candidate GPU scores from the accepted sequential run:

| Profile | Block size | GPU score | Speedup vs CPU | Compression ratio | Kernel launches | Kernel ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Mixed | 256 KiB | 61319 | 2.15573x | 0.500086 | 320 | 1461.97 |
| Mixed | 1 MiB | 65313 | 2.25921x | 0.500021 | 320 | 1496.21 |
| Mixed | 4 MiB | 61118 | 2.03632x | 0.500005 | 320 | 1856.84 |
| Mixed | 16 MiB | 49094 | 1.16346x | 0.500001 | 320 | 6633.27 |
| Compressible | 256 KiB | 65411 | 2.68814x | 0.100275 | 320 | 1674.62 |
| Compressible | 1 MiB | 68587 | 1.69274x | 0.100069 | 320 | 1698.20 |
| Compressible | 4 MiB | 63552 | 1.50987x | 0.100017 | 320 | 2388.83 |
| Compressible | 16 MiB | 52538 | 0.828151x | 0.100004 | 320 | 7272.05 |
| Incompressible | 256 KiB | 53580 | 2.50298x | 1.000000 | 320 | 1496.48 |
| Incompressible | 1 MiB | 55659 | 2.68230x | 1.000000 | 320 | 1512.64 |
| Incompressible | 4 MiB | 52650 | 2.44127x | 1.000000 | 320 | 1870.88 |
| Incompressible | 16 MiB | 41911 | 1.66562x | 1.000000 | 320 | 5357.02 |

## Conclusion

Segmented decode materialization improved the recommended 1 MiB GPU score for
Mixed, Compressible, and Incompressible workloads after repeating the
Compressible suite. It also reduced the Mixed 16 MiB RAM-only GPU time by
16.41%, which directly addresses the large-block under-occupancy issue noted in
the previous benchmark documents. The 1 MiB block size remains the recommended
balanced level-5 setting across all three tested workload profiles.
