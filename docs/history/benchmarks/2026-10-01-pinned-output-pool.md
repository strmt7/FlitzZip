# Bounded HIP Output Reuse And Shared Checksum Workers

This development checkpoint follows the [shared CRC and dictionary review](2026-10-01-shared-crc-backend.md).
Measurements used uncommitted development source on `main`, based on
`65be0ccc012e968b26a8ef6389784387e7ad38c0`. These measurements identify a local
production-path improvement, not release acceptance, a compression-ratio
improvement, or a published ranking against other archive applications.

## Root Causes And Implementation

The first owned pinned-output candidate retained the existing 32-window GPU
decode queue. Its process-wide 512 MiB pin limit admitted only four 128 MiB
outputs at once. The 10 GiB benchmark used three encode/verify windows; only
12 outputs, totaling 1,536 MiB, obtained pinned storage. The remaining outputs
used heap storage. A separate instrumented HIP trace confirmed 12 successful
host allocations and 12 successful frees, not a cleanup failure. Allocation
and free API worker-time sums were 947,147 and 187,811 microseconds; these are
not phase wall times. The trace is diagnostic and is excluded from throughput
comparisons.

Required-GPU extraction now limits its decode queue against the same host pin
capacity and reuses a bounded operation-local output pool. Aggregate pin
reservations cannot exceed 512 MiB or one-sixteenth of available physical RAM
at allocation admission. Idle buffers remain charged to that budget. The pool
selects the smallest fitting buffer and releases unusable idle extents before
attempting a larger allocation. It retains at most 64 idle buffers; no global
buffer cache survives an operation. Lower-memory hosts may admit fewer tasks.

Owned output pairs each allocation with its matching heap, HIP, or pool
release. Borrowers retain shared pool lifetime; moves and destruction on another
thread do not lose the release owner. Failed HIP frees retain their reservations
and increment diagnostics. Pin-budget denial or allocation failure falls back
to heap output, not to a CPU codec. Required-GPU decoding still must execute
through HIP. CPU-only blocks never request pinned output, and CPU/automatic
queue policy is unchanged.

Allocations use `hipHostMallocPortable`, without changing device selection or
using write-combined memory. AMD documents both the transfer benefit and the
host-RAM cost of pinned memory; write-combined storage is unsuitable for the
CPU reads needed here. Windows NUMA policy is not assumed available.
[AMD host-memory guidance](https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/hip_runtime_api/memory_management/host_memory.html).

Queue admission alone regressed the Compressible profile. Pool reuse reduced
allocation work but did not remove that regression. Code inspection found that
the four admitted GPU outputs still computed their host checksums serially;
the RAM path also copied the encoder's one-worker share into decoding. Native
extraction and the RAM benchmark now share one overflow-safe per-window worker
calculation. With 32 requested workers and four decode windows, each window
receives eight checksum workers. Small or low-concurrency shapes resolve their
own shares; this is not a fixed host-specific worker count or a new universal
hard cap on every pipeline shape.

The checksum remains IEEE CRC-32 over the actual copied host bytes. Independent
CPU ranges use the existing SDK backend and ordered CRC combination, at least
8 MiB per admitted task, bounded by the existing 64-worker ceiling. All readers
join before output ownership can be released, including exception unwinding.
No device checksum substitutes for validation of host output. Production
`extract_suzip` and RAM extraction use the same `decode_owned_chunk` path.
Pool setup and final release remain inside their extraction timers.

## Controls And Measurement Method

Each main comparison contains 36 runs: three paired rounds of Mixed,
Compressible, and Incompressible in CPU and GPU modes. Build order and mode
order alternate. Every run processes 10 GiB at effort 5, 8 MiB blocks, and
32 requested workers. The 250 ms inter-run pause is outside child phase timers.
Encoding and verification retain their original 32-window queue; required-GPU
extraction in the final candidate uses four windows and eight checksum workers
per window on this host. CPU extraction retains 32 windows and one worker per
window.

The platform is Windows 11 x64, Ryzen 9 9950X with 32 logical processors,
64 GiB installed RAM, and one Radeon RX 9070 XT (`gfx1201`). The trusted loaded
HIP runtime file version is `10.0.3679.0`. This does not establish hardware
acceptance for other release targets, runtime versions, or multi-GPU systems.
No unrelated processes, drivers, power settings, or device selection were
changed.

