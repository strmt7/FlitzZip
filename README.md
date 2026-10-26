<p align="center">
  <img src="resources/brand/flitzzip-logo.svg" alt="FlitzZip logo" width="760">
</p>

[![License: AGPL-3.0](https://img.shields.io/badge/license-AGPL--3.0-blue.svg)](LICENSE)
[![Windows CI](https://img.shields.io/github/actions/workflow/status/strmt7/FlitzZip/windows-ci.yml?branch=main&label=windows-ci)](https://github.com/strmt7/FlitzZip/actions/workflows/windows-ci.yml)
[![Security](https://img.shields.io/github/actions/workflow/status/strmt7/FlitzZip/security-code-scanning.yml?branch=main&label=security)](https://github.com/strmt7/FlitzZip/actions/workflows/security-code-scanning.yml)
[![Lint](https://img.shields.io/github/actions/workflow/status/strmt7/FlitzZip/lint.yml?branch=main&label=lint)](https://github.com/strmt7/FlitzZip/actions/workflows/lint.yml)
[![Fuzzing](https://img.shields.io/github/actions/workflow/status/strmt7/FlitzZip/fuzzing.yml?branch=main&label=fuzzing)](https://github.com/strmt7/FlitzZip/actions/workflows/fuzzing.yml)
[![Scorecard](https://img.shields.io/github/actions/workflow/status/strmt7/FlitzZip/scorecard.yml?branch=main&label=scorecard)](https://github.com/strmt7/FlitzZip/actions/workflows/scorecard.yml)
[![Release](https://img.shields.io/github/actions/workflow/status/strmt7/FlitzZip/release.yml?branch=main&label=release)](https://github.com/strmt7/FlitzZip/actions/workflows/release.yml)
[![Commit activity](https://img.shields.io/github/commit-activity/m/strmt7/FlitzZip)](https://github.com/strmt7/FlitzZip/commits/main)
[![Codecov](https://img.shields.io/codecov/c/github/strmt7/FlitzZip?label=Codecov&logo=codecov)](https://codecov.io/gh/strmt7/FlitzZip)
[![cocoindex-code](https://img.shields.io/static/v1?label=&message=cocoindex-code&color=555&logo=github&logoColor=white)](https://github.com/cocoindex-io/cocoindex-code)
[![caveman](https://img.shields.io/static/v1?label=&message=caveman&color=555&logo=github&logoColor=white)](https://github.com/JuliusBrussee/caveman)
[![crawl4ai](https://img.shields.io/static/v1?label=&message=crawl4ai&color=555&logo=github&logoColor=white)](https://github.com/unclecode/crawl4ai)

# FlitzZip

A native Windows archive manager with AMD GPU acceleration, a graphical
interface and a command-line tool.

FlitzZip uses its versioned `.flzip` format for GPU compression and supports
common archive formats through in-process CPU adapters. Archive creation,
extraction and verification share explicit format capabilities and guarded
file publication.

**Prerelease software.** Keep original files and verify archives before relying
on them for long-term storage. See [release readiness](docs/beta-readiness.md)
for qualification scope and known limitations.

## What it does

- Create, extract and verify native FLZIP and supported ZIP archives.
- Work with TAR variants, CPIO, AR, Gzip, Bzip2 and Zstandard; extract additional
  formats including supported 7z, CAB, ISO, RPM and WIM subsets.
- Run native compression on a compatible AMD HIP GPU with explicit backend
  selection, progress, cancellation and operation history.
- Reject malformed metadata, unsafe paths and accidental overwrites, with
  optional SHA-256 hashing and Microsoft Defender scanning.

The [format support matrix](docs/archive-format-support.md) lists exact
create, extract and verification capabilities. Unsupported methods fail with
an error. GPU acceleration applies to native FLZIP operations.

## Neutron star mode

Neutron is a separate experimental FLZIP mode that prioritizes archive size.
It searches additional GPU candidates and selects the smallest complete
representation found, including its decoding metadata.

The mode appears below efforts 1–9 when compatible HIP hardware is available
and the required-GPU setting is enabled. It always requires real GPU execution.
The numeric efforts keep their own behavior.

See the [mode description](docs/neutron-star-mode-research.md) and
[public-corpus results](docs/neutron-public-corpus-benchmark.md). Results depend
on the workload; current evidence does not establish universal size superiority
or a comparative GPU speedup.

## Get FlitzZip

Published packages are available from [GitHub Releases](https://github.com/strmt7/FlitzZip/releases).
The release process produces a portable ZIP and an MSI containing the same
HIP-enabled binaries, SHA-256 checksums and corresponding source.

- **Portable:** extract the ZIP and run `FlitzZip.exe`.
- **Installer:** use the MSI for a standard Windows installation under
  `Program Files`; administrator approval may be required.

Candidate validation and permission to publish are separate steps. The current
release procedure and acceptance criteria are in the [release guide](docs/release.md).

## Requirements

FlitzZip targets **Windows 11 x64**. GPU operations additionally require a
compatible AMD GPU and a driver providing the HIP runtime expected by the
package. Check the [supported build targets](docs/release.md#gpu-targets) and
[runtime prerequisites](docs/portability.md#runtime-prerequisites).

Missing GPU prerequisites leave capability reporting and CPU functions usable.
Neutron remains unavailable. Runtime and kernel payloads undergo explicit
admission checks before GPU support is advertised.

## Quick start

From the extracted portable directory, run these examples in PowerShell:

```powershell
.\flitzzip_cli.exe compress --format zip --output "archive.zip" "path\to\folder"
.\flitzzip_cli.exe verify "archive.zip"
.\flitzzip_cli.exe extract --output "restored" "archive.zip"
```

For native GPU compression:

```powershell
.\flitzzip_cli.exe compress --format flzip --require-gpu --verify-after-write --output "archive.flzip" "path\to\folder"
```

Use `.\flitzzip_cli.exe --help` for command syntax. The
[CLI guide](docs/cli.md) covers format selection, Neutron, verification,
filename encoding and exit codes.

## Documentation

Start with the [documentation index](docs/README.md).

| Task | Guide |
| --- | --- |
| Use the CLI or check a format | [CLI](docs/cli.md), [format support](docs/archive-format-support.md) |
| Install and use the application | [Installation](docs/installing.md), [graphical workflow](docs/using-flitzzip.md) |
| Resolve an error | [Troubleshooting](docs/troubleshooting.md) |
| Understand safety and report a vulnerability | [Security design](docs/security.md), [reporting policy](SECURITY.md) |
| Assess performance claims | [Benchmark evidence](docs/benchmarks/README.md) |

## Development and contributing

Bug reports are most useful with a version, exact command or user action,
expected result, observed result and a small reproducible input. See
[CONTRIBUTING.md](CONTRIBUTING.md) for contributions, the
[developer guide](docs/development/README.md) for building and testing, and
[AGENTS.md](AGENTS.md) for AI-agent instructions.

## License

FlitzZip is licensed under [AGPL-3.0](LICENSE). Bundled components retain their
own licenses and notices; see [third-party software](docs/third-party.md).
Release packages include the required notices and corresponding source.
