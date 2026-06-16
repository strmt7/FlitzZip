![SuperZip logo](resources/brand/superzip-logo.svg)

# SuperZip

SuperZip is a Windows x64 archive application built around AMD HIP acceleration.
Its native `.suzip` format is the GPU-first path. Standard `.zip` support exists
for compatibility and is handled by the vendored miniz 3.1.1 codebase.
Uncompressed `.tar` support is implemented by a native bounded adapter with the
same extraction path-safety checks. Single-file `.gz` streams are implemented
through miniz raw deflate with CRC32/ISIZE verification. Other common archive
formats are recognized for clear diagnostics and are tracked in
`docs/archive-format-support.md`.

The product ships as two equivalent Windows packages:

- A portable ZIP for controlled environments where users do not want an MSI.
- An MSI installer for managed installation, uninstall, and enterprise rollout.

Both release artifacts are built from the same HIP-enabled binaries. A portable
package is not a reduced CPU-only build.

## Requirements

Runtime systems need:

- Windows 11 x64.
- A supported AMD GPU.
- An AMD GPU driver that provides the HIP runtime required by the build.

AMD documents the HIP runtime as part of the AMD GPU driver, not something an
application should redistribute. SuperZip therefore delay-loads the HIP runtime,
reports missing prerequisites clearly, and records the expected runtime DLL in
`superzip-runtime-dependencies.json` inside each HIP-enabled package.

Development systems additionally need:

- Visual Studio C++ build tools.
- CMake.
- AMD HIP SDK for Windows with `HIP_PATH` pointing at the SDK root.

## Build

The normal local build is HIP-enabled:

```powershell
tools/build.ps1 -Configuration Release -HipArch gfx1201
tools/test.ps1 -Configuration Release
build/Release/superzip_cli.exe dependency-check
```

The build script defaults to HIP. Use `-CpuOnlyValidation` only for hosted CI or
static-analysis jobs that cannot install the AMD HIP SDK:

```powershell
tools/build.ps1 -Configuration Release -CpuOnlyValidation
tools/test.ps1 -Configuration Release
```

CPU-only validation artifacts must not be published as SuperZip product
releases.

When building HIP objects on Windows, `tools/build.ps1` leaves
`-VcvarsVersion` empty by default. The HIP compile helper then discovers the
Visual Studio C++ toolsets installed on the host and prefers a compatible MSVC
toolset for HIP before falling back to Visual Studio's default environment.
Pass `-VcvarsVersion <major.minor>` only when validating a specific toolset.

## Package

Package the current HIP build:

```powershell
tools/package.ps1 -Configuration Release
```

The package script refuses to package a CPU-only build unless
`-AllowCpuValidationPackage` is passed explicitly for internal validation. For
MSI packaging, install the pinned repo-local WiX tool and set
`SUPERZIP_ACCEPT_WIX_OSMF_EULA=wix7` only after accepting the WiX v7 OSMF EULA:

```powershell
tools/install_wix.ps1
$env:SUPERZIP_ACCEPT_WIX_OSMF_EULA = "wix7"
tools/package.ps1 -Configuration Release -CreateMsi
```

The MSI build defaults to the release deployment scope: per-machine install
under `C:\Program Files\SuperZip`. Like normal Windows desktop software, that
installer requires elevation when installed by a non-admin user.
The installer offers `Create Desktop shortcut` as an optional MSI feature rather
than silently creating a desktop shortcut.

For local coding and non-admin installer tests only, build an explicit per-user
MSI:

```powershell
tools/build.ps1 -Configuration Release -MsiInstallScope perUser
tools/package.ps1 -Configuration Release -CreateMsi
```

Do not publish a per-user MSI as a product release.

## CLI

```powershell
build/Release/superzip_cli.exe dependency-check
build/Release/superzip_cli.exe gpu-info
build/Release/superzip_cli.exe formats
build/Release/superzip_cli.exe identify archive.tar
build/Release/superzip_cli.exe benchmark-suite --profile Mixed --compression-level 5 --tune
build/Release/superzip_cli.exe compress --format suzip --require-gpu --output archive.suzip path\to\folder
build/Release/superzip_cli.exe compress --format suzip --require-gpu --verify-after-write --output archive.suzip path\to\folder
build/Release/superzip_cli.exe extract --format suzip --require-gpu --output restored archive.suzip
build/Release/superzip_cli.exe compress --format zip --output archive.zip path\to\folder
build/Release/superzip_cli.exe extract --format zip --output restored archive.zip
build/Release/superzip_cli.exe compress --format tar --output archive.tar path\to\folder
build/Release/superzip_cli.exe extract --output restored archive.tar
build/Release/superzip_cli.exe compress --format gz --output file.txt.gz file.txt
build/Release/superzip_cli.exe extract --output restored file.txt.gz
build/Release/superzip_cli.exe verify --sha256 archive.suzip
```