| Build | CLI SHA-256 |
| --- | --- |
| Saved pageable control, SDK CRC plus dictionary repair | `12d42ef5104d5b07d25b7e9495fc7b5d532926d6ddb7f6fd49ba46166581b81d` |
| Fresh pins, original 32-window decode queue | `c8ca870fd034c1d0181a3d846b8ba7a1fefbe4a8b9bd7b6c6c66ba30216fc28a` |
| Fresh pins, bounded four-window queue | `43f2cf1c55a660f040a39f4cc4408334aa82dafba2550f9fa7773e9e0dcf7ecd` |
| Reused pins, four-window queue, serial host checksum | `6eb52c4d46d0cb9000300995f0e6f01933ce2fd5e1e56ffab5fa14679ac6e803` |
| Reused pins plus shared decode/checksum worker policy | `6708823762bc211f9513cdfa42c1faa2d88e63f6eba2147e5e327d36b8efab20` |

Every run asserted `memory_only=true`, `disk_write_bytes=0`, reconstruction CRC,
and actual HIP telemetry for GPU work. The 10 GiB RAM benchmark checks CRCs;
it does not compare every restored byte with a retained 10 GiB source. Focused
owned-output tests and bounded real-archive fixtures separately compare full
bytes. CRC agreement is not a cryptographic integrity or security proof.

## Intermediate Results Retained

GPU extraction medians, in seconds, retain the unsuccessful phases rather than
selecting only the final win:

| Candidate | Mixed Before / After | Compressible Before / After | Incompressible Before / After |
| --- | ---: | ---: | ---: |
| Fresh pins, 32-window queue | 1.53319 / 1.40147 | 0.901753 / 0.819161 | 1.80370 / 1.61743 |
| Fresh pins, four-window queue | 1.51464 / 1.43910 | 0.914239 / 0.925615 | 1.74680 / 1.40877 |
| Pool reuse, serial checksum | 1.56872 / 1.44685 | 0.881535 / 0.953843 | 1.87999 / 1.28797 |

The latter two candidates are not accepted as standalone improvements for all
profiles. Their Compressible regressions motivated the shared checksum-worker
fix. Different rounds have different controls; stage medians must not be
subtracted as a controlled measurement of each isolated component.

## Final Paired Results

The following values are median seconds across three repeats. Extraction
speedup is each matched control time divided by its candidate time, reported
as the range of all three pairs. Sizes are complete modeled archives, including
index and footer, and were unchanged between control and candidate in every
matched case.

| Profile | Mode | Archive Bytes | Compress Before / After | Verify Before / After | Extract Before / After | Paired Extraction Speedup |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Mixed | CPU | 4,603,345,552 | 4.63644 / 4.54132 | 0.992641 / 0.972190 | 0.989236 / 0.969245 | 0.998-1.023x |
| Mixed | GPU | 4,527,858,919 | 3.47561 / 3.27113 | 0.780200 / 0.738421 | 1.52142 / 1.03902 | 1.380-1.466x |
| Compressible | CPU | 1,074,642,523 | 1.88603 / 1.88514 | 0.974501 / 0.990363 | 0.911211 / 0.891618 | 0.962-1.042x |
| Compressible | GPU | 1,073,857,115 | 1.88847 / 1.78816 | 0.157206 / 0.162347 | 0.913227 / 0.567376 | 1.599-1.663x |
| Incompressible | CPU | 10,737,441,371 | 3.42536 / 3.29996 | 0.973341 / 0.971214 | 0.821750 / 0.839209 | 0.975-1.005x |
| Incompressible | GPU | 10,737,441,371 | 2.71506 / 2.24818 | 0.680323 / 0.598159 | 1.77710 / 0.880604 | 1.980-2.031x |

Archive/input ratios are respectively 0.428720/0.421690 for Mixed CPU/GPU,
0.100084/0.100011 for Compressible CPU/GPU, and 1.000002 for both
Incompressible modes. This extraction change does not improve those ratios.
Compression includes source generation; the pool/checksum changes do not alter
encoder search. Compression and verification shifts are reported, not
attributed solely to the extraction fix. Incompressible CPU extraction and
Compressible GPU verification were slower by median; small CPU differences
overlap paired variation and are not claimed as wins.

Final GPU extraction is faster than CPU on Compressible, but still slower on
Mixed and Incompressible. There is no universal GPU advantage. Each final GPU
run processed 10,240 MiB through pinned output while allocating only 512 MiB
of fresh pinned extents. Those counters represent cumulative successful decode
allocation and output bytes, not instantaneous resident RAM or VRAM. CPU pin
counters were zero. Separate `decode_inflight_chunks` and
`decode_codec_workers` fields make decoder policy visible without relabeling
the encoder's existing worker fields.

## Resource Context And Drift

The final 36-run record contains 57 during-workload host observations,
72 before/after observations, and three preflight observations. During-workload
CPU peaked at 90.41%, including the benchmark; available RAM stayed at or above
20,519 MiB. Disk busy time peaked at 11.88% and paging at 165.38 pages/s.
Before/after CPU peaked at 9.23% and the busiest other GPU engine at 0.78%.
During-run non-child GPU accounting peaked at 39.68%; system/driver activity
may contribute, so it is not proof of independent competing work.

