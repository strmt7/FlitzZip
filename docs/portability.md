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

- `.szip` compression reads source files in configurable chunks.
- `.szip` verification and extraction decode bounded block windows.
- ZIP compatibility delegates file I/O to miniz file streaming APIs.

Large-file benchmarks should be run before changing archive layout, block size,
or chunk grouping.
