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

The RAM-only benchmark also records host-observed encode worker stages. Its
`gpu_classification_stage_worker_seconds` group splits admission/input allocation,
input upload, source CRC, and candidate validation. These are nested inside the
existing classification total, not additional top-level stages. Do not add them
to that total or interpret summed concurrent worker time as elapsed wall time or
HIP event time. The boundaries do not add device synchronization or change codec
selection. Optional-GPU successes and archive read-back verification preserve
these fields; failed GPU attempts must not pollute CPU fallback statistics.

The October 2 Mixed diagnostic sweep used 10 GiB, effort 5, all seven block
sizes and one CPU/GPU run per size. All 14 lanes verified bytes, remained
RAM-only and retained the earlier baseline's exact archive sizes. On that host,
admission/input allocation plus upload accounted for about 72-75% of the
classification worker interval. This identifies a next investigation, not
a controlled performance improvement or a general hardware result.

AMD documents [stream copies as synchronous and ordinary device frees as implicitly synchronizing](https://rocm.docs.amd.com/projects/HIP/en/latest/doxygen/html/group___memory.html).
Those contracts make allocation/transfer lifetime a relevant hypothesis, but
do not prove which runtime call causes the measured interval on Windows.
Any allocator change must retain capability checks, bounded memory, stream
lifetime and failure cleanup before making comparative speed claims.

This diagnostic is intentionally development-only. Do not use it to claim that
the GUI is GPU accelerated, and do not replace the required release benchmark
suite with this narrower transfer check.
