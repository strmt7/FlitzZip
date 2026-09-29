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
`464dbbc4050a88b881053f2e46c304cdce25888a`; all five records identify
the same CLI binary SHA-256,
`9f36903c69d7af8d3587f3333c08c04db3a95619902c512321c2def52f0869da`.

## Results

| Level | CPU archive bytes | CPU median compression s | GPU archive bytes | GPU median compression s |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 4,516,058,195 | 3.14 | 4,570,444,975 | 3.19 |
| 3 | 4,567,903,633 | 3.66 | 4,530,309,983 | 3.54 |
| 5 | 4,603,919,674 | 5.37 | 4,526,533,383 | 3.58 |
| 7 | 4,616,060,676 | 8.19 | 4,526,527,359 | 3.66 |
| 9 | 4,631,403,996 | 12.83 | 4,520,595,799 | 3.71 |

The five plotted GPU sizes are distinct. L9 is 49,849,176 bytes (1.09%)
smaller than L1 on this generated workload. Relative to the prior clean
checkpoint (`54a4ccf`), full-sample Huffman coding reduced L9 by 5,923,188
bytes (0.13%); the other four plotted GPU sizes were unchanged. These are
deterministic archive-byte differences, not a speedup claim. The CPU lane is
not monotonic toward smaller archives here: its Zstandard effort policy
produces larger archives at higher settings on this particular Mixed corpus.
It must not be presented as a size win.

Single-run, GPU-only size diagnostics on the same source and input filled the
four gaps in the plotted series. Exact archive bytes were L2 4,549,808,095;
L4 4,526,934,191; L6 4,526,535,043; and L8 4,526,524,051. Together with
the paired points above, all nine GPU sizes differ. L6 is nevertheless 1,660
bytes **larger** than L5, so effort is not monotonic on this corpus. The
[four diagnostic records](data/) are RAM-only, byte-verified, and show HIP
kernel activity; their one-run timings are not part of the reviewed graph.

The CPU level-1 timing range was 3.13-4.99 seconds, including one slow run;
its 3.14-second median does not prove a level-1 CPU/GPU speed difference.
Every point has three runs, and the graph shows observed ranges. A dedicated
timing study would need more repetitions and a controlled contention window.

The [tradeoff graph](../../resources/benchmarks/native-effort-tradeoff.svg)
shows median compression seconds against exact archive bytes. Its horizontal
archive-size axis is explicitly truncated; exact values above, not plotted
distance, should be used for percentage comparisons. The
[five paired records](data/) preserve every run, HIP telemetry, resource samples,
source identity, and binary hash. Concurrent host work can affect timing, so
these observations do not establish a universal CPU/GPU speedup or behavior
on other files and GPUs. An entropy-saturated or incompressible input can
legitimately give identical output sizes at multiple effort settings; the
encoder does not add padding or deliberately worsen a lower level.