Use `--require-gpu` for `.suzip` operations that must fail instead of falling
back to the CPU validation path. In required-GPU mode, native `.suzip` data must
be encoded with HIP-supported block kinds; archives that require CPU deflate are
rejected instead of being partially decoded by miniz. Extraction refuses to
overwrite by default. Use `--force-cpu` only for diagnostics and CPU/GPU
benchmarks on a HIP-enabled build. Optional `--verify-after-write`, `--sha256`,
and `--defender-scan` flags add post-write archive validation, integrity
hashing, and Microsoft Defender checks without making those extra passes
implicit.
ZIP, TAR, and Gzip compatibility are deliberately separate from SUZIP tuning.
`--require-gpu`, `--force-cpu`, worker controls, block-size controls,
compression-level controls, and `--verify-after-write` are accepted only on
native `.suzip` commands because compatibility formats do not use the AMD HIP
SUZIP codec. `extract` defaults to auto-detection for implemented archive
formats. Recognized but unsupported formats fail explicitly instead of using
external tools or hidden fallbacks.

## GUI

The Win32 GUI includes Queue, Compress, Extract, Security Review, History, AMD
GPU, Preferences, and About pages. It is PerMonitorV2 DPI-aware, double
buffered, and uses native DPI fonts so it remains crisp on high-refresh and
high-resolution displays.

## Security

SuperZip treats archive contents as untrusted input. It rejects unsafe
extraction paths, malformed metadata, CRC mismatches, and accidental overwrites
by default. Microsoft Defender scanning and SHA-256 integrity hashing are
available as opt-in checks.

GitHub Actions run build/test validation, CodeQL, Trivy, Semgrep, DevSkim, OSV,
Dependency Review, default-branch OSSF Scorecard, workflow linting, secret
scanning, SBOM generation, ClusterFuzzLite parser fuzzing, and a
Greenbone/OpenVAS integration audit. The live OpenVAS/Vulnetix scan is
scheduled/manual and requires
an OIDC-backed scanner configuration broker so private scanner credentials are
never bound directly in workflow YAML.

## Fuzzing

Security-sensitive parsers are fuzzed with ClusterFuzzLite. The integration
builds libFuzzer targets for SuperZip archive-index metadata and archive-entry
path canonicalization with address and undefined-behavior sanitizers:

```powershell
tools/fuzz.ps1 -Runs 512
```

The GitHub workflow runs automatically on pull requests, pushes to `main`, a
weekly schedule, and manual dispatch.

## Benchmarking

For a direct correctness proof that `--require-gpu` is not falling back to CPU,
run:

```powershell
tools/gpu_proof.ps1 -Configuration Release
```

This proof generates a tiny temporary filesystem workload, runs compress, compress with
`--verify-after-write`, verify, and extract with `--require-gpu`, then fails
unless backend HIP telemetry reports kernel launches, HIP event time, transfer
bytes, device allocations, and matching restored file hashes.

For a HIP-runtime-only stress test that isolates the AMD device from archive
format and codec policy, run:

```powershell
tools/gpu_diagnostic.ps1 -Configuration Release -Seconds 8 -BufferMiB 256 -InnerIterations 2048 -SampleIntervalMs 50
```

Use this diagnostic to distinguish a broken HIP/runtime path from an archive
codec optimization issue. A valid archive benchmark still has to use
`--require-gpu` or `tools/bench.ps1`; the diagnostic is not a compression
throughput result.

`tools/storage_smoke.ps1` is the only normal filesystem smoke for the archive
write/read path. It defaults to an 8 MiB bounded workload and deletes the
temporary data after SHA-256 comparison:

```powershell
tools/storage_smoke.ps1 -Configuration Release
```

`tools/bench.ps1` is memory-only by default. It generates deterministic chunks
inside `superzip_cli.exe`, stores the encoded archive representation in process
RAM, verifies/extracts from RAM, and reports `memory_only=true` plus
`disk_write_bytes=0` from both the forced-CPU and required-GPU benchmark lanes:

```powershell
tools/bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1 -BlockSizeKiB 256,1024,4096,16384
```

Use `-Profile Mixed`, `-Profile Compressible`, and `-Profile Incompressible`
when characterizing a release candidate. The script refuses workloads smaller
than 10 GiB in memory mode and sweeps the production block-size choices:
256 KiB, 1 MiB, 4 MiB, and 16 MiB. It reports production-aligned worker
allocation (`Workers`, `InflightChunks`, and `CodecWorkers`) plus backend HIP
event counters, kernel time, transfer bytes, and allocation bytes emitted by the
SuperZip GPU backend itself. Benchmark records include compression ratio so CPU
and GPU lanes are compared at the same compression level, with level 5 as the
standard balanced baseline. Do not treat a GPU-only timing as a product
benchmark; if required HIP is slower than forced CPU, record it as an
optimization finding. Multi-GB filesystem benchmarks are intentionally blocked
during development to avoid SSD wear. Filesystem mode is limited to a bounded
smoke payload of at most 64 MiB; use `tools/storage_smoke.ps1` for the normal
archive write/read path.

The built-in CLI benchmark suite gives a numerical system score and can tune
the production block size while staying RAM-only:

```powershell
build/Release/superzip_cli.exe benchmark-suite --profile Mixed --compression-level 5 --tune
```

See `docs/security.md`, `docs/portability.md`, `docs/design.md`,
`docs/compression-backend-evaluation.md`,
`docs/performance-block-size-validation.md`,
`docs/compression-level-and-benchmark-suite.md`,
`docs/refactoring-governance.md`,
`docs/benchmarks/2026-06-15-ram-block-size-sweep.md`,
`docs/benchmarks/2026-06-15-ram-level5-benchmark-suite.md`,
`docs/third-party.md`, `docs/release.md`, and
`docs/security-code-scanning.md`.
