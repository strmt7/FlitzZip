# Troubleshooting

Start with the exact error and the affected operation. Preserve source files
and avoid retrying a failed extraction over valuable existing data until the
cause is understood.

## GPU acceleration is unavailable

Check the status on **System**. From a portable directory, PowerShell diagnostics
are also available:

```powershell
.\flitzzip_cli.exe dependency-check
.\flitzzip_cli.exe gpu-info
```

Confirm that the AMD device, Windows version and driver are supported by the
[build's GPU targets](release.md#gpu-targets). Keep the complete application
package together; the kernel DLL and dependency manifest must match that build.
Install or update the supported AMD driver when its required runtime is absent.
Do not substitute individual runtime DLLs from unrelated downloads.

Neutron requires compatible hardware and the required-GPU setting. Compatibility
formats use CPU adapters. An available driver alone does not prove that the
selected GPU can execute the kernels in a particular package.

## An archive or compression method is unsupported

Check the [format matrix](archive-format-support.md). An extension can identify
a container whose compression method is outside the supported subset. Recognition,
extraction, creation and verification are separate capabilities.

For legacy filenames, use the format's supported member-name encoding option.
Malformed names and path collisions remain errors. Changing the encoding does
not make an invalid archive valid.

## Extraction or publication fails

Check the destination, available storage, permissions, overwrite choice and
whether another process holds the input or output open. Keep the original error;
later successful reuse does not explain the earlier failure.

Truncation, CRC mismatch, invalid metadata and unsafe paths indicate an archive
that FlitzZip cannot accept. Obtain a complete trusted copy or use an independent
compatible tool to diagnose the input. Do not rely on partial output as a
successful extraction.

## Compression is slow or the result is larger

Compression depends on the input. Metadata overhead matters for tiny files, and
already-compressed data may not shrink. Neutron prioritizes its size search and
can take much longer than numeric efforts. See
[benchmark evidence](benchmarks/README.md) for measured workloads and limitations.

The System graphs are current observations rather than a controlled benchmark.
An unavailable counter is shown as unavailable. A GPU utilization percentage
alone does not establish compression speed or output-size improvement.

## Report a reproducible problem

Open the log through **Settings > Open log file** when relevant. Include the
FlitzZip version, Windows version, exact steps or command, expected result,
actual error, and a small input that reproduces the issue. GPU reports should
also identify the device and driver version. Remove private paths, filenames,
contents and credentials from shared material.

Use [GitHub Issues](https://github.com/strmt7/FlitzZip/issues) for ordinary bugs
and [SECURITY.md](../SECURITY.md) for suspected vulnerabilities. The
[release limitations](beta-readiness.md#outstanding-acceptance-limits) retain
the scope of unresolved historical observations.
