# GPU-Accelerated UI And Codec Research

Checked on 2026-06-20.

This document records source-backed engineering lessons from GPU-accelerated UI
systems, GPU decompression/compression libraries, and AMD HIP performance
guidance. It is not a design-copying document. External projects are used only
to extract architecture patterns that can be evaluated inside SuperZip's
existing Windows-native, AMD HIP-only product boundary.

## Sources Reviewed

- AMD HIP performance guidelines:
  <https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/performance_guidelines.html>
- AMD HIP host memory guidance:
  <https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/hip_runtime_api/memory_management/host_memory.html>
- AMD HIP graph guidance:
  <https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/hip_runtime_api/hipgraph.html>
- AMD rocPRIM documentation:
  <https://rocm.docs.amd.com/projects/rocPRIM/en/latest/index.html>
- AMD rocPRIM device reduce documentation:
  <https://rocm.docs.amd.com/projects/rocPRIM/en/latest/device_ops/reduce.html>
- AMD rocPRIM histogram documentation:
  <https://rocm.docs.amd.com/projects/rocPRIM/en/docs-5.7.0/device_ops/histogram.html>
- ROCm hipCOMP-core:
  <https://github.com/ROCm/hipCOMP-core>
- Zarr v2 storage specification, reviewed to understand chunked array
  metadata and compressor declarations:
  <https://zarr-specs.readthedocs.io/en/latest/v2/v2.0.html>
- Microsoft DirectStorage GDeflate sample notes:
  <https://github.com/microsoft/DirectStorage/blob/main/GDeflate/README.md>
- Microsoft DirectStorage GPU decompression deep dive:
  <https://devblogs.microsoft.com/directx/directstorage-1-1-now-available/>
- NVIDIA nvCOMP C API documentation, reviewed only as a public architecture
  reference for batched GPU compression APIs:
  <https://docs.nvidia.com/cuda/nvcomp/c_api.html>
- Microsoft Direct2D overview:
  <https://learn.microsoft.com/en-us/windows/win32/direct2d/direct2d-overview>
- Microsoft DirectComposition overview:
  <https://learn.microsoft.com/en-us/windows/win32/directcomp/why-use-directcomposition->
- Microsoft Windows Visual layer overview:
  <https://learn.microsoft.com/en-us/windows/apps/develop/composition/visual-layer>
- Qt Quick scene graph documentation:
  <https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html>
- Flutter Impeller documentation:
  <https://docs.flutter.dev/perf/impeller>

## Relevant Field Patterns

### GPU UI systems

Modern GPU-accelerated UI stacks converge on the same broad design:

- Retain renderable state between frames instead of rebuilding everything for
  every paint.
- Batch work to reduce draw calls, state changes, and per-frame setup.
- Keep rendering/composition independent from the app's UI event thread when
  possible.
- Precompile or cache GPU resources so first use does not cause visible stalls.
- Use instrumentation that can explain frame pacing, render-thread work,
  resource uploads, and broken refresh-rate assumptions.

SuperZip currently uses a Win32 GDI/GDI+ double-buffered renderer. That is not a
GPU-rendered UI stack. For the current fixed-size enterprise interface, this is
acceptable only if the UI thread remains responsive, animation timers are
bounded, and archive work never blocks painting or input. A future Direct2D or
Visual-layer renderer could improve animation smoothness and high-DPI text
rendering, but it is not the main path to archive throughput. It must be treated
as a separate UI-rendering increment, not as evidence that compression is GPU
accelerated.

### GPU compression and decompression systems

High-throughput GPU decompression systems do not send one large serial stream to
one small kernel. They split data into independently schedulable tiles or
chunks, launch enough work to fill the device, keep metadata in device-visible
buffers, and batch many streams to reduce tail effects. DirectStorage GDeflate
uses 64 KiB tiles and batches work so GPU thread groups can process many tiles.
nvCOMP's public API exposes the same high-level model: arrays of device chunk
pointers, device chunk sizes, temporary GPU workspace, asynchronous operation,
and explicit stream synchronization.

The useful lesson for SuperZip is the architecture, not the vendor API:

