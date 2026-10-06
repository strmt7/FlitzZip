# GPU Entropy Winner Preparation

Entropy selection now measures each candidate with a checked scalar byte total
and materializes segment offsets only for the final winner. Previously every
block allocated a scratch offset vector, and each strict improvement copied
that vector into the block plan. Blocks without a winning candidate still paid
for scratch storage.

One shared measurement helper handles both the allocation-free comparison and
the winning offset pass. It checks candidate/storage shape, measured-length
access and 32-bit offset overflow. Selection still compares the complete
codebook, offset table and bitstream size. The strict comparison preserves
the earlier candidate and baseline on ties. The winning tables, payload layout,
GPU kernels and SUZIP format versions are unchanged.

## Component Evidence — 3 October 2026

An isolated C++20 diagnostic compiles the exact before/after production host
functions with MSVC 19.51 in Release mode. The fixture covers all seven product
block sizes, 1/8/16 candidates, and five selection shapes: no improvement, only
the first improvement, successive improvements including Huffman, tied sizes
and zero-length candidates. The 105 comparisons preserve every selected kind,
table index, bitstream size, use flag, offset and serialized codebook byte.
Another 210 paired checks retain missing-length and offset-overflow rejection.
This diagnostic is separate from production HIP execution.

Global allocation instrumentation around the selection call observes:

| Selection result | Before allocations | After allocations |
| --- | ---: | ---: |
| No entropy winner | 1 | 0 |
| Adaptive-prefix winner | 2 | 1 |
| Huffman winner | 3 | 2 |

Every observed case removes exactly one offset-vector allocation. Its logical
size ranges from 260 bytes at 256 KiB blocks to 16,388 bytes at 16 MiB blocks.
Actual allocator traffic includes implementation overhead: the largest removed
allocation requests 16,427 bytes on this MSVC implementation. An independent
scratch-vector control measures that overhead instead of assuming allocated
bytes equal logical bytes. These are allocation traffic, not process peak RAM.
Repeated copies on intermediate improvements are also removed.

The initial diagnostic extractor mishandled a default span argument and omitted
a serializer dependency; its isolated build failed. The corrected build then
caught an allocation-byte assertion that ignored MSVC's large-vector alignment
overhead. The final diagnostic measures its independent allocation control and
passes. Both failures remain retained; neither was a production test failure or
discarded timing observation.

No timing comparison was performed. The allocation result does not establish
archive throughput, kernel speed, compression-ratio improvement or a speedup
on any GPU. The second pass reads only the winning candidate's immutable length
data; a future timed study must account for that work as well as removed
allocations and copies. Historical benchmark records keep their original
source and binary identities.

## Production Verification

The ordinary HIP-enabled Release build passes, including all configured device
images. All 17 affected `hip-entropy-encode` component cases pass against the
production implementation, with required-HIP readiness checked by the canonical
runner. They include the entropy portfolio, all nine effort levels, per-block
selection, mixed chunks, reference packing/read-back, Huffman and malformed
payload/table rejection. Changed-input hygiene, the pinned Gitleaks/DevSkim
preflight, language lint and changed-function documentation/complexity gates
also pass. No unrelated native suite or benchmark was rerun.

The frozen baseline, exact-source diagnostic and complete allocation results
remain under ignored `out/entropy-selection-*` paths. The final diagnostic
receipt is `out/entropy-selection-isolated-contract-v3-20261003.json`; the
production receipt is `out/entropy-selection-component-verification-20261003.json`.
Hosted exact-SHA qualification and the open-finding security gate remain separate
acceptance work. This component result does not qualify the beta for release.
