# 2026-06-18 Repeat RAM-Only GPU Benchmark

Purpose: repeat the SUZIP GPU benchmark after external host load may have
affected earlier observations. All commands were run from `C:\SuperZip` against
the current Release build. The benchmark stayed in RAM-only mode; no generated
10 GiB workload was written to storage.

## Commands

```powershell
tools\bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1 -BlockSizeKiB 256,1024,4096,16384 -ShowOperationStats
build\Release\superzip_cli.exe benchmark-suite --profile Mixed --compression-level 5 --tune
build\Release\superzip_cli.exe benchmark-suite --profile Compressible --compression-level 5 --tune
build\Release\superzip_cli.exe benchmark-suite --profile Incompressible --compression-level 5 --tune
```

## Host GPU

```text
hip_compiled=true
hip_runtime_loadable=true
hip_runtime_name=amdhip64_7.dll
device_name=AMD Radeon RX 9070 XT
gcn_arch=gfx1201
```

## Mixed 10 GiB Sweep

| Block size | CPU seconds | GPU seconds | End-to-end speedup | Compression ratio | GPU kernel launches | GPU kernel ms | H2D MiB | D2H MiB | Memory-only | Disk writes |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: |
| 256 KiB | 12.364 | 6.202 | 1.99x | 0.500086 | 320 | 1487.22 | 20483.63 | 10243.75 | true | 0 |
| 1 MiB | 11.825 | 5.947 | 1.99x | 0.500021 | 320 | 1462.16 | 20480.91 | 10242.81 | true | 0 |
| 4 MiB | 11.396 | 6.638 | 1.72x | 0.500005 | 320 | 2146.97 | 20480.23 | 10242.58 | true | 0 |
| 16 MiB | 11.324 | 9.262 | 1.22x | 0.500001 | 320 | 5031.17 | 20480.06 | 10242.52 | true | 0 |

## Built-In Suite Results

| Profile | Recommended block size | GPU score | Speedup vs CPU | Compression ratio | GPU proof |
| --- | ---: | ---: | ---: | ---: | --- |
| Mixed | 1 MiB | 59109 | 2.067x | 0.500021 | 80 encode chunks, 160 decode chunks, 320 kernel launches |
| Compressible | 1 MiB | 65085 | 1.695x | 0.100069 | 80 encode chunks, 160 decode chunks, 320 kernel launches |
| Incompressible | 1 MiB | 47778 | 2.430x | 1.000000 | 80 encode chunks, 160 decode chunks, 320 kernel launches |

## Conclusion

The repeat run confirms that the current level-5 recommendation remains 1 MiB
blocks for Mixed, Compressible, and Incompressible workloads. The required-GPU
lane did not silently fall back to CPU: every GPU result reported nonzero HIP
kernel launches, HIP event time, host-to-device transfers, device-to-host
transfers, and device allocation bytes. Every lane reported `memory_only=true`
and `disk_write_bytes=0`.
