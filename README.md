<p align="center">
  <img src="resources/brand/superzip-logo.svg" alt="SuperZip logo" width="760">
</p>

[![License: AGPL-3.0](https://img.shields.io/badge/license-AGPL--3.0-blue.svg)](LICENSE)
[![Windows CI](https://img.shields.io/github/actions/workflow/status/strmt7/SuperZip/windows-ci.yml?branch=main&label=windows-ci)](https://github.com/strmt7/SuperZip/actions/workflows/windows-ci.yml)
[![Security](https://img.shields.io/github/actions/workflow/status/strmt7/SuperZip/security-code-scanning.yml?branch=main&label=security)](https://github.com/strmt7/SuperZip/actions/workflows/security-code-scanning.yml)
[![Lint](https://img.shields.io/github/actions/workflow/status/strmt7/SuperZip/lint.yml?branch=main&label=lint)](https://github.com/strmt7/SuperZip/actions/workflows/lint.yml)
[![Fuzzing](https://img.shields.io/github/actions/workflow/status/strmt7/SuperZip/fuzzing.yml?branch=main&label=fuzzing)](https://github.com/strmt7/SuperZip/actions/workflows/fuzzing.yml)
[![Scorecard](https://img.shields.io/github/actions/workflow/status/strmt7/SuperZip/scorecard.yml?branch=main&label=scorecard)](https://github.com/strmt7/SuperZip/actions/workflows/scorecard.yml)
[![Release](https://img.shields.io/github/actions/workflow/status/strmt7/SuperZip/release.yml?branch=main&label=release)](https://github.com/strmt7/SuperZip/actions/workflows/release.yml)
[![Commit activity](https://img.shields.io/github/commit-activity/m/strmt7/SuperZip)](https://github.com/strmt7/SuperZip/commits/main)

# SuperZip

SuperZip is a Windows archive app with a native `.suzip` format accelerated by
AMD HIP. It also creates and extracts common archive formats, offers archive
verification and guarded extraction, and provides both a graphical app and a CLI.

**Beta:** SuperZip is prerelease software. Keep original files and verify
archives before relying on them for long-term storage. The latest published
build may be older than the source on `main`.

[Download a release](https://github.com/strmt7/SuperZip/releases) as a portable
ZIP or an MSI installer. Both packages contain the same HIP-enabled binaries;
the portable edition is not a reduced CPU-only build. Check the requirements
below before installing.

| Task | Formats |
| --- | --- |
| Native GPU-accelerated archiving | `.suzip` |
| Common archive creation and extraction | ZIP, TAR and supported compressed TAR variants, CPIO, AR, Gzip, Bzip2, Zstandard, and other documented formats |
| Extraction of additional formats | Supported ZIPX methods, 7z, CAB, ISO, RPM, WIM, and other documented subsets |

The [format support matrix](docs/archive-format-support.md) gives exact
create/extract capabilities and method limits. `.suzip` is a distinct versioned
format, not a renamed ZIP file; its [format specification](docs/native-suzip-format.md)
documents compatibility and verification. Unsupported methods fail explicitly.

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

The default CMake generator is Visual Studio 2022. On a fresh build directory,
VS 2026 can be selected with `-Generator "Visual Studio 18 2026"` and CMake
4.2 or newer. Do not change generators inside an existing CMake build tree.
The hosted CPU-only workflow defines test lanes for Windows Server 2022 with VS 2022 and Windows Server
2025 with VS 2026. These checks do not establish AMD GPU runtime support on
Windows Server or hardware that was not exercised. The release runtime target
remains Windows 11 x64 with the build's supported AMD HIP configuration.

References: [CMake VS 2026 generator](https://cmake.org/cmake/help/latest/generator/Visual%20Studio%2018%202026.html)
and [GitHub runner images](https://github.com/actions/runner-images).

The normal local build is HIP-enabled:

```powershell
tools/build.ps1 -Configuration Release -HipArch gfx1201
tools/test.ps1 -Configuration Release
build/Release/superzip_cli.exe dependency-check
```

Single-target development builds remain supported. For the multi-target
release configuration, use `-HipArch release`; a custom subset can be passed
as a quoted list such as `-HipArch "gfx1100,gfx1201"`. The release preset
contains `gfx1100`, `gfx1101`, `gfx1102`, `gfx1151`, `gfx1200`, and `gfx1201`.
The compiler emits a device image for each target into the same Windows x64
binary. Compilation coverage is not runtime validation on every GPU; see
[release validation](docs/release.md#gpu-targets).

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
Windows owns the UAC consent prompt, so the MSI cannot shorten an unanswered
admin-rights prompt from inside the package. SuperZip-owned installer launch and
release validation paths use bounded waits instead: MSI install and uninstall
smoke phases time out after 300 seconds instead of waiting indefinitely.
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
build/Release/superzip_cli.exe extract --format zipx --output restored archive.zipx
build/Release/superzip_cli.exe compress --format tar --output archive.tar path\to\folder
build/Release/superzip_cli.exe extract --output restored archive.tar
build/Release/superzip_cli.exe compress --format tar.gz --output archive.tar.gz path\to\folder
build/Release/superzip_cli.exe extract --output restored archive.tar.gz
build/Release/superzip_cli.exe compress --format tar.bz2 --output archive.tar.bz2 path\to\folder
build/Release/superzip_cli.exe extract --output restored archive.tar.bz2
build/Release/superzip_cli.exe extract --output restored archive.tar.xz
build/Release/superzip_cli.exe compress --format tar.zst --output archive.tar.zst path\to\folder
build/Release/superzip_cli.exe extract --output restored archive.tar.zst
build/Release/superzip_cli.exe compress --format gz --output file.txt.gz file.txt
build/Release/superzip_cli.exe extract --output restored file.txt.gz
build/Release/superzip_cli.exe compress --format bz2 --output file.txt.bz2 file.txt
build/Release/superzip_cli.exe extract --output restored file.txt.bz2
build/Release/superzip_cli.exe extract --output restored file.txt.xz
build/Release/superzip_cli.exe extract --output restored file.txt.lzma
build/Release/superzip_cli.exe compress --format zst --output file.txt.zst file.txt
build/Release/superzip_cli.exe extract --output restored file.txt.zst
build/Release/superzip_cli.exe compress --format z --output file.txt.Z file.txt
build/Release/superzip_cli.exe extract --output restored file.txt.Z
build/Release/superzip_cli.exe extract --output restored file.txt.b64
build/Release/superzip_cli.exe extract --output restored file.txt.hqx
build/Release/superzip_cli.exe extract --format macbinary --output restored file.macbin
build/Release/superzip_cli.exe extract --output restored file.txt.xxe
build/Release/superzip_cli.exe extract --output restored file.txt.uue
build/Release/superzip_cli.exe compress --format cpio --output archive.cpio path\to\folder
build/Release/superzip_cli.exe extract --output restored archive.cpio
build/Release/superzip_cli.exe compress --format cpio.gz --output archive.cpgz path\to\folder
build/Release/superzip_cli.exe extract --output restored archive.cpgz
build/Release/superzip_cli.exe compress --format ar --output archive.ar path\to\folder
build/Release/superzip_cli.exe extract --output restored archive.ar
build/Release/superzip_cli.exe extract --format deb --output restored package.deb
build/Release/superzip_cli.exe extract --format iso --output restored image.iso
build/Release/superzip_cli.exe extract --format rpm --output restored package.rpm
build/Release/superzip_cli.exe extract --format cab --output restored package.cab
build/Release/superzip_cli.exe extract --format 7z --output restored archive.7z
build/Release/superzip_cli.exe extract --format arj --output restored archive.arj
build/Release/superzip_cli.exe extract --format arc --output restored archive.arc
build/Release/superzip_cli.exe extract --format lha --output restored archive.lzh
build/Release/superzip_cli.exe extract --format wim --output restored image.wim
build/Release/superzip_cli.exe extract --format xar --output restored archive.xar
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
ZIP, ZIPX, TAR, TAR.GZ, TAR.BZ2, TAR.XZ, TAR.LZ, Gzip, Bzip2, XZ, LZMA, lzip,
Unix Compress, Base64, BinHex, MacBinary, XXEncode, UUE, CAB, 7z, ARJ, SEA ARC/ARK, LHA/LZH, WIM, XAR, CPIO,
CPIO.GZ, AR, DEB, ISO, and RPM compatibility are deliberately separate from
SUZIP tuning.
`--require-gpu`, `--force-cpu`, worker controls, block-size controls, and
`--verify-after-write` are accepted only on native `.suzip` commands because
compatibility formats do not use the AMD HIP SUZIP codec.
`--compression-level <1-9>` also controls the CPU-backed ZIP, Gzip, Bzip2,
Zstandard, TAR.GZ, TAR.BZ2, TAR.ZST, and CPIO.GZ writers. Uncompressed TAR,
CPIO, and AR containers and the fixed-policy Unix Compress writer reject that
flag rather than ignore it. `extract` defaults to auto-detection for implemented archive
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
available as opt-in checks. Defender scans are hidden subprocesses with a
bounded timeout, and a timeout is reported as a failed scan rather than as a
clean result.

GitHub Actions run build/test validation, CodeQL, Trivy, Semgrep, DevSkim, OSV,
Dependency Review, default-branch OSSF Scorecard, workflow linting, secret
scanning, SBOM generation, ClusterFuzzLite parser fuzzing, and a
Greenbone/OpenVAS integration audit. The live OpenVAS/Vulnetix scan is
scheduled/manual and requires
an OIDC-backed scanner configuration broker so private scanner credentials are
never bound directly in workflow YAML.

The lint workflow is language-specific for this repository: `clang-format` for
owned C/C++ source, PSScriptAnalyzer for PowerShell, Ruff for Python helper
scripts, yamllint for GitHub YAML, pymarkdownlnt for Markdown, and cmakelang
for CMake. Workflow security is checked separately with actionlint, zizmor, and
CodeQL Actions so GitHub-context injection issues are treated as security
findings rather than style nits.

## Fuzzing

Security-sensitive parsers are fuzzed with ClusterFuzzLite. The integration
builds libFuzzer targets for SuperZip archive-index metadata, archive-entry
path canonicalization, ISO metadata, CAB metadata, RPM header metadata, 7z
decode/metadata handling, LZMA and lzip stream handling, ARJ metadata/stored-payload
handling, SEA ARC/ARK metadata/unpacked-payload handling, CPIO metadata/path
handling, LHA/LZH
decode/metadata handling, and XAR
TOC/payload metadata handling with address
and undefined-behavior sanitizers:

```powershell
tools/fuzz.ps1 -Runs 512
```

The GitHub workflow runs automatically on pull requests, pushes to `main`, a
weekly schedule, and manual dispatch.

## Benchmarking

![Measured archive application comparison against 7-Zip and Zstandard](resources/benchmarks/application-comparison.svg)

The [Silesia application comparison report](docs/benchmarks/comparison-silesia-2026-09-29.md)
shows exact test-system specifications and five-run ZIP/Zstandard results.
The [reviewed raw record](docs/benchmarks/data/comparison-silesia-current.json)
and [comparison methodology](docs/comparative-benchmark-methodology.md) disclose
commands, hashes, host load, and limits. SuperZip is not uniformly faster or
smaller: the official Zstd CLI was faster on both single-file cases. These
compatibility-format results do not measure native HIP compression.

![Measured archive size versus creation time across five effort settings](resources/benchmarks/effort-tradeoff.svg)

The [size-versus-time effort study](docs/benchmarks/comparison-effort-2026-09-29.md)
compares SuperZip ZIP and ZST output with 7-Zip and Zstandard on the same
Silesia inputs. Each point has ten timed runs, exact archive bytes, and
independently verified extraction. Higher effort sometimes costs far more time
for a smaller file; equal numeric levels do not imply equal work across tools.

![Measured native CPU and GPU archive size and throughput](resources/benchmarks/native-cpu-hip.svg)

This snapshot uses three paired RAM-only 10 GiB runs per case, 16 MiB blocks,
an AMD Ryzen 9 9950X, and an AMD Radeon RX 9070 XT. It measures median
encode, verify, and extract throughput on the synthetic profiles shown, from
commit `fd92bc5c728443800040c93509c2cf249fb3a0a1`; the
[reviewed records](docs/benchmarks/data/) include exact byte counts and binary
hashes. GPU mode is slower on SegmentedRecords at level 1. It requires AMD HIP
but still uses CPU work for orchestration and I/O. Its device-event time
was unavailable, so that case supports wall-time comparisons only. These
results do not predict performance on other files or hardware. The
`benchmark-graph` workflow regenerates and checks the image from those records.

![Native archive size and compression time by CPU and GPU effort](resources/benchmarks/native-effort-tradeoff.svg)

The [native effort report](docs/benchmarks/native-effort-2026-09-29.md) compares
five settings on the same 10 GiB RAM-only Mixed workload. GPU archives have
different measured sizes at every plotted setting, and every sample passed
verification and extraction. The graph prints exact archive bytes; its
archive-size axis is truncated to make nearby points readable. The
[methodology](docs/comparative-benchmark-methodology.md) and
[raw records](docs/benchmarks/data/) contain the source and binary hashes,
individual timings, and limits of the comparison.

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
tools/bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1 -BlockSizeKiB 256,512,1024,2048,4096,8192,16384
```

Use `-Profile Mixed`, `-Profile Compressible`, and `-Profile Incompressible`
when characterizing a release candidate. The script refuses workloads smaller
than 10 GiB in memory mode and sweeps the production block-size choices:
256 KiB, 512 KiB, 1 MiB, 2 MiB, 4 MiB, 8 MiB, and 16 MiB. It reports production-aligned worker
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
