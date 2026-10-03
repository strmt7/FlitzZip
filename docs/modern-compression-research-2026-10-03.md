# Modern Lossless Compression Research — 3 October 2026

This review identifies mechanisms worth testing in SuperZip. Security findings
remain the immediate development priority. None of these papers establishes a
SuperZip speedup, compression ratio, or compatibility claim. No external code,
model, kernel, runtime dependency, or archive representation was imported.

## Primary Sources And Transferable Ideas

| Work and publication evidence | Mechanism | SuperZip hypothesis and limit |
| --- | --- | --- |
| [Invariant Bit Packing](https://arxiv.org/html/2605.30728v1), EuroSys, April 2026; extended paper, 29 May 2026 | Remove bit positions constant across tensor groups, retain reconstruction metadata, and organize parallel decoding around transfer costs. | Test bounded, reversible bit-plane grouping only on blocks with measured redundancy. ML tensor statistics do not establish compressibility of arbitrary archive bytes. Its CUDA and zero-copy implementation is not an approved SuperZip backend. |
| [ZipServ](https://www.cse.ust.hk/~weiwa/papers/zipserv-asplos26.pdf), ASPLOS, March 2026 | Fixed-width bitmap representations exploit concentrated BF16 exponents; fused decompression and matrix multiplication avoid intermediate global-memory traffic. | Study bounded exception streams and fusion with an existing consumer, such as CRC or output materialization. Archive extraction must still emit complete original bytes; tensor-core computation and reported inference gains do not transfer to archiving. |
| [DFloat11](https://arxiv.org/html/2504.11651v1), April 2025 preprint version reviewed | Entropy-code exponents, use small staged lookup tables, and coordinate parallel decode positions. | Compare complete metadata cost and lookup latency. The paper's compact tables rely on skewed trees and frequency adjustment; a byte codec must handle every admitted symbol distribution and reject ambiguous tables. |
| [ZipNN](https://arxiv.org/abs/2411.05239v2), June 2025 revision, identified as IEEE Cloud in the author record | Lossless encoding specializes compression to neural-network representation and redundancy. | Evaluate reversible separation of structured byte fields against the unchanged candidate portfolio. Specialization needs an exact inverse and profitable complete payload, rather than an extension-based assumption about contents. |
| [Low-precision weights and caches](https://arxiv.org/abs/2508.19263), 20 August 2025 preprint | Encode exponent and mantissa components separately, including existing FP8 and FP4 representations. | Separation can expose entropy hidden by interleaved bytes. Compressing an existing low-precision representation losslessly does not authorize quantizing archive input or discarding original precision. |
| [Adaptive floating-point compression](https://arxiv.org/abs/2511.04140v2), 11 November 2025 preprint revision | Combine asynchronous transfers, an exact numerical transform within its domain, and sparse bit-plane handling of outliers. | Study transfer overlap and local exceptions. A numerical-domain conversion needs an explicit domain proof; generic archive bytes must preserve every bit pattern, including NaN payloads and signed zero. |

The Nature publisher page for *Lossless data compression by large models*
could not be retrieved: its identity-provider redirect returned a non-retryable
HTTP 500. It is not used as evidence for a design decision in this review.

## Ordered Experiments After Security Remediation

1. **Compact canonical Huffman metadata.** The current version-eight byte
   lookup occupies 8,192 bytes. A possible new representation stores 256
   code widths in 128 nibble-packed bytes and reconstructs canonical codes.
   The potential metadata reduction is 8,064 bytes per admitted block; this is
   arithmetic, not a measured archive improvement. Prove exact reconstruction,
   complete prefix coverage, absent-symbol handling, malformed-width rejection
   and bounded allocation before considering a new format version.
2. **Small bounded lookup alternatives.** Compare a reconstructed full table,
   staged tables and canonical decoding. The current HIP materializer assigns
   one thread to each 4 KiB segment and reads the global lookup; it does not
   already have a shared table per segment. Include table construction,
   auxiliary device storage, launches and transfers in the comparison.
3. **Reversible grouping and exceptions.** Evaluate byte-plane or bit-plane
   transforms with explicitly encoded layout and a bounded exception stream.
   Preserve raw and existing dictionary candidates. Choose a transform only
   when its complete encoded payload, including metadata and alignment, wins.
4. **Remove redundant passes.** Fuse only compatible stages with clear
   ownership and validation contracts. Measure transfer overlap against actual
   HIP scheduling and memory reservations; do not assume NVIDIA warp sizes,
   tensor-core instructions or ML inference consumers apply to AMD HIP.

All experiments must use byte-exact readback, authentic corpora alongside
mixed and incompressible controls, equal effort levels, RAM-only development
timing and disclosed resource costs. Retain all observations and use the
repository's scientific sampling protocol. A format change requires versioned
semantics, legacy reading and CPU/HIP malformed-input coverage. Security scan
coverage must remain intact while these hypotheses are evaluated.
