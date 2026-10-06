# GPU-Accelerated UI And Codec Research

This document records source-backed engineering lessons from GPU-accelerated UI
systems, GPU decompression/compression libraries, and AMD HIP performance
guidance. It is not a design-copying document. External projects are used only
to extract architecture patterns that can be evaluated inside SuperZip's
existing Windows-native, AMD HIP-only product boundary.

## Architecture References

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

The GDI renderer now retains one off-screen bitmap and memory DC across paints.
The surface is recreated when client dimensions or monitor identity change and
invalidated on DPI/display changes. Per-frame SaveDC/RestoreDC keeps fonts,
clipping, and coordinate transforms from leaking into later frames. Allocation
or presentation failure falls back to direct painting; it must not leave a
blank window. The cache holds at most one surface, bounded to 67,108,864 pixels,
and releases the old surface before allocating a replacement.

The memory-only `gdi_back_buffer` tests verify 200 same-size frames with one
surface allocation, changed destination pixels, resize and monitor invalidation,
destination clipping, font release, callback-exception recovery, invalid-size
rejection, and unchanged process GDI-handle counts after repeated destruction.
This proves avoided allocation work, not measured FPS or GPU codec acceleration.
The resource ownership and state-reset rules follow Microsoft's
[memory DC](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-createcompatibledc)
and [SaveDC](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-savedc)
contracts.

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

## Current SuperZip Boundaries

The [native format specification](native-suzip-format.md) defines the actual
versioned representations. Current HIP compression includes dictionary,
prefix/Huffman, pattern and sparse candidates; Neutron additionally considers
compound encodings, byte-plane transforms and independent entropy contexts.
Required-HIP creation does not use CPU Deflate or silently retry on CPU.
Independent CPU readers remain available for supported native representations.
See the [Neutron algorithm and research guide](neutron-star-mode-research.md)
for its exact minimum-byte parsing and closed candidate set.

The codec pipeline owns host archive buffers around bounded HIP work. Transfers,
allocations, launches and synchronization must be measured separately from
end-to-end compression. Device allocation-byte counters describe actual allocated
extents, not peak live VRAM or the number of runtime calls. Neutron's same-lifetime
parser arrays share one admitted allocation; other operations follow their own
documented owners and budgets. Neither a retained buffer nor asynchronous API
spelling alone establishes overlap or faster archives.

The separately admitted kernel module resolves the selected architecture's
kernels before advertising capability. Missing, changed or unsupported HIP
payloads reject required-GPU work. The supported-target build matrix establishes
compilation; each architecture needs its own runtime evidence.

## Evaluation Sequence

1. Identify a measured bottleneck or complete-size limitation using current
   source and source-bound evidence. Separate GUI responsiveness, codec work,
   transfers, allocation overhead and archive I/O.
2. Preserve explicit ownership, ordering, bounded work, cancellation and error
   propagation. Stream-ordered allocation, persistent scratch pools and async
   copies require supported-runtime qualification before production adoption.
3. Compare complete archives on licensed original files through the existing
   RAM-only protocols. Retain exact-byte readback, all losing observations and
   the declared uncertainty; a single kernel microbenchmark is insufficient.
4. Keep Neutron experiments isolated from numeric levels. Other levels adopt a
   result only after demonstrating both smaller files and higher speed there.
5. Reuse validated evidence only while source, tools, configuration and relevant
   outputs match. Current measurements live in the [benchmark index](benchmarks/README.md);
   superseded engineering runs remain in Git and local evidence.

## Research Dependencies And Product Boundaries

The [backend evaluation](compression-backend-evaluation.md) records dependency
eligibility. hipCOMP-core, CUDA libraries and inference-specific codecs are
research references, not implicit product dependencies. New runtime dependencies
need the existing licensing, security, packaging and maintainer review. AMD HIP
remains the only approved archive-compute boundary.

Do not count GPU-rendered UI as archive acceleration, conceal CPU substitution,
pin unbounded host memory, change driver timeout policy, or write generated
multi-gigabyte benchmark payloads to storage. Preserve canonical branding and
native product text. External UI and codec projects provide architectural ideas,
not artwork, text or code to copy without the applicable permission.
