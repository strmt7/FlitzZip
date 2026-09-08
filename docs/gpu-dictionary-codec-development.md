# GPU Dictionary Codec Development

## Scope

The dictionary encoder and decoder are test-target-only HIP implementations. They are not
linked into the application or CLI, do not emit SUZIP archive blocks, and
do not change the native format version. Production HIP compression still
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
  literal bytes cooperatively. A 256-position shared-memory cache retains
  lengths and distances across short matches in tiled mode. Longer matches
  skip unused tiles; tiled encoding does not allocate or populate a global
  match record for every input byte. Every refill is cooperative, with a barrier before
  changing bounds so all lanes make the same refill decision.
- The output uses the documented
  [LZ4 block format](https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md),
  including little-endian distances, extended lengths, final literal-only
  sequences, five final literals, and the twelve-byte final-match restriction.
  It is not an LZ4 frame or a new public compatibility format.
- The encoder downloads only used payload bytes and four bytes of length
  metadata per segment. The separate diagnostic matcher can download match
  records, but encoding does not take that path.
- Compared with dense search, tiled global workspace falls by eight bytes per
  source byte: 512 KiB for a 64 KiB input and 32 MiB for a 4 MiB batch. The
  cache uses 1 KiB of static shared memory per thread block, separate from the
  reported global allocation count. Search budgets, nearest-distance ties,
  sequence selection, and encoded bytes are unchanged.
- Automatic mode retains dense parallel search for small batches and low
  efforts. It selects tiled search when input bytes times the per-position
  comparison budget reaches 8 GiB: level 9 at 1 MiB, level 8 at 2 MiB, or
  level 7 at 4 MiB. This product is a work-bound proxy, not an actual read
  counter. The crossover policy follows local repeated comparisons; it is
  not a hardware-independent optimum. Explicit dense and tiled modes remain
  available in this internal API for equivalence checks and future tuning.
- Batches are capped at 4 MiB, total reserved device workspace at 256 MiB,
  and each encoded slot at 65,809 bytes. Existing aggregate HIP reservations
  also apply. Capacity failure rejects the batch instead of exposing output.
- Missing HIP is an explicit error for nonempty encoding. Empty input needs
  no GPU operation. No CPU dictionary compression fallback is present.

## Required-HIP Decoder

The decoder accepts at most 64 independent segments and 4 MiB of decoded
bytes. Each nonempty segment declares at most 64 KiB of output and 65,809
encoded bytes. Host-side extent admission precedes GPU allocation; HIP then
validates sequence lengths, backward distances, complete input consumption,
exact output lengths, and final-literal restrictions. It downloads output only
after every segment has succeeded. A failed segment cannot expose a partially
decoded batch.

One thread block handles each segment. Literal bytes are copied cooperatively;
overlapping matches read the already materialized distance-byte period instead
of depending on concurrently written match bytes. All copies remain within
the segment, with barriers between sequence phases. Packed input, descriptors,
statuses, and output together use less than 9 MiB of device workspace at the
maximum batch size, subject to aggregate HIP memory reservation limits.
There is no CPU decode fallback. Empty batches need no GPU work, and invalid
device-event timing remains explicitly unavailable.

Existing encoder fixtures now decode on HIP as well as through the independent
bytewise CPU reference. Additional tests cover an external python-lz4 4.4.5
fixture, full-segment overlapping matches longer than the encoder's current
8 KiB search limit, distance 65,524, truncated sequences and extensions,
inconsistent sizes, unused final-token match bits, and recovery after a
rejected mixed batch. The unused bits follow independent LZ4 reader behavior;
they do not introduce a match in the final literal-only sequence. These checks
are not yet archive-format integration or a decoder speed claim.

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

## Encoder Timing Comparison

On 2026-09-08, the HIP-enabled Release diagnostic compared dense and automatic
search in four alternating pairs across 162 cases: all nine levels, six batch
sizes, and three deterministic profiles. Hardware was a Radeon RX 9070 XT
with driver 32.0.31041.1004 and Ryzen 9 9950X. These are isolated experimental
encoder calls, not application throughput or a CPU-versus-GPU comparison.
All cases preserved payload sizes. Selected level-9 results follow; speedup
is the median of the four paired ratios, not the ratio of the two medians.

| Input | Profile | Payload Bytes | Dense Median ms | Automatic Median ms | Paired Speedup |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 MiB | Repeated 16 KiB record | 266,736 | 5.60 | 3.60 | 1.57x |
| 1 MiB | Period 13 | 4,896 | 6.37 | 3.67 | 1.74x |
| 2 MiB | Repeated 16 KiB record | 533,472 | 10.53 | 5.08 | 2.09x |
| 2 MiB | Period 13 | 9,792 | 11.95 | 4.50 | 2.62x |
| 4 MiB | Repeated 16 KiB record | 1,066,944 | 19.69 | 8.01 | 2.51x |
| 4 MiB | Period 13 | 19,584 | 21.67 | 6.75 | 3.35x |