- Archive work should be chunked into many GPU-visible units.
- Metadata needed by the GPU should stay in device-accessible arrays.
- Temporary device workspace should be allocated once per bounded execution
  window or reused through a pool, not allocated and freed for every tiny stage.
- Host/device copies should be batched and minimized.
- Required-GPU lanes must fail if GPU work cannot run; hidden CPU fallback must
  never be counted as GPU throughput.

### AMD HIP optimization guidance

AMD's HIP guidance maps directly to SuperZip's current bottlenecks:

- Profile first with HIP API timing, kernel timing, transfer timing, and system
  GPU telemetry.
- Use streams to overlap independent copies and kernels.
- Use pinned host memory for high-throughput transfer paths when the RAM budget
  and system pressure allow it.
- Minimize host/device transfers and avoid copying intermediate data back to the
  host when it can stay on the device.
- Avoid frequent `hipMalloc`/`hipFree` in loops; reuse allocations or use stream
  ordered allocation paths where they are proven stable on the supported Windows
  HIP toolchain.
- Use enough blocks and warps to keep the device busy, but do not create so many
  kernels that launch overhead dominates.
- Consider HIP graphs only for repeated, stable command sequences after the
  stream and memory-resource design is proven.

rocPRIM is an approved HIP-native building block for reductions, scans, and
segmented operations. It is not a complete archive codec, but it is a credible
way to reduce custom prefix-sum, offset, and integrity-reduction code if a
benchmark proves a win.

## Current SuperZip Gap Analysis

The current `.suzip` HIP path performs real AMD HIP work and is measured with
nonzero kernel launches, HIP event time, transfer bytes, and required-GPU
failure behavior. Version 3 can also emit native GPU static-prefix and adaptive
GPU-prefix blocks for low-entropy byte streams, which gives required-HIP jobs
real compact GPU-decodable payloads without CPU Deflate fallback. It is still
not a fully GPU-resident general-purpose compression pipeline. The remaining
gaps are:

- Input and output are still host-resident archive buffers around each HIP
  stage.
- The current codec copies complete source chunks to VRAM and copies decoded
  chunks back for extraction.
- Several helpers still allocate and free device memory per operation.
- There is no multi-stream copy/compute overlap in the production codec path.
- The native `.suzip` compression model is currently block classification,
  compact fill/pattern representation, static and adaptive low-entropy prefix
  coding, raw payloads, GPU materialization, and GPU CRC. It is not a GPU
  DEFLATE, Zstandard, ANS, LZ dictionary, or GDeflate implementation.

This distinction matters. Dramatic GPU utilization gains require a larger
codec-pipeline redesign, not only GUI polish or benchmark settings.

## Implemented 2026-06-19 Codec Fix

The accepted code changes in this iteration fix a real ratio defect in the
required-HIP native path while preserving the AMD-only GPU boundary:

1. `.suzip` version 2 adds a native GPU static-prefix block kind for low-entropy
   byte streams that are neither fill nor short periodic patterns.
2. Required-HIP compression may emit raw, fill, GPU-pattern, or GPU-prefix
   blocks, but still must not emit CPU Deflate blocks.
3. The GPU prefix encoder uses 4 KiB independently decodable segments, computes
   segment bit counts on HIP, packs segment bitstreams on HIP with 256 threads
   per segment, and validates extraction through the normal archive CRC path.
4. Verification and extraction can decode GPU-prefix blocks on HIP when
   required-GPU mode is selected; CPU decoding remains available only for normal
   non-required compatibility reads of native archives.
5. The benchmark workload now includes a deterministic low-entropy non-pattern
   region in the Mixed profile so the root failure class remains visible in
   RAM-only CPU/GPU sweeps.

The earlier classifier optimization remains in place:

1. The host selects provisional raw, fill, or short-pattern candidates from a
   bounded sample of each block.
2. HIP verifies provisional fill/pattern candidates across 64 KiB tiles.
3. Raw-only chunks skip the fill/pattern verification kernel.

This directly addresses two defects: large archive blocks launched too little
GPU work, and required-HIP archives had no compact representation for
low-entropy non-pattern data.

