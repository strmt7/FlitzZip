# GPU-Accelerated UI And Codec Research

Checked on 2026-06-19.

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
failure behavior. Version 2 can also emit native GPU static-prefix blocks for
low-entropy byte streams, which gives required-HIP jobs real compact
GPU-decodable payloads without CPU Deflate fallback. It is still not a fully
GPU-resident general-purpose compression pipeline. The remaining gaps are:

- Input and output are still host-resident archive buffers around each HIP
  stage.
- The current codec copies complete source chunks to VRAM and copies decoded
  chunks back for extraction.
- Several helpers still allocate and free device memory per operation.
- There is no multi-stream copy/compute overlap in the production codec path.
- The native `.suzip` compression model is currently block classification,
  compact fill/pattern representation, static low-entropy prefix coding, raw
  payloads, GPU materialization, and GPU CRC. It is not a GPU DEFLATE,
  Zstandard, ANS, LZ dictionary, or GDeflate implementation.

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

## Zarr-Style Ratio Reproduction

A maintainer-provided 542.5 MiB Zarr-style directory with 1,411 regular files
and 1,827 directories reproduced the ratio defect. The Zarr metadata declared a
chunk compressor, so many payload chunks were low-entropy streams rather than
plain text or fill patterns.

| Path | Output bytes | Ratio | Native block evidence |
| --- | ---: | ---: | --- |
| Old required-HIP `.suzip` | 569,234,175 | 1.00069 | 1,412 raw blocks, no compact native blocks |
| Fixed required-HIP `.suzip` v2 | 403,457,469 | 0.709263 | 1,400 GPU-prefix blocks, 12 raw blocks, 0 CPU Deflate blocks |

The fixed archive passed required-HIP verification and required-HIP extraction.
The extracted tree then passed a SHA-256 comparison for every restored file:
zero mismatches and zero extra files.

## RAM-Only Benchmark Evidence

The command below used the Release build, compression level 5, a 10 GiB
generated Mixed workload, and the default memory-only benchmark mode. Every
lane reported `memory_only=true` and `disk_write_bytes=0`.

```powershell
tools\bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1 -BlockSizeKiB 256,512,1024,2048,4096,8192,16384
```

| Block size | CPU total seconds | GPU total seconds | GPU speedup | CPU ratio | GPU ratio | GPU kernels | GPU kernel ms | GPU pattern blocks | GPU prefix blocks |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 KiB | 17.63206 | 14.22199 | 1.24x | 0.439201 | 0.425743 | 31,040 | 3668.58 | 10,240 | 10,240 |
| 512 KiB | 15.08117 | 10.72676 | 1.41x | 0.439115 | 0.425698 | 15,680 | 3196.23 | 5,120 | 5,120 |
| 1 MiB | 14.97491 | 9.50097 | 1.58x | 0.439084 | 0.425676 | 8,000 | 3059.16 | 2,560 | 2,560 |
| 2 MiB | 14.89909 | 8.85762 | 1.68x | 0.439069 | 0.425665 | 4,160 | 3008.47 | 1,280 | 1,280 |
| 4 MiB | 14.94820 | 8.41256 | 1.78x | 0.439061 | 0.425659 | 2,240 | 2975.87 | 640 | 640 |
| 8 MiB | 14.95788 | 8.45319 | 1.77x | 0.439058 | 0.425656 | 1,280 | 3035.12 | 320 | 320 |
| 16 MiB | 14.72284 | 8.29575 | 1.77x | 0.439056 | 0.425655 | 800 | 2882.01 | 160 | 160 |

The built-in tuner selected 16 MiB for the revised Mixed profile:

| Compression level | Block size | GPU score | Speedup vs CPU | Ratio | GPU pattern blocks | GPU prefix blocks | Memory only | Disk writes |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: |
| 5 | 16 MiB | 39114 | 1.82845x | 0.425655 | 160 | 160 | true | 0 |

## Accepted Performance Direction

Short-term accepted work:

1. Continue tiled GPU verification and materialization.
2. Reduce host/device copies where the decoded stream can be verified or
   checksummed on the device.
3. Add profiler-assisted evidence with HIP API timing and system GPU telemetry
   before claiming a bottleneck is solved.
4. Evaluate compact metadata tables for non-raw blocks only, so raw-heavy mixed
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