The 4 MiB level-9 paired ranges were 2.00-2.86x and 3.07-3.87x respectively.
Random-data timings were mixed and do not establish a speed improvement.
Earlier explicit-tiled comparisons showed substantial small-input slowdowns;
automatic mode therefore retains dense search for those regimes. No output
padding, reduced effort, format change, or CPU compression fallback is involved.

Resource context was sampled throughout: CPU 1.8-28.3%, available RAM
12,630-14,940 MiB, and disk queue 0-2. GPU counter refresh handled process
churn without treating missing values as zero. Eleven of 53 samples lacked
one or more GPU deltas; the short-lived benchmark children themselves did
not yield paired GPU utilization samples. Thus the sampled background GPU
values are not benchmark utilization, nor proof of exclusive GPU access.
Host-wall timings include synchronization, but valid device-event timing and
broader GPU hardware measurements remain open. Ordinary host variation is
not grounds to label small timing differences a regression or a speedup.

## Verification And Remaining Work

Native tests cover hand-derived golden blocks, all nine effort sizes, an
independent bytewise decoder, overlapping copies, literal-length extensions,
segment boundaries, sparse tile matches, maximum batches, resource admission,
and missing-HIP behavior. Dense diagnostic matches also feed an independent
serial reference packer at all nine efforts, checking exact encoded bytes and
global workspace accounting across cache and segment boundaries.
Opt-in fixture export uses
`SUPERZIP_DICTIONARY_INTEROP_EXPORT`, refuses existing output filenames, and
caps total writes at 64 MiB. Ordinary tests write no dictionary fixtures.

`SUPERZIP_DICTIONARY_BENCHMARK=1` enables the otherwise inactive
`dictionary_encoder_benchmark_opt_in` test. It measures the full host-wall
`encode_segments` call, including allocation, indexing, packing, transfers,
and synchronization, after a warmup. Independent CPU and required-HIP
roundtrip checks run outside each timed span. The fixed RAM-only cases cover
64 KiB and 4 MiB batches, random bytes, repeated 16 KiB records, period-13
data, and efforts 1, 5, and 9. `SUPERZIP_DICTIONARY_BENCHMARK_EXTENDED=1`
expands coverage to all nine efforts and 64, 128, 512, 1024, 2048, and
4096 KiB batches. `SUPERZIP_DICTIONARY_TILED=1` selects explicit tiled search;
`SUPERZIP_DICTIONARY_AUTOMATIC=1` takes precedence and selects automatic
mode. Without either switch the diagnostic uses dense search. Fixture export is forbidden in that mode.
Use a resource-monitoring harness and repeated alternating binaries; this
diagnostic is not a production archive benchmark or device-event timing.

All 320 native tests passed after cooperative tile search was added. An
external `python-lz4` 4.4.5 decoder independently restored 242 exported blocks
covering 14,025,752 decoded bytes. The initial 236 blocks also remained encoded
byte-for-byte identically after the tiled-search rewrite.
The external decoder was installed only in ignored development output;
no Python or LZ4 runtime dependency was added to the product. Repeat external
interoperability validation after encoding changes, not just the internal
roundtrip test.

After the cooperative decoder and final-token compatibility correction, all
337 native tests passed in the HIP-enabled Release build. The 36-format CLI
matrix, independent standard-format readers, benchmark-reporting tests,
changed-function contracts, and portable packaging checks also passed.
These are correctness gates, not controlled throughput measurements.

After the dense/tiled strategy revision, all 356 native tests passed in the
HIP-enabled Release build. The external python-lz4 4.4.5 decoder restored
356 exported blocks covering 16,540,373 decoded bytes; all 242 preserved
baseline block/raw pairs remained byte-identical. New checks cover all-nine
effort reference packing, short tails, cache/segment boundaries, invalid
strategy values, exact global workspace accounting, and automatic crossover
boundaries. These results do not satisfy the remaining archive integration,
release, or deferred security gates below.

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
5. Production telemetry conversion and accumulation now reject invalid numeric
   durations without failing archive work. A standalone HIP runtime probe also
   reproduced negative timings outside SuperZip on the development host;
   reliable device-event timing still needs isolation before timing claims.
6. Complete archive integration, full release gates, and the deferred security
   work before making production or clean-security claims.
