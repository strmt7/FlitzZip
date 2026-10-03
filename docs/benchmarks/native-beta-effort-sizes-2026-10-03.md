# Native Beta Effort Size Diagnostics — 3 October 2026

All nine efforts were checked on the generated **10 GiB Mixed** input with
**8 MiB blocks**, forced CPU and required HIP. Each of the 18 observations
independently validated every byte, reported `memory_only=true` and
`disk_write_bytes=0`, and retained its raw journal. These are **exact size
diagnostics only**: one observation per lane cannot estimate variability or
qualify a timing claim. All timing-quality labels remain inconclusive. No
observations were dropped, pooled with earlier studies or repeated in response
to their measured values.

Clean source `8cabac46fa306dbab0dbcb9591440f0b7c50c7ad`; CLI SHA-256
`a2294790af46e0cf243c5012381c41760ba1bd52af517109004c60d9219a0b06`. Native input digest
`2319e61aff7fed492a9dd44fa7e8fb6c6a9f85e968716ff94910b5d60d6ca940` and portable receipt
`5f3f3abddb07bd276b2c29af704d93df80b9a631aa2358b750322899c7001ada` were identical across all nine cases.
Hardware: AMD Ryzen 9 9950X / Radeon RX 9070 XT, complete pinned ROCm Core SDK
10.0.0. Effort order was 1 through 9, with CPU then GPU within each independent
fixed-count study. This is not an order-counterbalanced effort timing comparison.

Every input is **10,737,418,240 bytes**. Ratios below divide the complete modeled
archive size by input size, including framing/index metadata. Encoded payload
bytes and all worker, resource and HIP counters remain in the linked records.

| Effort / record | CPU archive bytes | CPU archive/input ratio | HIP archive bytes | HIP archive/input ratio | HIP prefix blocks |
| ---: | ---: | ---: | ---: | ---: | ---: |
| [1](data/beta-effort-size-L1-20261003.json) | 4,516,089,518 | 0.420593612 | 4,570,471,535 | 0.425658332 | 320 |
| [2](data/beta-effort-size-L2-20261003.json) | 4,516,089,518 | 0.420593612 | 4,551,164,963 | 0.423860267 | 320 |
| [3](data/beta-effort-size-L3-20261003.json) | 4,516,089,518 | 0.420593612 | 4,531,631,627 | 0.422041083 | 320 |
| [4](data/beta-effort-size-L4-20261003.json) | 4,516,089,518 | 0.420593612 | 4,528,266,271 | 0.421727660 | 320 |
| [5](data/beta-effort-size-L5-20261003.json) | 4,516,089,518 | 0.420593612 | 4,527,857,731 | 0.421689612 | 320 |
| [6](data/beta-effort-size-L6-20261003.json) | 4,516,089,518 | 0.420593612 | 4,527,848,567 | 0.421688758 | 320 |
| [7](data/beta-effort-size-L7-20261003.json) | 4,516,089,518 | 0.420593612 | 4,527,841,503 | 0.421688101 | 320 |
| [8](data/beta-effort-size-L8-20261003.json) | 4,516,089,518 | 0.420593612 | 4,527,836,835 | 0.421687666 | 320 |
| [9](data/beta-effort-size-L9-20261003.json) | 4,516,089,518 | 0.420593612 | 4,521,915,459 | 0.421136195 | 320 |

Archive size need not decrease at every effort: the candidate search budget is
different, and independent block decisions can tie or regress. No padding or
artificial degradation is used to manufacture distinctions. The record retains
the actual measured values; no smoothing or replacement is applied.

Every adjacent HIP effort reduced the modeled archive size on this particular input.

This synthetic Mixed input uses pattern/prefix compression, not native
dictionary or sparse-pattern payload blocks. The result does not characterize
authentic file corpora or compare equivalent codec work across applications.
The separately [qualified level-5 block study](native-beta-blocks-2026-10-03.md)
uses a pilot and repeated confirmations; it is not pooled with these diagnostics.

## Reproduce One Size Diagnostic

```powershell
tools/bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 1 -BlockSizeKiB 8192 -FixedIterations -Iterations 1 -SuiteTimeoutSeconds 300 -RunTimeoutSeconds 180 -JsonOutput out/new-native-size-L1.json
```

Run levels 1 through 9 on the same frozen source/build to reproduce all size
cases. For performance conclusions use independent pilot-based repeated
confirmation, sufficient wall budgets and the declared scientific safeguards;
these single-observation timers must never enter a qualified tradeoff graph.
