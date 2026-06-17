# GPU CRC and Integrity Research

SuperZip needs integrity checks that are correct before they are fast. The
archive format currently uses ZIP-compatible CRC-32 for accidental corruption
detection and an opt-in SHA-256 file hash through Windows CNG for stronger
integrity checks. Any GPU integrity path must preserve those semantics and must
not exist only to inflate GPU utilization counters.

## Sources Reviewed

- AMD HIP asynchronous execution documentation:
  <https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/hip_runtime_api/asynchronous.html>
- AMD HIP performance guidance on streams:
  <https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/performance_guidelines.html>
- AMD rocPRIM documentation:
  <https://rocm.docs.amd.com/projects/rocPRIM/en/docs-7.1.1/index.html>
- rocPRIM reduce documentation:
  <https://rocm.docs.amd.com/projects/rocPRIM/en/latest/device_ops/reduce.html>
- rocPRIM scan documentation:
  <https://rocm.docs.amd.com/projects/rocPRIM/en/docs-6.0.0/device_ops/scan.html>
- zlib CRC-32 source and combine implementation:
  <https://github.com/madler/zlib/blob/develop/crc32.c>
- zlib manual for `crc32_combine`:
  <https://www.zlib.net/manual.html>
- NVIDIA nvCOMP CRC32 API documentation, used only as evidence that GPU
  compression libraries treat CRC as a chunked low-level primitive:
  <https://docs.nvidia.com/cuda/nvcomp/crc32.html>
- Shieh et al., "A Systematic Approach for Parallel CRC Computations":
  <https://caslab.ee.ncku.edu.tw/dokuwiki/_media/research%3Acaslab_2001_jnl_01.pdf>
- Liu et al., "An Efficient Parallel CRC Computing Method for High Bandwidth
  Networks and FPGA Implementation":
  <https://www.mdpi.com/2079-9292/13/22/4399>
- BLAKE3 Internet-Draft:
  <https://www.ietf.org/archive/id/draft-aumasson-blake3-00.html>

## Findings

CRC-32 is mathematically parallelizable, but not by running the ordinary byte
loop in many unrelated GPU threads. Correct parallel CRC splits the message into
ordered segments, computes a CRC for each segment, and combines the segment CRCs
with GF(2) polynomial operators using the exact length of each following
segment. SuperZip already has a tested CPU `crc32_combine` implementation for
this concatenation step.

AMD's available primitives are useful building blocks, not a complete CRC
library. rocPRIM provides HIP device-level reductions, segmented reductions, and
scans. A CRC concatenation operator can be made associative when each item
carries both CRC state and byte length, but ordered combination must be handled
carefully because CRC concatenation is not commutative.

The best GPU design for SuperZip is a fused path:

1. While a chunk is already resident on the AMD GPU for classification or
   materialization, compute fixed-size segment CRCs in the same stream.
2. Return only segment CRC states and byte lengths to the host, or reduce them
   on-device with an ordered associative combine operator.
3. Combine chunk CRCs on the host with the existing `crc32_combine` function so
   archive-level CRC semantics remain identical.
4. Benchmark against the current CPU CRC on the 10 GiB mixed, compressible, and
   incompressible workloads before enabling it by default.

A standalone GPU CRC pass that copies data only for checksum work is not an
enterprise-quality optimization for this project. It increases launches and
transfer pressure and can make the archive path slower while still reporting
GPU activity. That design is rejected unless a benchmark proves otherwise on
the required AMD HIP lane.

## Integrity Algorithm Options

CRC-32 remains required for ZIP compatibility and for the current `.suzip`
metadata. It is fast and good for accidental corruption detection, but it is not
a cryptographic integrity mechanism.

SHA-256 through Windows CNG is the current opt-in strong integrity check. It is
portable across supported Windows systems, uses OS-maintained cryptographic
code, and is the right default trust baseline.

BLAKE3 is designed for high parallelism and is a plausible future opt-in
integrity mode for very large archives. Adding it would require a format and UI
decision because it is a different digest algorithm, not a replacement for ZIP
CRC-32. It must be implemented from a maintained, audited source or a reviewed
in-house implementation with test vectors before it can be considered for a
release.

## Current Implementation

SuperZip now computes source CRC-32 inside the HIP encode path while the source
chunk is already resident on the AMD GPU. Verification can also compute CRC-32
over decoded no-deflate chunks in VRAM and copy back only compact CRC segment
metadata instead of copying the full decoded chunk only to checksum it.
CPU-deflate payloads remain a CPU codec feature. A required-HIP verification or
extraction rejects those blocks instead of invoking miniz, while optional GPU
mode may fall back completely to the CPU codec.

The current implementation is intentionally conservative:

- The archive format still stores ZIP-compatible CRC-32.
- The host combines chunk CRCs with the tested `crc32_combine` implementation.
- `--require-gpu` still fails if the HIP path cannot run.
- The benchmark output must expose HIP kernel launches, HIP event time,
  transfer bytes, device allocation bytes, and whether the run was memory-only.

Measured results are profile-dependent. The fused CRC path improves cases where
GPU fill/pattern classification avoids large decode-to-host checksum copies.
After aligning the RAM-only benchmark with production worker allocation,
compressible data is slightly faster on required HIP on the current AMD test
host, while mixed and incompressible data are tied or slower. Incompressible data
carries HIP transfer and launch overhead without finding enough compressible
structure. That is an optimization finding, not a result to hide or relabel.

This implementation is not a full GPU deflate or hipCOMP-core production codec.
Deflate blocks are owned by the CPU codec, required-HIP archives avoid them, and
hipCOMP-core remains a research candidate until its upstream production-readiness
warning and Windows packaging risk are resolved.

## Future Implementation Gate

Additional GPU integrity algorithms or broader GPU CRC reductions should be
promoted only after all of these are true:

- The implementation computes the same CRC-32 as `crc32()` for randomized,
  compressible, incompressible, odd-length, empty, and multi-chunk data.
- Required-GPU compression reports nonzero CRC kernel telemetry without adding
  a second host-to-device copy of the same source bytes.
- CPU-only and GPU-required 10 GiB memory-only benchmarks show measured results
  for mixed, compressible, and incompressible profiles with regressions reported
  explicitly.
- Verification and extraction still reject corrupt payloads before reporting
  success.
- The code remains AMD HIP only and builds on a host with a supported AMD GPU
  without CUDA, NVIDIA libraries, or deployment-specific assumptions.