## Implemented 2026-06-20 HIP Batching And Device Verification

The accepted code changes in this iteration reduce launch, allocation, and
host-copy overhead without changing the `.suzip` v2 archive format:

1. Prefix-code compression now batches the whole uploaded chunk: one HIP length
   pass over every 4 KiB prefix segment, one HIP pack pass for the selected
   prefix blocks, and one combined device-to-host bitstream copy.
2. Prefix-capable verification now materializes decoded bytes in VRAM and runs
   the CRC over the device buffer, copying back only compact CRC segment
   metadata instead of the whole decoded chunk.
3. Decode/extract skips the raw/fill/pattern materializer launch for prefix-only
   chunks, avoiding kernels that would have no bytes to write.
4. The resource policy remains bounded by the existing 128 MiB archive chunk
   limit and the HIP VRAM budget check. The implementation does not fill 80% of
   total VRAM; it uses currently free VRAM with an explicit reserve so the OS,
   display stack, and other processes are not starved.

## Implemented 2026-06-20 Adaptive GPU Prefix Blocks

The accepted code changes in this iteration make higher required-HIP
compression levels meaningful without emitting CPU Deflate blocks:

1. `.suzip` version 3 adds a native adaptive GPU-prefix block kind. Each block
   stores a bounded 84-byte codebook, per-4 KiB segment offsets, and a GPU-packed
   bitstream.
2. Levels 1, 3, and 5 keep the fast static GPU-prefix path. Levels 7 and 9
   evaluate an adaptive codebook candidate and publish it only when its measured
   encoded byte count is smaller than the existing GPU-native candidate.
3. Adaptive packing and adaptive decoding run through AMD HIP kernels. CPU
   decode exists only for non-required compatibility reads and verification.
4. Required-HIP verification and extraction reject CPU Deflate blocks as before;
   stronger levels do not use hidden CPU fallback.
5. Telemetry counts emitted prefix blocks only. Kernel, transfer, and allocation
   counters still include the adaptive evaluation work because that work really
   executed on the GPU.

hipCOMP-core was reviewed again before this change. Its public README identifies
the project as an early-access technology preview, states that production
workloads are not recommended, and notes that newer nvCOMP algorithms such as
DEFLATE, GDEFLATE, ANS, and zSTD are not part of that repository yet. Therefore
it remains a research dependency candidate rather than a production shortcut.

## Implemented 2026-06-20 Block-Selective GPU Prefix Blocks

The accepted code changes in this iteration close a mixed-block ratio defect in
the required-HIP native path:

1. Prefix-code evaluation now applies to every verified raw block inside a
   chunk, not only to chunks where every block is raw.
2. Fill and pattern blocks in the same chunk are preserved unchanged, while
   eligible raw blocks may be replaced by static or adaptive GPU-prefix blocks
   only when the measured encoded payload is smaller.
3. Required-HIP archives still must not emit CPU Deflate blocks. The fallback
   for an uneconomical GPU-prefix candidate remains the existing GPU-verified
   fill, pattern, or raw block representation.
4. A regression test now creates a mixed chunk containing a fill block followed
   by a low-entropy raw block and proves that the low-entropy raw block becomes
   a GPU-prefix block, then verifies and extracts the archive with required
   HIP.

## Zarr-Style Ratio Reproduction

A maintainer-provided 542.5 MiB Zarr-style directory with 1,411 regular files
and 1,827 directories reproduced the ratio defect. The Zarr metadata declared a
chunk compressor, so many payload chunks were low-entropy streams rather than
plain text or fill patterns.

| Path | Output bytes | Ratio | Native block evidence |
| --- | ---: | ---: | --- |
| Old required-HIP `.suzip` | 569,234,175 | 1.00069 | 1,412 raw blocks, no compact native blocks |
| Fixed required-HIP `.suzip` v2/v3 level 5 | 403,457,451 | 0.709263 | Static GPU-prefix blocks, 0 CPU Deflate blocks |
| Adaptive required-HIP `.suzip` v3 level 9 | 400,381,293 | 0.703855 | Adaptive GPU-prefix blocks where smaller, 0 CPU Deflate blocks |
| Same-input SuperZip ZIP level 5 | 400,176,189 | 0.703494 | Standards-compatible ZIP path through miniz |
| Same-input independent ZIP level 5 | 399,577,914 | 0.702443 | Local external comparison only; not a repository dependency |

