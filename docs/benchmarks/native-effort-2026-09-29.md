# Native CPU And GPU Effort Study

## Scope

This is a reproducible, RAM-only `.suzip` archive study, not a disk-throughput
or cross-product result. The deterministic Mixed generator supplied 10 GiB
(10,737,418,240 bytes) at each effort, with 16 MiB archive blocks. Each of
levels 1, 3, 5, 7, and 9 had three alternating CPU/GPU pairs. Every run
verified and extracted the modeled archive byte for byte, required zero disk
writes, and checked that the GPU lane launched HIP kernels. Compression time
excludes the separately measured verification and extraction phases but
includes generation and orchestration. The full procedure and graph rules are
in [the methodology](../comparative-benchmark-methodology.md).

The test host ran Windows 11 Pro build 26200 with an AMD Ryzen 9 9950X
(16 cores, 32 threads), 64 GiB RAM, and an AMD Radeon RX 9070 XT with driver
32.0.31041.1004. The source was clean at commit
`54a4ccf0d88d7919ebb9127c93eb2c4d3f3745b4`; all five records identify
the same CLI binary SHA-256,
`6ad98611512a35ac90243b0669cc48251b95e312cf54fa3a9d8d1cdb758d4ee5`.

## Results

| Level | CPU archive bytes | CPU median compression s | GPU archive bytes | GPU median compression s |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 4,516,058,195 | 3.24 | 4,570,444,975 | 3.23 |
| 3 | 4,567,903,633 | 3.80 | 4,530,309,983 | 3.61 |
| 5 | 4,603,919,674 | 5.39 | 4,526,533,383 | 3.73 |
| 7 | 4,616,060,676 | 8.35 | 4,526,527,359 | 3.70 |
| 9 | 4,631,403,996 | 12.66 | 4,526,518,987 | 3.66 |

The five plotted GPU sizes are distinct. L9 is 43,925,988 bytes (0.96%)
smaller than L1 on this generated workload; the final high-effort differences
are small. The CPU lane is not monotonic toward smaller archives here: its
Zstandard effort policy produces larger archives at higher settings on this
particular Mixed corpus. It must not be presented as a size win. A separate
single-run GPU size diagnostic also produced distinct output at all nine
levels, but L6 was 1,660 bytes larger than L5. Those untimed diagnostics are
not part of this reviewed five-level graph.

The [tradeoff graph](../../resources/benchmarks/native-effort-tradeoff.svg)
shows median compression seconds against exact archive bytes. Its horizontal
archive-size axis is explicitly truncated; exact values above, not plotted
distance, should be used for percentage comparisons. The
[five raw records](data/) preserve every run, HIP telemetry, resource samples,
source identity, and binary hash. Concurrent host work can affect timing, so
these observations do not establish a universal CPU/GPU speedup or behavior
on other files and GPUs. An entropy-saturated or incompressible input can
legitimately give identical output sizes at multiple effort settings; the
encoder does not add padding or deliberately worsen a lower level.