Seven during-run supplemental queries flagged errors, each with eight invalid
counter samples. Required CPU/RAM/paging/disk fields remained available; valid
GPU samples were retained, but partial counter coverage limits GPU-context
conclusions. No invalid samples were replaced with zero. The record does not
identify each invalid instance, so its exact cause is not asserted. Process
I/O counters were unavailable; zero archive writes is separately asserted by
the product's RAM benchmark. Sparse observations do not prove exclusive
hardware, thermal stability, or absence of short contention.

Mixed GPU extraction increased from 1.01995 to 1.03902 to 1.10261 seconds
across the three candidate rounds. Several CPU and verification groups also
slowed monotonically; small changes are not inferred to be product gains.
A focused AB/BA confirmation with a 500 ms untimed pause produced candidate
Mixed GPU extraction times of 1.02001 and 1.00568 seconds, versus controls
1.40838 and 1.57387. The slowdown did not reproduce, but this does not prove
that the longer pause caused recovery. All four runs preserved archive size,
CRC validation, RAM-only operation, and real HIP use. Their observations had
at least 27,035 MiB available RAM and no invalid counter samples. Because these
runs do not write the archive to storage, they do not establish SSD cache
exhaustion or a need for long disk-cooldown delays.

## Correctness And Acceptance

New regressions cover aggregate pin admission, concurrent reservation caps,
exact ownership across moves and threads, pool best-fit reuse and lifetime,
failure cleanup, CPU-only policy, bounded queue admission, shared worker
arithmetic, and independent IEEE CRC-oracle checks for unaligned large ranges,
irregular tails, and worker boundaries. A real required-GPU archive fixture
checks complete restored bytes and queue admission. Benchmark reporting tests
keep fresh allocation bytes distinct from all reused output bytes.

The full pool snapshot passed all 22 local verification commands with
527 C++ tests, automated GUI smoke with 79 captures and 24 settings-migration
cases, independent interoperability, the 36-format matrix, fourteen bounded
sanitizer/fuzzer targets, portable packaging, MSI identity, and eleven MCP
containment tests. Two fresh compact System/Compress screenshots were inspected;
automation coverage is not full manual visual acceptance of every captured
state. The subsequent shared checksum/worker delta passed all ten
classifier-selected commands, including 530 C++ tests, independent
interoperability, the 36-format matrix, and fourteen bounded sanitizer/fuzzer
targets. Its verification is recorded separately below.
The unchanged GUI/packaging snapshot is not presented as a new full run on the
final checksum binary. A subsequent final pre-push pass separately completed
all 22 full verification commands with 530 C++ tests, 79 current-run GUI
captures, and the same interoperability, sanitizer, packaging, and MCP gates.
The CLI hash remained the measured final-candidate hash. Capture counts come
from each run's returned manifest, not stale images accumulated in `out/`.

The final fourteen-case Mixed sweep covers 256, 512, 1,024, 2,048, 4,096,
8,192, and 16,384 KiB in both modes. Every complete archive size matches the
saved control sweep, with 10 GiB input and zero archive writes. Every GPU case
reports real HIP timing, 512 MiB fresh pins, 10 GiB reused pinned output,
four decode windows, and eight checksum workers. One run per block size is a
regression sweep, not a repeated release performance study.

Local evidence, intentionally excluded from source control:

- `out/benchmarks/pinned-output-paired-2026-09-30T233324-7286821Z-n36.json`
- `out/research/pinned-output-hip-api-summary.json`
- `out/benchmarks/pinned-output-admission-paired-2026-09-30T235044-8068885Z-n36.json`
- `out/benchmarks/pinned-output-pool-paired-2026-10-01T001549-4721928Z-n36.json`
- `out/benchmarks/pinned-output-shared-workers-paired-2026-10-01T004327-0510675Z-n36.json`
- `out/benchmarks/pinned-output-trend-confirmation-2026-10-01T010325-5081782Z-n4.json`
- `out/benchmarks/pinned-output-shared-workers-seven-blocks.json`
- `out/benchmarks/dictionary-admission-seven-blocks-serial.json`
- `out/verification-pinned-output-pool-final-batch.log`
- `out/verification-pinned-output-shared-workers-delta.log`
- `out/verification-pinned-output-prepush-full.log`

Remaining gates include complete operation/visual review, CPU-format competitor
measurements, held-out GPU workloads, broader host/runtime acceptance, hosted
finding triage, dependency resolution, exact-SHA workflow completion, installer
install/repair/uninstall, final end-user documents and graphs, and maintainer
approval before release. A local policy scan or this checkpoint does not prove
zero vulnerabilities or complete review of every repository function.