The fixed archive passed required-HIP verification and required-HIP extraction.
The extracted tree then passed a SHA-256 comparison for every restored file:
zero mismatches and zero extra files.

The same-input 2026-06-20 smoke confirmed that current `.suzip` no longer keeps
that folder near its original size. It also exposed a remaining production
performance gap: the folder contains many small files, so the current native
writer launches GPU encode and verify work per file entry. The measured level-5
required-HIP no-verify compression run emitted 1,400 GPU-prefix blocks but also
4,212 HIP kernel launches and about 30.50 seconds of HIP kernel time. With
`--verify-after-write`, the same input required 7,023 total HIP kernel launches
and about 62.45 seconds of HIP kernel time because verification decodes every
entry. A durable speed fix for this workload needs a bounded small-entry
batching design or a persistent HIP workspace/stream design; it should not be
solved with hidden CPU Deflate fallback or by weakening required-HIP semantics.

## RAM-Only Benchmark Evidence

The command below used the Release build, compression level 5, a 10 GiB
generated Mixed workload, and the default memory-only benchmark mode. Every
lane reported `memory_only=true` and `disk_write_bytes=0`.

```powershell
tools\bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1 -BlockSizeKiB 256,512,1024,2048,4096,8192,16384
```

| Block size | CPU total seconds | GPU total seconds | GPU speedup | CPU ratio | GPU ratio | GPU kernels | GPU kernel ms | GPU pattern blocks | GPU prefix blocks |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 KiB | 16.09866 | 8.35037 | 1.93x | 0.439201 | 0.425743 | 360 | 2955.68 | 10,240 | 10,240 |
| 512 KiB | 14.98367 | 7.87814 | 1.90x | 0.439115 | 0.425698 | 360 | 2969.21 | 5,120 | 5,120 |
| 1 MiB | 14.93346 | 7.67386 | 1.95x | 0.439084 | 0.425676 | 360 | 2875.34 | 2,560 | 2,560 |
| 2 MiB | 14.80519 | 7.62835 | 1.94x | 0.439069 | 0.425665 | 360 | 2889.80 | 1,280 | 1,280 |
| 4 MiB | 14.83379 | 7.52817 | 1.97x | 0.439061 | 0.425659 | 360 | 2908.20 | 640 | 640 |
| 8 MiB | 14.86088 | 7.41113 | 2.01x | 0.439058 | 0.425656 | 360 | 2860.30 | 320 | 320 |
| 16 MiB | 14.74788 | 7.57451 | 1.95x | 0.439056 | 0.425655 | 360 | 2843.21 | 160 | 160 |

The built-in tuner selected 8 MiB for the same level-5 Mixed profile on the
same host state:

| Compression level | Block size | GPU score | Speedup vs CPU | Ratio | GPU pattern blocks | GPU prefix blocks | Memory only | Disk writes |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: |
| 5 | 8 MiB | 43348 | 2.03853x | 0.425656 | 320 | 320 | true | 0 |

The same runs recorded exact input/output bytes:

