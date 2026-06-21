# Development Transfer Diagnostics

SuperZip's production telemetry already records AMD HIP kernel launches,
host-to-device bytes, device-to-host bytes, and device allocation bytes for every
operation. `tools\transfer_diagnostics.ps1` exposes that data as a development
diagnostic without adding a product UI path or a separate benchmark
implementation.

Run it only after a Release build:

```powershell
tools\build.ps1 -Configuration Release
tools\transfer_diagnostics.ps1 -Configuration Release -WorkloadProfile Mixed -CompressionLevel 5 -BlockSizeKiB 1024
```

The tool runs the same RAM-only `memory-benchmark` command twice: once with
`--force-cpu` and once with `--require-gpu`. It refuses to pass unless both lanes
report `memory_only=true` and `disk_write_bytes=0`, and unless the GPU lane
reports nonzero HIP transfers and allocation counters.

The output is JSON so agents and programmers can diff runs while changing codec
code:

- `cpu_input_bytes`, `cpu_output_bytes`, and `cpu_compression_ratio`
- `gpu_input_bytes`, `gpu_output_bytes`, and `gpu_compression_ratio`
- `gpu_h2d_bytes`, `gpu_d2h_bytes`, and `gpu_device_allocation_bytes`
- `gpu_kernel_launches` and `gpu_kernel_ms`

This diagnostic is intentionally development-only. Do not use it to claim that
the GUI is GPU accelerated, and do not replace the required release benchmark
suite with this narrower transfer check.
