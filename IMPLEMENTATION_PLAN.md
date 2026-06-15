# SuperZip Enterprise Implementation Plan

## Direction

SuperZip is a native Windows x64 archive application written in modern C++20.
The GPU boundary is AMD HIP only. The native `.suzip` format is the
GPU-accelerated product path, while standard `.zip` support is compatibility
only through miniz 3.1.1. Uncompressed `.tar` support is compatibility only
through SuperZip's native bounded TAR adapter.

Portable ZIP and MSI packages must be functionally identical HIP-enabled
artifacts. CPU-only builds exist only so hosted CI can run static analysis and
core archive tests where AMD HIP is unavailable.

## Requirements

- AMD HIP is the only GPU acceleration method.
- No CUDA, WebGPU, DirectCompute, OpenCL, or cross-vendor fallback.
- Windows x64 only.
- Portable and MSI packages must both contain the same HIP-enabled executables.
- The app must delay-load the AMD HIP runtime and report missing prerequisites
  through `dependency-check`, not fail with a Windows loader dialog.
- The MSI and release workflow must validate that the installed artifact is
  HIP-enabled. They must not silently install or downgrade AMD GPU drivers.
- Archive work must be chunked and bounded in memory.
- Native archive chunks are hard-capped at 128 MiB, archive metadata counts are
  bounded, and HIP allocations preflight available VRAM before kernel work.
- Native `.suzip` blocks may be fill, raw, or bounded miniz-deflate payloads;
  the GPU acceleration boundary remains AMD HIP-only.
- Compatibility archives must use in-process parsers/writers. Product code must
  not shell out to host archive tools or silently route through untracked
  fallback utilities.
- Extraction must reject traversal, absolute paths, UNC paths, reserved Windows
  device names, malformed metadata, CRC mismatches, and accidental overwrites.
- Microsoft Defender scanning and SHA-256 hashing remain opt-in.
- No secrets, personal paths, local machine names, build artifacts, or release
  archives may be committed.

## Architecture

```text
src/
  app/        Native Win32 GUI and DPI-aware layout
  cli/        Command-line automation and dependency checks
  core/       Archive model, safety validation, integrity, progress
  gpu/        AMD HIP device discovery and GPU codec boundary
  tar/        TAR compatibility adapter with two-pass validation
  zip/        ZIP compatibility adapter using miniz 3.1.1
tests/       C++ regression tests
tools/       Build, package, benchmark, and security automation
third_party/ Patched production dependencies plus upstream provenance copies
```

The only layer allowed to call HIP is `src/gpu/`. Higher layers consume typed
archive options, progress snapshots, and errors.

## Iterations

### Iteration 1: Architecture

- Replaced prototype-level shell assumptions with a native C++/Win32 app.
- Preserved AMD HIP as the fundamental acceleration boundary.
- Separated native `.suzip` from `.zip` compatibility.

### Iteration 2: Responsiveness

- Added background archive work.
- Kept progress state explicit and sampled.
- Designed the GUI around PerMonitorV2 DPI and high-refresh repaint coalescing.

### Iteration 3: Security

- Added archive path validation and malicious-entry tests.
- Added optional Defender scanning and optional SHA-256 integrity checks.
- Added layered CI security scans and OpenVAS/Vulnetix integration lanes.

### Iteration 4: Packaging

- Made HIP the default local build.
- Added explicit CPU-only validation mode for hosted CI.
- Switched to the static MSVC runtime so packages do not need a VC redistributable.
- Delay-loaded the AMD HIP runtime and added `dependency-check`.
- Made portable packaging fail closed for CPU-only builds.
- Added release inputs for HIP SDK installer checksum verification and WiX v7
  EULA acknowledgement.

### Iteration 5: Vendored Dependency Hardening

- Preserved an unmodified miniz 3.1.1 upstream source archive under
  `third_party/upstream/miniz/3.1.1/`.
- Kept the patched production copy under `third_party/miniz/`.
- Removed static-analysis findings without changing ZIP compatibility behavior.

### Iteration 6: Benchmarking And Tuning

- Exposed native `.suzip` compression levels 1, 3, 5, 7, and 9 in the GUI and
  CLI, with level 5 as the balanced default.
- Added RAM-only compression-ratio reporting so CPU and GPU throughput is
  compared at equivalent compression strength.
- Added the built-in `benchmark-suite` command for numerical system scoring and
  production block-size autotuning without multi-GB SSD writes.
- Added refactoring governance and an audit helper so future cleanup is planned,
  measured, and behavior-preserving.

### Iteration 7: Real Archive Compatibility

- Added an archive-format registry and `formats`/`identify` CLI commands.
- Added native uncompressed TAR create/extract support with archive-wide path
  validation before any output is written.
- Kept document/package ZIP containers out of the user-facing format matrix.
- Updated GUI extraction to auto-route implemented SUZIP, ZIP, and TAR formats
  while reporting recognized unsupported formats explicitly.

## Validation Gates

For HIP-capable Windows hosts:

```powershell
tools\build.ps1 -Configuration Release
tools\test.ps1 -Configuration Release
build\Release\superzip_cli.exe dependency-check
tools\gpu_proof.ps1 -Configuration Release -SizeMiB 512
tools\package.ps1 -Configuration Release
tools\bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1
build\Release\superzip_cli.exe benchmark-suite --profile Mixed --compression-level 5 --tune
```

For hosted CI without HIP:

```powershell
tools\build.ps1 -Configuration Release -CpuOnlyValidation
tools\test.ps1 -Configuration Release
tools\security_scan.ps1
```

Before publishing, GitHub workflows must complete without skipped user-authored
jobs, deployments must remain absent, and open vulnerability alerts must be
triaged through real fixes or documented external governance constraints.
Release performance notes must include forced-CPU and required-HIP runs with
CPU, GPU, logical-disk active time, and disk throughput telemetry for mixed,
compressible, and incompressible workloads.
Required-HIP claims must also include backend `gpu_*` counters proving HIP
kernel launches, HIP event time, transfer bytes, and device allocation bytes.
