# Operation-Owned HIP Sparse Timing Events

Sparse mismatch collection now retains one RAII event pair across its count and
gather passes, matching the entropy encoders' ownership pattern. Count completes
and its elapsed time is recorded before gather borrows the pair. Each operation
owns its handles; concurrent chunks share no events. Empty candidates create no
events, and an unprofitable count result returns without creating another pair.
Early returns and exceptions retain automatic cleanup.

The kernels, patch admission, archive bytes, winner selection, transfers and
bounded allocation policy are unchanged. The HIP Release build and 12 direct
cases pass: sparse admission/layout, short and long motifs, CPU/HIP decode,
independent archive readers, version-seven publication and competing block
winners. HIP is available before and after the cohort. No unrelated format,
GUI or installer suite was repeated.

## Instrumented Mechanism Comparison

The clean baseline is `a7e181dc949276a3da0a84c3417bbf61d184c34f`.
One bounded whole-process HIP API trace per variant uses a generated 10 GiB
SparseRecord workload at level 5 with 8192 KiB blocks, 32 workers, encode depth
32 and decode depth 4. Both traces independently validate every decoded byte.
All observations are retained; no samples are trimmed or selectively repeated.
Native receipts show identical build recipes/toolchains and only the sparse HIP
source differs among native inputs.

| API | Baseline calls | Candidate calls |
| --- | --- | --- |
| `hipEventCreate` | 4,320 | 4,160 |
| `hipEventDestroy` | 4,320 | 4,160 |

Each variant has 33 traced threads. Complete API calls decrease from 27,480 to
27,160. All other API counts, allocation request sizes/counts, transfer
sizes/directions/counts and failed-call counts match; no parsed calls fail.
This workload reaches gather in 80 encode chunks, removing two creations and
two destructions per such chunk. Other workloads can reach gather less often.

Both variants report 24,239,360 payload bytes, 24,262,491 archive bytes, 1,280
sparse blocks and 2,000 product kernel launches. Product H2D/D2H bytes and
device/pinned allocation totals match. The archive-to-input fraction is
0.00225962 for this intentionally repetitive generated profile; it is not a
real-corpus compression claim or a comparison with other archive tools.
Both report `memory_only=true`, `disk_write_bytes=0` and
`validated_bytes=10737418240`. Archive payloads stay in RAM; diagnostic trace
metadata is written separately, bounded to 64 MiB per trace.

One instrumented observation per separate build period cannot establish a
throughput improvement. Traced worker durations overlap and include logging
overhead. No speed claim is made, no confidence interval is inferred and no
full block-size/profile sweep is presented as completed. The verified benefit
is fewer operation-owned event lifecycles with preserved measured outputs and
resource requests on the exercised GPU.

## Evidence And Qualification Limits

Private raw traces, CLI output and sanitized reports remain under
`out/sparse-event-{baseline,candidate}-api-trace-20261003.*` and
`out/sparse-event-{baseline,candidate}-trace-report-20261003.json`.
The independent comparison is
`out/sparse-event-trace-comparison-20261003.json`; focused correctness is
`out/sparse-event-component-verification-20261003.json`.
The successful candidate receipt is
`3e3053414bb525b958077253ec600af1de939e794a1d186bf5d54b44cbe397e0`,
with native input digest
`2838224b7140a180d734d40307cf9bfc9d43b48b7d0c600f981bd5621d6ecb6f`.

The earlier hosted complete-SDK ROCm qualification at `7a3cc5b` passes compile,
link and required-HIP receipt checks. It does not qualify this subsequent source
change or execution on every compiled architecture. Exact-commit qualification,
the open security backlog and broader corpus/UI/release work remain pending.
