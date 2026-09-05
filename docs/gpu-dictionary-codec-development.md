# GPU Dictionary Codec Development

## Scope

The dictionary encoder is a test-target-only HIP implementation. It is not
linked into the application or CLI, does not emit SUZIP archive blocks, and
does not change the native format version. Production HIP compression still
has the two prefix-code effort tiers described in
[the compression policy](compression-level-and-benchmark-suite.md).

## Implemented Path

- Exact four-byte prefix keys, ordered by segment and source position through
  SDK-supplied rocPRIM radix sorting, form deterministic backward match chains.
- Levels 1-9 have candidate budgets of 1 through 256, doubling at each level.
  Their byte-comparison budgets likewise double from 32 through 8,192.
- Matches remain within independent 64 KiB segments, have lengths 4-8,192,
  and use bounded, alignment-safe word comparisons with a byte tail.
- HIP selects the next match in fixed-size cooperative tiles and packs the
  literal bytes cooperatively. Empty lanes do not scan the full remaining
  suffix again for every selected match.
- The output uses the documented
  [LZ4 block format](https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md),
  including little-endian distances, extended lengths, final literal-only
  sequences, five final literals, and the twelve-byte final-match restriction.
  It is not an LZ4 frame or a new public compatibility format.
- The encoder downloads only used payload bytes and four bytes of length
  metadata per segment. The separate diagnostic matcher can download match
  records, but encoding does not take that path.
- Batches are capped at 4 MiB, total reserved device workspace at 256 MiB,
  and each encoded slot at 65,809 bytes. Existing aggregate HIP reservations
  also apply. Capacity failure rejects the batch instead of exposing output.
- Missing HIP is an explicit error for nonempty encoding. Empty input needs
  no GPU operation. No CPU dictionary compression fallback is present.

## Size Evidence

The deterministic repeated-record test contains 65,536 bytes: a seeded,
full-alphabet 16 KiB record repeated four times. The RAM-only block results
from the September 2026 implementation are:

| Effort | Encoded Payload Bytes |
| ---: | ---: |
| 1 | 21,918 |
| 2 | 19,347 |
| 3 | 17,947 |
| 4 | 17,214 |
| 5 | 16,934 |
| 6 | 16,791 |
| 7 | 16,719 |
| 8 | 16,694 |
| 9 | 16,671 |

These are actual encoded bytes with independent byte-exact decoding, not
match-length estimates. They exclude archive/container metadata. They prove
distinct effort outcomes on this fixture, not distinct sizes for every input.
The existing production HIP codec at level 9 stores 65,536 payload bytes on
the same fixture; the experimental level-9 block stores 16,671 bytes.
Lower settings are not padded or deliberately degraded. Incompressible or
already optimally represented data can legitimately have equal sizes.

No speed conclusion is drawn from these size checks. The maintainer initially
deferred timing during heavy host use, then re-enabled benchmarks with CPU,
GPU, RAM, and storage contention checks before and throughout measurements.
Correctness and size checks remain separate. Negative HIP event durations were observed in earlier
diagnostics; the experimental matcher now represents negative and non-finite
durations as unavailable, not zero. Runtime errors still propagate.

## Verification And Remaining Work

Native tests cover hand-derived golden blocks, all nine effort sizes, an
independent bytewise decoder, overlapping copies, literal-length extensions,
segment boundaries, sparse tile matches, maximum batches, resource admission,
and missing-HIP behavior. Opt-in fixture export uses
`SUPERZIP_DICTIONARY_INTEROP_EXPORT`, refuses existing output filenames, and
caps total writes at 64 MiB. Ordinary tests write no dictionary fixtures.

All 320 native tests passed after cooperative tile search was added. An
external `python-lz4` 4.4.5 decoder independently restored 242 exported blocks
covering 14,025,752 decoded bytes. The initial 236 blocks also remained encoded
byte-for-byte identically after the tiled-search rewrite.
The external decoder was installed only in ignored development output;
no Python or LZ4 runtime dependency was added to the product. Repeat external
interoperability validation after encoding changes, not just the internal
roundtrip test.

Before production integration:

1. Add the versioned native block contract, bounded CPU and HIP readers,
   archive-level corruption handling, and backward-reader coverage.
2. Select dictionary blocks against existing raw/fill/pattern/prefix choices,
   counting complete metadata costs. Do not replace smaller existing blocks.
3. Record pinned rocPRIM production provenance and required license notices.
   Current tests use the installed HIP SDK headers, reporting rocPRIM 4.1.0.
4. Validate hardware/configuration portability and controlled end-to-end speed,
   resource usage, and size across representative held-out workloads. No local
   GPU result proves support for other architectures.
5. Audit the production telemetry conversion separately: experimental timing
   validation does not repair the existing unchecked milliseconds-to-integer
   conversion in `record_gpu_kernel_launch`.
6. Complete archive integration, full release gates, and the deferred security
   work before making production or clean-security claims.
