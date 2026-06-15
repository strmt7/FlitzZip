# Portability And Installation Model

SuperZip is portable across supported Windows hosts, not across GPU vendors.
The supported runtime boundary is Windows x64 with an AMD GPU and an AMD driver
that provides the HIP runtime required by the build.

## Product Packages

The portable ZIP and MSI are functionally identical:

- Same HIP-enabled `SuperZip.exe`.
- Same HIP-enabled `superzip_cli.exe`.
- Same security behavior, archive formats, and dependency manifest.
- Same requirement for an AMD driver-provided HIP runtime.

The portable ZIP avoids MSI registration and uninstall state. It does not avoid
or remove GPU requirements.

The MSI defaults to per-user installation for non-admin deployment and
automated silent smoke tests. Build with `tools\build.ps1 -MsiInstallScope
perMachine` only for managed per-machine rollout and validate that artifact from
an elevated/admin runner.

## Runtime Prerequisites

Runtime hosts need:

- Windows 11 x64.
- A supported AMD GPU.
- An AMD GPU driver that provides the HIP runtime DLL recorded in
  `superzip-runtime-dependencies.json`.

SuperZip delay-loads the HIP runtime so missing prerequisites are reported by
`superzip_cli.exe dependency-check` and the GUI AMD GPU page instead of a Windows
loader dialog.

## Build Discovery

Build scripts discover:

- CMake from common Visual Studio and system locations.
- ROCm/HIP SDK from `HIP_PATH`.
- `hipcc.exe` from `%HIP_PATH%\bin`.
- `amdhip64.lib` from `%HIP_PATH%\lib`.
- A compatible Visual Studio toolset through `vcvarsall.bat`.

If `tools\build.ps1 -VcvarsVersion` is empty, the HIP object compile helper
enumerates installed MSVC toolsets under the discovered Visual Studio instance,
prefers a HIP-compatible toolset when one is present, and falls back to the
Visual Studio default only when no preferred toolset is installed. Use an
explicit `-VcvarsVersion` for toolchain qualification or reproducing a compiler
issue; do not hardcode developer-machine paths.

The default HIP architecture is `gfx1201` for the current workstation. Use
`tools/build.ps1 -HipArch <gfx...>` for another supported target. Distribution
should either build per target architecture or add a validated fat-binary matrix
when the Windows HIP toolchain supports it cleanly.

## Dependency Checks

Use this command for automation and installer smoke tests:

```powershell
superzip_cli.exe dependency-check
```

Exit codes:

- `0`: HIP build, HIP runtime loadable, AMD GPU available.
- `10`: CPU-only validation build.
- `11`: HIP runtime missing or not loadable.
- `12`: HIP runtime loadable but no supported AMD GPU is available.

## Display Scaling

The GUI declares PerMonitorV2 DPI awareness in its manifest and sets the same
awareness context at startup. Fonts are recreated per monitor DPI, layout
coordinates use integer scaling, and painting is double-buffered. Progress
repaint requests are coalesced so fast jobs or high-refresh displays do not
accumulate redundant paint work.

## User Data

Future persistence should store preferences and history under the user's Windows
known application-data folder. It must not store build paths, secrets, or
developer-machine locations.

## Large Files

Archive operations are designed around bounded chunks:

- `.suzip` compression reads source files in configurable chunks.
- `.suzip` verification and extraction decode bounded block windows.
- ZIP compatibility delegates file I/O to miniz file streaming APIs.

Large-file benchmarks should be run before changing archive layout, block size,
or chunk grouping. The default benchmark path is memory-only so routine
performance work does not write tens of gigabytes to the NVMe device.
Performance claims must compare forced CPU and required AMD HIP runs on the
same generated data set:

```powershell
tools\gpu_proof.ps1 -Configuration Release
tools\storage_smoke.ps1 -Configuration Release
tools\bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 1 -Iterations 1
```

Record the workload size, iteration count, GPU model, driver/runtime version,
sampled CPU/GPU utilization, process I/O transfer bytes, backend HIP chunk
counts, HIP kernel launches, HIP event time, GPU transfer bytes, compression
level, and whether the system was thermally or power limited. Run mixed,
compressible, and incompressible profiles before making release performance
claims. Do not publish a GPU-only number as a product benchmark. Low GPU engine
utilization or a required-HIP lane slower than forced CPU is an engineering
optimization finding, not a number to hide.

The memory-only benchmark command must report `memory_only=true` and
`disk_write_bytes=0`. It exercises the codec, in-memory archive representation,
verification, and extraction paths without large filesystem writes. It does not
claim sustained storage throughput. The benchmark must also report
`CodecWorkers`, which is resolved with the same per-entry worker allocation rule
used by production archive operations. Do not introduce benchmark-only
parallelism that is absent from the product path. Failed optional HIP attempts
must not be counted as completed GPU encode/decode chunks, kernel launches,
transfer bytes, or device allocations in CPU-fallback telemetry. Use
`tools\storage_smoke.ps1`
for the small filesystem correctness path; it writes a bounded temporary
payload, verifies the archive, compares SHA-256 hashes after extraction, and
deletes the temporary data.

Full filesystem benchmarks remain available only for storage devices reserved
for benchmark wear:

```powershell
tools\bench.ps1 -Configuration Release -Mode Filesystem -AllowLargeDiskWrites -SizeMiB 10240 -Profile Mixed -CompressionLevel 1 -Iterations 1
```

When using the explicit filesystem mode, additionally record logical-disk
active time, disk read/write throughput, and process I/O transfer bytes.

Windows Task Manager and `\GPU Engine(*)\Utilization Percentage` can miss or
understate short AMD HIP bursts. Use `tools\gpu_proof.ps1` and the `gpu_*`
fields emitted by `superzip_cli` to prove whether the required-GPU path actually
submitted HIP kernels. A valid required-GPU proof must show nonzero
`gpu_kernel_launches`, `gpu_kernel_ms`, `gpu_h2d_bytes`, and
`gpu_device_allocation_bytes`; verification and extraction must also show
nonzero `gpu_d2h_bytes`.

Use `tools\gpu_diagnostic.ps1` when the question is whether the host can execute
sustained HIP work at all. That script runs a HIP-only kernel workload and
independently samples Windows GPU engine counters. It proves runtime execution;
it does not replace the forced-CPU versus required-HIP archive benchmark.
