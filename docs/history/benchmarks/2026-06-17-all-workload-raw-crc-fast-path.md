# All-Workload Raw CRC Fast Path - 2026-06-17

This run extends the level-5 RAM-only benchmark evidence beyond the mixed
workload. It measures Mixed, Compressible, and Incompressible profiles after the
decoded-stream CRC work, rejects one metadata lookup experiment, and accepts the
raw-payload CRC shortcut that improves the recommended 1 MiB candidate on every
profile.

All commands used the 10 GiB memory-only benchmark suite:

```powershell
build\Release\superzip_cli.exe benchmark-suite --profile Mixed --compression-level 5 --tune
build\Release\superzip_cli.exe benchmark-suite --profile Compressible --compression-level 5 --tune
build\Release\superzip_cli.exe benchmark-suite --profile Incompressible --compression-level 5 --tune
```

Every recorded lane reported `memory_only=true`, `disk_write_bytes=0`, nonzero
HIP kernel launches, nonzero HIP event time, and nonzero host/device transfers.

## Iteration Summary

| Iteration | Decision | Reason |
| --- | --- | --- |
| Linear decoded-block lookup | Baseline | Accepted direct decoded CRC from the previous pass. |
| Binary decoded-block lookup | Rejected | Helped some Mixed/Compressible points but regressed Incompressible and the 1 MiB recommendation for two profiles. |
| Raw-payload CRC shortcut | Accepted | Uses the existing contiguous GPU CRC kernel only when every raw block maps byte-for-byte to the decoded stream. Mixed, Compressible, and Incompressible 1 MiB scores all improved. |

## Recommended 1 MiB Candidate

| Profile | Baseline GPU score | Raw fast-path GPU score | Change | Raw fast-path speedup vs CPU | Compression ratio |
| --- | ---: | ---: | ---: | ---: | ---: |
| Mixed | 59,696 | 61,107 | +2.36% | 2.14559x | 0.500021 |
| Compressible | 65,129 | 65,958 | +1.27% | 1.70129x | 0.100069 |
| Incompressible | 47,648 | 49,515 | +3.92% | 2.52398x | 1.000000 |

## Raw Fast-Path Sweep

| Profile | Block size | GPU score | Speedup vs CPU | Compression ratio | Kernel launches | Kernel ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Mixed | 256 KiB | 60,186 | 2.11497x | 0.500086 | 320 | 1,400.96 |
| Mixed | 1 MiB | 61,107 | 2.14559x | 0.500021 | 320 | 1,441.77 |
| Mixed | 4 MiB | 56,806 | 1.83587x | 0.500005 | 320 | 2,247.85 |
| Mixed | 16 MiB | 47,446 | 1.27842x | 0.500001 | 320 | 5,118.42 |
| Compressible | 256 KiB | 64,772 | 2.78890x | 0.100275 | 320 | 1,571.28 |
| Compressible | 1 MiB | 65,958 | 1.70129x | 0.100069 | 320 | 1,637.53 |
| Compressible | 4 MiB | 61,389 | 1.52942x | 0.100017 | 320 | 2,519.74 |
| Compressible | 16 MiB | 49,804 | 0.97262x | 0.100004 | 320 | 5,702.07 |
| Incompressible | 256 KiB | 48,612 | 2.35052x | 1.000000 | 320 | 1,496.40 |
| Incompressible | 1 MiB | 49,515 | 2.52398x | 1.000000 | 320 | 1,452.47 |
| Incompressible | 4 MiB | 46,667 | 2.17819x | 1.000000 | 320 | 2,188.57 |
| Incompressible | 16 MiB | 38,875 | 1.56814x | 1.000000 | 320 | 5,243.86 |

## Required Mixed `bench.ps1` Sweep

The change-aware verifier selected the standard Mixed CPU/GPU sweep after the
production HIP change. This run also reported `memory_only=true` and
`disk_write_bytes=0` for every lane.

| Lane | Block size | Total seconds | Compression ratio | Compress MiB/s | Verify MiB/s | Extract MiB/s | Speedup vs CPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| CPU | 256 KiB | 11.74029 | 0.500954 | 1,166.26 | 6,972.75 | 6,865.38 | 1.00x |
| GPU | 256 KiB | 5.68967 | 0.500086 | 3,724.06 | 10,580.60 | 5,192.24 | 2.06x |
| CPU | 1 MiB | 11.76128 | 0.500874 | 1,186.38 | 6,549.68 | 6,536.76 | 1.00x |
| GPU | 1 MiB | 5.72784 | 0.500021 | 3,717.69 | 10,698.10 | 5,078.71 | 2.05x |
| CPU | 4 MiB | 11.42571 | 0.500855 | 1,204.54 | 6,920.30 | 7,087.41 | 1.00x |
| GPU | 4 MiB | 6.50060 | 0.500005 | 3,248.25 | 10,862.80 | 4,256.96 | 1.76x |
| CPU | 16 MiB | 11.42676 | 0.500850 | 1,199.44 | 7,238.54 | 6,943.29 | 1.00x |
| GPU | 16 MiB | 9.06011 | 0.500001 | 2,293.43 | 10,954.30 | 2,797.51 | 1.26x |

## Production Interpretation

The raw-payload shortcut is deliberately narrow. It is used only after normal
decode-layout validation proves that each raw block is contiguous and
byte-identical to the decoded stream. Mixed chunks that contain fills or pattern
blocks keep the generic decoded-stream CRC path, preserving the same archive
semantics and failure behavior.

The 1 MiB block size remains the recommendation for all three measured
workloads at compression level 5.
