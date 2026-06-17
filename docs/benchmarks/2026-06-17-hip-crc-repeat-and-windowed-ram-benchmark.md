# HIP CRC Repeat And Windowed RAM Benchmark - 2026-06-17

This run repeats the 10 GiB GPU benchmark after the host was no longer running a
game, validates the bounded RAM-only benchmark pipeline, and records the HIP CRC
performance iteration accepted for production.

## Commands

```powershell
build\Release\superzip_cli.exe memory-benchmark --size-mib 10240 --profile Mixed --compression-level 5 --block-size-kib 1024 --require-gpu
tools\bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1 -BlockSizeKiB 256,1024,4096,16384 -SkipCpu -ShowOperationStats
tools\bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1 -BlockSizeKiB 256,1024,4096,16384 -ShowOperationStats
```

## Host And Runtime

| Field | Value |
| --- | --- |
| Workload | 10 GiB mixed, memory-only |
| Compression level | 5 |
| HIP runtime | `amdhip64_7.dll` |
| GPU | AMD Radeon RX 9070 XT |
| GPU arch | `gfx1201` |
| Memory proof | `memory_only=true`, `disk_write_bytes=0` for every CPU and GPU lane |

## Accepted Production Change

The memory benchmark now processes the virtual 10 GiB workload in bounded
windows instead of retaining the whole synthetic archive at once. This keeps the
same deterministic workload while respecting the 80% host-RAM policy on systems
that do not have enough free memory for a full retained 10 GiB archive plus
working buffers.

The HIP verify path now computes decoded-stream CRC directly from encoded block
metadata on the GPU. This removes the verification-only decoded output
allocation and one materialization launch per verified chunk. The first direct
CRC attempt searched block metadata per byte and was rejected after measurement.
The accepted version resolves the block once per CRC segment and keeps the
per-byte loop branch-local inside the resolved block.

## Iteration Evidence

| Iteration | 1 MiB GPU total seconds | Verify MiB/s | Kernel launches | Device alloc MiB | Status |
| --- | ---: | ---: | ---: | ---: | --- |
| Pre-change repeated baseline | 6.20107 | 8,239.78 | 400 | 40,963.88 | Historical comparison |
| Direct CRC, per-byte block search | 57.36770 | 194.92 | 320 | 30,723.88 | Rejected |
| Direct CRC, segment block search | 7.14027 | 4,645.59 | 320 | 30,723.88 | Rejected |
| Accepted direct CRC | 5.76300 | 10,956.00 | 320 | 30,723.88 | Kept |

Accepted 1 MiB focused improvement versus the clean repeated GPU baseline:
`(6.20107 - 5.76300) / 6.20107 = 7.06%` end-to-end.

## Final CPU/GPU Sweep

| Lane | Block size | Total seconds | Compression ratio | Compress MiB/s | Verify MiB/s | Extract MiB/s | Speedup vs CPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| CPU | 256 KiB | 11.72711 | 0.500954 | 1,179.01 | 6,625.10 | 6,843.93 | 1.00x |
| GPU | 256 KiB | 5.77769 | 0.500086 | 3,726.08 | 10,960.00 | 4,887.37 | 2.03x |
| CPU | 1 MiB | 11.62706 | 0.500874 | 1,197.11 | 6,711.13 | 6,618.03 | 1.00x |
| GPU | 1 MiB | 5.85777 | 0.500021 | 3,733.68 | 10,660.70 | 4,752.55 | 1.98x |
| CPU | 4 MiB | 11.27908 | 0.500855 | 1,219.70 | 7,041.46 | 7,164.19 | 1.00x |
| GPU | 4 MiB | 6.42384 | 0.500005 | 3,293.10 | 10,923.60 | 4,308.15 | 1.76x |
| CPU | 16 MiB | 11.30781 | 0.500850 | 1,214.01 | 7,239.33 | 7,021.23 | 1.00x |
| GPU | 16 MiB | 9.11383 | 0.500001 | 2,255.37 | 10,944.40 | 2,814.80 | 1.24x |

## GPU Proof Counters

| Block size | Encode chunks | Decode chunks | Kernel launches | Kernel ms | H2D MiB | D2H MiB | Device alloc MiB |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 KiB | 80 | 160 | 320 | 1,348.76 | 20,484.26 | 10,243.75 | 30,728.01 |
| 1 MiB | 80 | 160 | 320 | 1,367.77 | 20,481.06 | 10,242.81 | 30,723.88 |
| 4 MiB | 80 | 160 | 320 | 2,072.86 | 20,480.27 | 10,242.58 | 30,722.84 |
| 16 MiB | 80 | 160 | 320 | 4,919.20 | 20,480.07 | 10,242.52 | 30,722.59 |

## Interpretation

The required-AMD-HIP path was active for every GPU lane. The proof of GPU work
is the SuperZip backend telemetry: HIP kernel launches, HIP event kernel time,
host-to-device bytes, device-to-host bytes, and device allocations. Windows GPU
engine sampling can under-report short HIP bursts, so it is useful for trends
but not sufficient by itself to prove or disprove HIP execution.

The accepted CRC change reduces GPU verification allocations by about 10 GiB
over this sweep and removes 80 kernel launches versus the repeated baseline.
For this host and workload, 256 KiB and 1 MiB remain the strongest production
block-size candidates at compression level 5.
