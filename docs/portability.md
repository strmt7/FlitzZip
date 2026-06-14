# Portability And Installation Model

SuperZip must be host-agnostic and installation-agnostic within its supported
platform boundary.

## Supported Platform Boundary

- Windows 11.
- AMD GPU supported by the installed AMD ROCm/HIP for Windows runtime.
- ROCm/HIP installed in any location exposed through `HIP_PATH`.
- Visual Studio C++ build tools available for local source builds.

The app must not hard-code the developer machine's ROCm path, Visual Studio
path, username, temporary directory, checkout directory, or GitHub credentials.

## Build Discovery

Build scripts discover:

- CMake from common Visual Studio and system locations.
- ROCm/HIP from `HIP_PATH`.
- `hipcc.exe` from `%HIP_PATH%\bin`.
- `amdhip64.lib` from `%HIP_PATH%\lib`.
- A compatible Visual Studio toolset through `vcvarsall.bat`.

The default local HIP architecture is `gfx1201` for the current workstation, but
`tools/build.ps1 -EnableHip -HipArch <gfx...>` overrides it. For distribution,
build separate artifacts per supported AMD GFX target or add a fat-binary build
matrix after validating ROCm's Windows support for each target.

## Runtime Discovery

At runtime, SuperZip queries HIP for device count and selected device
properties. It reports unsupported or unavailable GPUs explicitly. It must never
fall back to CUDA, WebGPU, DirectCompute, or another vendor path.

## Display Scaling

The GUI declares PerMonitorV2 DPI awareness in its manifest and sets the same
awareness context at startup. Fonts are recreated per monitor DPI, layout
coordinates are scaled with integer `MulDiv` math, and painting is
double-buffered. Progress repaint requests are coalesced so very high refresh
rate displays do not accumulate redundant paint messages.

## User Data

The application should store preferences and history under the user's Windows
known application-data folder in a later persistence pass. It must not store
absolute build paths or secrets.

## Large Files

Archive operations are designed around bounded chunks rather than whole-file
buffers:

- `.szip` compression reads source files in configurable chunks.
- `.szip` verification and extraction decode bounded block windows.
- ZIP compatibility delegates file I/O to miniz file streaming APIs.

Future release packaging should keep this model intact and add large-file
regression benchmarks before changing archive layout or block grouping.
