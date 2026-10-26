# Build FlitzZip

FlitzZip is a Windows-native C++20 project. Product builds target Windows x64
and include AMD HIP support. The build scripts discover supported tools and
admit exact pinned dependencies rather than relying on a particular host layout.

## Prerequisites

- Windows 11 x64 and a supported Visual Studio C++ toolchain.
- CPython 3.13 or 3.14 for development provisioning.
- A compatible AMD GPU and driver for actual HIP execution tests.

The scripts provision the pinned CMake and complete ROCm SDK when absent.
The first SDK download and extraction require substantial space; unchanged
verified installations are reused. Use the
[ROCm guide](../rocm-toolchain.md) for an existing or custom SDK directory.
`-HipPath` is explicit configuration; a global `HIP_PATH` is not required.

## Build and run

From the repository root in PowerShell:

```powershell
tools\build.ps1 -Configuration Release
.\build\Release\FlitzZip.exe
```

The CLI is `build\Release\flitzzip_cli.exe`. Close the built GUI before rebuilding
or packaging. A fresh build directory selects an installed supported Visual
Studio generator; an existing build retains its configured generator.

For a coherent change, select its actual checks:

```powershell
tools\verification_plan.ps1 -IncludeUntracked
tools\verify_changes.ps1 -IncludeUntracked -Checkpoint intermediate
```

Follow [targeted verification](../targeted-verification.md) for committed ranges,
final checkpoints and hosted acceptance. A tool-only contract must not silently
rebuild native targets. Format, GUI and codec changes retain their real consumers.

## Broad native qualification

When required by the change or release review:

```powershell
tools\test.ps1 -Configuration Release
```

The native driver runs its registered CTest and CLI contracts. Required-HIP
correctness needs a real admitted GPU. Build and measurement receipts bind the
configuration, toolchain, relevant source and actual outputs.

CPU-only builds are available for hosted and static-analysis roles:

```powershell
tools\build.ps1 -Configuration Release -CpuOnlyValidation
tools\test.ps1 -Configuration Release
```

They do not qualify device execution and are not product release packages.
Single-target HIP development builds likewise do not replace the portable
[release target set](../release.md#gpu-targets).

## Packaging and specialized tools

Use the [release guide](../release.md) for portable/MSI packaging, WiX provisioning,
source offerings and installer lifecycle tests. Publication needs separate
maintainer approval after candidate qualification.

[Coverage](../code-coverage.md), [device measurement](hip-device-coverage.md),
[fuzzing and scanning](../security-code-scanning.md) and
[benchmarks](../benchmarks/README.md) each have distinct tool and resource contracts.
Use the existing launcher for the selected task. Preserve host policy and other
applications; do not change global execution policy or system tool versions as
an incidental build repair.