| Lane | Block size | Input bytes | Output bytes | Ratio |
| --- | ---: | ---: | ---: | ---: |
| CPU | 256 KiB | 10,737,418,240 | 4,715,887,361 | 0.439201 |
| CPU | 512 KiB | 10,737,418,240 | 4,714,965,564 | 0.439115 |
| CPU | 1 MiB | 10,737,418,240 | 4,714,631,176 | 0.439084 |
| CPU | 2 MiB | 10,737,418,240 | 4,714,468,439 | 0.439069 |
| CPU | 4 MiB | 10,737,418,240 | 4,714,384,769 | 0.439061 |
| CPU | 8 MiB | 10,737,418,240 | 4,714,346,936 | 0.439058 |
| CPU | 16 MiB | 10,737,418,240 | 4,714,330,593 | 0.439056 |
| GPU | 256 KiB | 10,737,418,240 | 4,571,380,884 | 0.425743 |
| GPU | 512 KiB | 10,737,418,240 | 4,570,899,604 | 0.425698 |
| GPU | 1 MiB | 10,737,418,240 | 4,570,658,964 | 0.425676 |
| GPU | 2 MiB | 10,737,418,240 | 4,570,538,644 | 0.425665 |
| GPU | 4 MiB | 10,737,418,240 | 4,570,478,484 | 0.425659 |
| GPU | 8 MiB | 10,737,418,240 | 4,570,448,404 | 0.425656 |
| GPU | 16 MiB | 10,737,418,240 | 4,570,433,364 | 0.425655 |

Additional 10 GiB RAM-only sweeps on the same build:

- `Compressible`: the full sweep passed with `memory_only=true` and
  `disk_write_bytes=0`; a focused rerun of the high-throughput candidates
  showed GPU speedups of 1.72x at 4 MiB, 1.67x at 8 MiB, and 1.65x at 16 MiB.
  The first full sweep included one noisy 8 MiB outlier, so future release
  evidence should continue to use at least one repeated candidate run.
- `Incompressible`: every swept block size stayed at ratio 1.0 and the
  required-HIP lane was faster than forced CPU, with speedups from 1.81x to
  2.78x.

## Accepted Performance Direction

Short-term accepted work:

1. Continue tiled GPU verification and materialization.
2. Reduce host/device copies where the decoded stream can be verified or
   checksummed on the device.
3. Add profiler-assisted evidence with HIP API timing and system GPU telemetry
   before claiming a bottleneck is solved.
4. Batch small regular-file entries into bounded GPU work windows or reuse a
   persistent HIP workspace so many-file folders do not pay one full allocation
   and kernel-launch sequence per file.
5. Evaluate compact metadata tables for non-raw blocks only, so raw-heavy mixed
   workloads do not launch verification work that immediately returns.

Medium-term accepted work:

1. Introduce a bounded HIP execution context that can reuse device scratch
   buffers without exceeding the existing VRAM budget.
2. Evaluate stream-ordered allocation or explicit scratch pools on the supported
   Windows HIP SDK, with fallback to the current blocking allocation path if the
   toolchain does not support it.
3. Evaluate pinned staging buffers only after proving that the pinned-RAM budget
   is bounded and released. Pinned memory must never push total system RAM usage
   above the repository's resource-safety policy.
4. Use HIP streams to overlap H2D, kernel, and D2H stages only when correctness
   tests prove no lifetime, ordering, or telemetry regression.
5. Consider rocPRIM for ordered scans/reductions if it removes custom GPU code
   and improves the 10 GiB RAM-only profile sweep.

Long-term research work:

1. Design a new `.suzip` codec generation that is GPU-native by construction:
   independently decodable tiles, device-side offset scans, compact per-tile
   status arrays, and batched encode/decode queues.
2. Keep hipCOMP-core behind research-only evaluation until upstream removes the
   production-workload warning or the project accepts a documented preview-risk
   decision.
3. Do not switch to CUDA, OpenCL, SYCL, WebGPU, or DirectCompute for the archive
   codec unless a maintainer explicitly changes the AMD HIP-only boundary.

## Rejected Shortcuts

- Counting GPU-rendered UI as archive acceleration.
- Hiding CPU fallback inside a required-GPU benchmark lane.
- Increasing benchmark workload disk writes to make I/O look busy.
- Pinning unbounded host RAM or holding large VRAM scratch buffers per worker.
- Adding an early-access GPU compression dependency to release artifacts without
  a documented security, licensing, packaging, and production-risk decision.
- Copying external application UI, text, icons, help content, or branding.

## Engineering Rule

Every future GPU performance change must report whether it improves CPU/GPU
speed at the same compression level and ratio, whether it changes transfer
bytes or device allocation behavior, and whether it moves work into actual HIP
kernels instead of only changing benchmark orchestration.
