# Compression Backend Evaluation

SuperZip's product requirement is AMD-only GPU acceleration with a Windows x64
application and installer. This document records which GPU-compute options are
credible for the current architecture and which options would change the
product boundary.

## Sources Reviewed

- hipCOMP-core:
  https://github.com/ROCm/hipCOMP-core
- AMD HIP SDK for Windows:
  https://rocm.docs.amd.com/projects/install-on-windows/en/latest/index.html
- AMD HIP SDK Windows component support:
  https://rocm.docs.amd.com/projects/install-on-windows/en/latest/conceptual/component-support.html
- AMD rocPRIM:
  https://rocm.docs.amd.com/projects/rocPRIM/en/latest/
- AMD rocSPARSE:
  https://rocm.docs.amd.com/projects/rocSPARSE/en/latest/
- Khronos OpenCL:
  https://www.khronos.org/opencl/
- Khronos SYCL:
  https://www.khronos.org/sycl/
- AdaptiveCpp AMD ROCm/HIP backend:
  https://github.com/AdaptiveCpp/AdaptiveCpp
- LLVM OpenMP offloading:
  https://openmp.llvm.org/

## Decision

The approved production path remains modern C++20 plus AMD HIP. The archive
format, telemetry, installer dependency checks, and benchmarks should continue
to make GPU usage explicit through the HIP backend. A required-GPU operation
must fail rather than silently using a CPU path.

rocPRIM is acceptable as a HIP-native building block when it reduces risk or
improves measurable performance. rocSPARSE, OpenCL, SYCL, and OpenMP offload
are not approved replacement codec paths for this repository.

hipCOMP-core is worth tracking as an experimental research branch only. Its
README describes the project as an early-access technology preview and warns
that production workloads are not recommended. That upstream status is not
compatible with making it the default SuperZip release codec today.

## Option Assessment

| Option | Fit | Assessment |
| --- | --- | --- |
| AMD HIP | Approved production boundary | Directly matches the AMD-only requirement and the existing Windows build, dependency-check, telemetry, and packaging model. |
| hipCOMP-core | Research only | Promising AMD GPU compression work, but upstream labels it early-access and not recommended for production. It must not be sold as a production-safe dependency without an explicit risk decision. |
| rocPRIM | Approved building block | HIP-native primitives such as reductions, scans, and segmented operations can support CRC, prefix metadata, and future block analysis without changing the architecture. |
| rocSPARSE | Not appropriate | A sparse linear-algebra library, not a general archive compression codec. It does not solve SuperZip's ZIP/SUZIP compression problem. |
| OpenCL | Not approved for production | Useful heterogeneous compute standard, but it introduces a separate runtime path and cross-vendor abstraction that conflicts with the current HIP-only product boundary. |
| SYCL | Not approved for production | SYCL can target AMD through ROCm/HIP toolchains, but it adds a new compiler/runtime abstraction and installer burden. It is not a codec by itself. |
| OpenMP offload | Not approved for production | LLVM OpenMP offload can target AMDGPU in supported toolchains, but it is compiler technology rather than a complete compression library and is not a stronger Windows app packaging story than HIP. |
| `std::execution` | CPU-side only for this product | Current mainstream Windows C++ standard-library execution policies do not make SuperZip compression run on AMD GPUs. They can help CPU validation code, not the required GPU codec. |

## Implementation Rules

- Keep HIP as the only production GPU boundary.
- Keep CPU fallback code explicit and visible in telemetry. `--require-gpu`
  must never fall back silently.
- Add rocPRIM only when a benchmark proves it reduces code complexity or
  improves real throughput.
- Do not add OpenCL, SYCL, OpenMP offload, CUDA, or vendor-neutral GPU
  abstraction layers without a design review and explicit maintainer approval.
- Do not integrate hipCOMP-core into a release artifact while upstream still
  describes it as unsuitable for production workloads, unless the release notes
  and risk register explicitly accept that preview dependency.
- Benchmark mixed, compressible, and incompressible 10 GiB memory-only
  workloads before and after any backend change, then run the tiny filesystem
  storage smoke to verify installable app behavior.

## Practical Roadmap

1. Continue hardening the existing HIP block-classification, GPU pattern, GPU
   materialization, and fused CRC path.
2. Use rocPRIM selectively for ordered segment reductions or scans if it
   replaces custom HIP reduction code with a faster and simpler primitive.
3. Keep hipCOMP-core as a separate spike behind build-time flags only after its
   Windows HIP SDK requirements, license, security posture, and benchmark
   behavior are verified.
4. Publish performance claims only from required-GPU and forced-CPU benchmarks
   that disclose workload profile, memory-only status, backend telemetry, and
   regressions.
