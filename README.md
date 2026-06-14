# SuperZip

SuperZip is a Windows x64 archive application built around AMD HIP acceleration.
Its native `.szip` format is the GPU-first path. Standard `.zip` support exists
for compatibility and is handled by the vendored miniz 3.1.1 codebase.

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
tools/build.ps1 -Configuration Release -HipArch gfx1201 -VcvarsVersion 14.44
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

## Package

Package the current HIP build:

```powershell
tools/package.ps1 -Configuration Release
```

The package script refuses to package a CPU-only build unless
`-AllowCpuValidationPackage` is passed explicitly for internal validation. For
MSI packaging, install the pinned WiX tool and set
`SUPERZIP_ACCEPT_WIX_OSMF_EULA=wix7` only after accepting the WiX v7 OSMF EULA:

```powershell
tools/package.ps1 -Configuration Release -CreateMsi
```

## CLI

```powershell
build/Release/superzip_cli.exe dependency-check
build/Release/superzip_cli.exe gpu-info
build/Release/superzip_cli.exe compress --format szip --require-gpu --output archive.szip path\to\folder
build/Release/superzip_cli.exe extract --format szip --require-gpu --output restored archive.szip
build/Release/superzip_cli.exe compress --format zip --output archive.zip path\to\folder
build/Release/superzip_cli.exe extract --format zip --output restored archive.zip
build/Release/superzip_cli.exe verify --sha256 archive.szip
```

Use `--require-gpu` for `.szip` operations that must fail instead of falling
back to the CPU validation path. Extraction refuses to overwrite by default.
Optional `--sha256` and `--defender-scan` flags add integrity hashing and
Microsoft Defender checks without making either feature implicit.

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
Dependency Review, OSSF Scorecard, workflow linting, secret scanning, SBOM
generation, and a Greenbone/OpenVAS integration audit. The live OpenVAS/Vulnetix
scan is scheduled/manual and requires repository secrets.

See `docs/security.md`, `docs/portability.md`, `docs/design.md`,
`docs/third-party.md`, `docs/release.md`, and
`docs/security-code-scanning.md`.
