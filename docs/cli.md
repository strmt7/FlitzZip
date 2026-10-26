# Command-line Guide

Examples use PowerShell. From the extracted portable package, select the
executable once; `&` invokes the path stored in `$FlitzZip`, including paths
containing spaces:

```powershell
$FlitzZip = (Resolve-Path ".\flitzzip_cli.exe").Path
& $FlitzZip --help
```

For a source build, use `.\build\Release\flitzzip_cli.exe` in the assignment
instead. Input and output paths below are examples; quote paths containing
spaces. Commands return `0` on success, `1` on operation or option-validation
failure and `2` when a missing or unknown command prints usage.

Archive commands reject unknown options before writing output. For a filename
beginning with `--`, put `--` after all options and before the input paths, for
example `& $FlitzZip verify --sha256 -- "--archive.zip"`. Extraction and
verification each accept exactly one archive, including after this separator.

## Check readiness and inspect archives

```powershell
& $FlitzZip dependency-check
& $FlitzZip gpu-info
& $FlitzZip formats
& $FlitzZip identify "archive.tar"
```

`dependency-check` checks runtime prerequisites; `gpu-info` reports the HIP
backend. `formats` lists the build's create/extract capabilities, while
`identify` detects an archive's format without extracting it.

## Create and restore a native GPU archive

```powershell
& $FlitzZip compress --format flzip --require-gpu --verify-after-write --output "archive.flzip" "path\to\folder"
& $FlitzZip verify --require-gpu --sha256 "archive.flzip"
& $FlitzZip extract --require-gpu --output "restored" "archive.flzip"
```

`--require-gpu` makes an unavailable HIP backend or a CPU-only native block
method fail explicitly. `--verify-after-write` adds an archive read-back pass;
`verify` checks an existing archive. `--sha256` additionally hashes the selected
path; a hash alone does not establish authenticity. Extraction detects the
format automatically and refuses accidental overwrite by default.

## Work with compatibility formats

```powershell
& $FlitzZip compress --format zip --compression-level 5 --output "archive.zip" "path\to\folder"
& $FlitzZip compress --format tar.zst --compression-level 5 --output "archive.tar.zst" "path\to\folder"
& $FlitzZip compress --format zst --compression-level 5 --output "file.txt.zst" "file.txt"
& $FlitzZip extract --output "restored" "archive.7z"
```

Choose `--format` when creating an archive. Folder/container formats such as
ZIP and TAR variants differ from single-file streams such as Gzip, Bzip2 and
Zstandard. Extraction normally needs no format flag; use an explicit format
when necessary, for example `--format macbinary`. The
[format support matrix](archive-format-support.md) documents every format,
alias and method limit, including extraction-only formats. Compatibility
adapters run on the CPU and do not use native FLZIP GPU tuning options.

## Commands and options

| Command | Purpose |
| --- | --- |
| `--help`, `--version` | Show syntax or build version. |
| `dependency-check`, `gpu-info` | Check prerequisites and HIP availability. |
| `formats`, `identify <archive>` | Inspect supported capabilities or detect a file's format. |
| `compress`, `extract` | Create an archive or restore its contents. |
| `verify <archive>` | Validate FLZIP, ZIP or supported ZIPX contents. |
| `gpu-diagnostic` | Run a bounded HIP diagnostic workload. |
| `memory-benchmark`, `benchmark-suite` | Measure native RAM-only workloads; see [benchmark guide](benchmarks/README.md). |

| Option | Scope and behavior |
| --- | --- |
| `--output <path>` | Required destination for compression or extraction. |
| `--format <name>` | Select creation format or override extraction auto-detection. |
| `--compression-level <1-9>` | Native FLZIP and CPU ZIP, Gzip, Bzip2, Zstandard, TAR.GZ, TAR.BZ2, TAR.ZST and CPIO.GZ writers. Uncompressed TAR, CPIO, AR and fixed-policy Unix Compress reject it. |
| `--require-gpu`, `--force-cpu` | Native FLZIP only; mutually exclusive. Use forced CPU for diagnostics or comparisons. |
| `--workers`, `--inflight`, `--block-size-kib` | Native operation controls; consult `--help` for each command's accepted options and ranges. |
| `--verify-after-write` | FLZIP or ZIP creation; adds post-write archive validation. |
| `--overwrite` | Extraction only; explicitly allow replacement of existing files. |
| `--validate-before-publish` | Extraction; validate through the guarded publication path before finalizing output. |
| `--sha256`, `--defender-scan` | Optional integrity hashing or bounded Microsoft Defender checks; additional passes are explicit. |
| `--name-encoding utf8\|system` | Compatibility extraction filename decoding; format-specific restrictions apply. |

Recognized but unsupported formats or methods fail explicitly. Full command
syntax comes from `& $FlitzZip --help`; native format semantics and CPU/GPU
limits are documented in the [FLZIP specification](native-flzip-format.md).

## Neutron star mode

Neutron is a separate native FLZIP mode. It requires a compatible AMD HIP GPU
and explicit required-GPU policy:

```powershell
& $FlitzZip compress --format flzip --require-gpu --neutron-star --verify-after-write --output "small.flzip" "path\to\folder"
```

The mode searches additional GPU candidates and retains the smallest complete
representation found. Efforts 1–9 remain separate. Read the
[mode description](neutron-star-mode-research.md) and
[measured results](neutron-public-corpus-benchmark.md) for its current scope.
