# SuperZip

SuperZip is a native Windows C++ archive application with an AMD-only HIP
acceleration boundary.

## Modes

- `SuperZip GPU (.szip)`: AMD HIP accelerated archive mode.
- `ZIP compatibility (.zip)`: standards-oriented ZIP mode using vendored miniz
  3.1.1.

## Build

Portable build without HIP:

```powershell
tools/build.ps1 -Configuration Release
tools/test.ps1
tools/security_scan.ps1
```

Local AMD HIP build:

```powershell
tools/build.ps1 -Configuration Release -EnableHip -HipArch gfx1201 -VcvarsVersion 14.44
build/Release/superzip_cli.exe gpu-info
```

`HIP_PATH` must point at the AMD ROCm/HIP installation.

Package a local build:

```powershell
tools/package.ps1 -Configuration Release
```

The package script installs the built executables and docs into `out/` and then
creates a ZIP package. Build/package outputs are ignored by Git.

Manual beta releases are created by `.github/workflows/beta-release.yml`. See
`docs/release.md` for version inputs, MSI packaging, and the self-hosted AMD HIP
runner requirement for GPU-enabled release binaries.

## CLI

```powershell
build/Release/superzip_cli.exe compress --format szip --output archive.szip path\to\folder
build/Release/superzip_cli.exe extract --format szip --output restored archive.szip
build/Release/superzip_cli.exe compress --format zip --output archive.zip path\to\folder
build/Release/superzip_cli.exe extract --format zip --output restored archive.zip
build/Release/superzip_cli.exe verify --sha256 archive.szip
```

Pass `--require-gpu` for `.szip` commands when the operation must fail instead
of using the CPU validation path. Extraction refuses to overwrite by default;
pass `--overwrite` only when replacing existing files is intended. Optional
`--sha256` and `--defender-scan` flags add integrity hashing and Microsoft
Defender scans without making either feature implicit.

## GUI

The Win32 GUI has Queue, Compress, Extract, Security Review, History, AMD GPU,
and Preferences pages. It is PerMonitorV2 DPI-aware, double-buffered, and uses
native DPI fonts so it remains crisp on high-refresh and high-resolution
displays. Archive work runs on a background thread and repaint requests are
coalesced to avoid flooding 200 Hz displays.

## Memory Model

Native `.szip` compression, verification, and extraction are chunked. ZIP
compatibility uses miniz file streaming APIs. SuperZip should not need to load a
whole large archive entry into RAM for normal operations.

## Security

SuperZip rejects unsafe extraction paths, malformed metadata, CRC mismatches,
and accidental overwrites by default. Microsoft Defender scanning and SHA-256
integrity hashing are available as opt-in hooks and are disabled by default.
GitHub Actions run automatic security scanning with CodeQL, Trivy, Semgrep,
DevSkim, OSV, Dependency Review, OSSF Scorecard, workflow linting, and an
optional secrets-gated Greenbone/OpenVAS plus Vulnetix lane.

See `docs/security.md`, `docs/portability.md`, `docs/design.md`,
`docs/third-party.md`, and `docs/release.md`. See
`docs/security-code-scanning.md` for the security workflow and repository-secret
setup.
