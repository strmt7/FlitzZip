# Compact Entropy Extension Design

This is the approved design for a Neutron-only GPU rANS candidate, proposed as
native block kind 14 (`GpuRans`) with minimum archive version 12. It is not yet
enabled in the application. The current [native format contract](native-flzip-format.md)
continues to describe the representations actually supported by production.

The extension replaces a fixed-size Huffman lookup with sparse integer models.
Whether that reduces a complete archive depends on its models, segment states,
offsets and payload. Adoption requires representative size evidence in addition
to independent correctness. Numeric efforts 1–9 retain their current behavior;
CPU reference code must never become a Neutron compression fallback.

## Payload

The containing descriptor supplies the decoded length `N`, which must be
nonzero and at most 16 MiB. Unused fill metadata is zero. A production plain
payload must be strictly smaller than `N`.

| Field | Representation and admission |
| --- | --- |
| Precision | u8, 8 through 16 |
| Context bits | u8, 0 through 8 |
| Segment shift | u8, 8 through 12; segments contain at most 256 through 4,096 decoded bytes respectively |
| Model count minus one | u8; count at most `2^context_bits` |
| Model row | Strictly increasing context id u8, then alphabet size minus one u8 |
| Singleton row | One symbol u8; frequency is implicitly `2^precision` |
| Other rows | Strictly increasing symbol u8 and positive frequency u16 LE pairs, summing exactly to `2^precision` |
| Segment offsets | `ceil(N / 2^segment_shift) + 1` u32 LE offsets relative to the stream region |
| Segment stream | Initial state u32 LE, followed by byte-aligned renormalization bytes |

Context zero must exist. The context is the high `context_bits` of the previous
decoded byte in the same segment; each segment starts with previous byte zero.
Zero context bits selects one unconditional model. Unused model rows are legal;
duplicate, out-of-range or missing actually used contexts are errors. There is
no missing-model fallback.

Use integer rANS with lower bound `L = 2^23` and states in `[L, 2^31)`.
Encoding visits symbols in reverse. Positive frequencies and the admitted
precision imply at most two byte emissions or reads per symbol; both directions
enforce that bound. A decoder produces exactly its declared segment length,
consumes every stream byte and finishes at state `L`. Singleton frequency
65,536 uses the implicit representation and is never narrowed into u16.

Offsets start at zero, end at the full stream extent, and describe dense,
disjoint streams. Each stream contains between four and
`2 * decoded_segment_length + 4` bytes. Prove fixed headers, models, offsets
and minimum state extents before allocation or pointer formation. Keep the
archive's independent decoded-data CRC: a mutated model can describe different
valid data, so entropy admission cannot replace data-integrity checks.

The writer normalizes integer counts by water filling with a one-frequency
floor and largest remainders, resolving equal remainders by ascending symbol.
Readers validate positive distributions and exact sums; they do not reconstruct
the writer's histogram or assume its particular normalization procedure.

## Compatibility and composition

Versions 1–11, their signatures, layouts and reserved bytes remain unchanged.
Version 12 requires an updated reader. A losing candidate retains the previous
winner and its existing minimum version; updating the application does not
gratuitously upgrade archive bytes.

One shared bounded version-admission contract must examine actual encoded
blocks and their fixed composition headers. The writer computes the maximum
while it owns each chunk's payload and carries that value into the final
index and footer. Inspecting outer descriptors alone is insufficient.

Kind 14 may be a direct block or a plain child of compound, byte-plane and
byte-plane-context frames. Existing closed stage sets expand only for this
plain kind; recursive compositions remain forbidden. Their existing header
layouts and zero reserved bytes remain unchanged. Compound headers identify
both stages even before the original payload has been recovered.

Before any decode dispatch or extraction publication, the reader checks every
advertised kind against the actual archive version. It rejects kind 14 inside
a version-1–11 frame, including an inner stage. Compound secondary decoding is
bounded, and the recovered original payload receives its own admission. All
existing intermediate-size, density, fill and extent checks remain mandatory.

## GPU execution and model reuse

Compression and required-GPU decoding execute through HIP. Host integer model
construction may coordinate device counts, consistent with the existing
entropy control plane. Portable CPU decoding is an independent reader, never
a compression substitute or evidence of device execution.

Each work item owns a bounded independent segment. No device-wide dependency
chain, global polling, cross-block synchronization or divergent barrier is
allowed. Buffers, streams and retained winners stay owned through actual
completion, including cancellation, failed copies, launches and host exceptions.
Failed completion must never be reported as successful cleanup or reuse.

Bound source scanning separately from entropy segments. Accumulate disjoint
source-tile ranges into one initially zero histogram and observe completion
between dispatches. Tile offsets refer to the complete source: a dispatch boundary
does not reset the previous byte. Reject empty, overlapping or out-of-range work
at the owning boundary. Publish reusable statistics only after every range has
completed and passed admission. Cancellation discards partial statistics; it
must not cache a successful source identity or release an in-flight allocation.
The dispatch bound does not reduce the supported decoded block size.

Build one full previous-byte transition histogram per admitted block, then
derive candidate contexts using those exact counts and the immutable source's
segment-boundary bytes. Counts alone cannot identify the predecessor changes at
particular segment boundaries. Avoid rebuilding a complete source histogram for
every precision and context option; its performance benefit remains a hypothesis
until measured. A bounded context-bucket implementation uses limited shared
storage and one full output histogram, rather than a dense histogram for every
segment in an entire file. Charge all histograms, models, trial streams, offsets
and retained winners to the existing resource-admission mechanisms. Do not
assume a device wave size, installation path or available VRAM.

### Exact model reuse

Let `T[p,s]` count source symbol `s` after previous byte `p`, with previous byte
zero for the first source byte. For one segment shift, copy this immutable table
into scratch storage. At each subsequent segment start, subtract that sample
from its actual predecessor row and add it to row zero. A predecessor already
equal to zero needs no change. Never correct the first source byte again.

Only after correction, merge predecessor rows sharing the selected high context
bits. Equivalently, for context `c`, let `B[c,s]` count segment-start samples whose
original predecessor maps to `c`. Then the candidate counts are
`H[c,s] = sum(T[p,s] for p in c) - B[c,s] + (sum(B[*,s]) if c == 0 else 0)`.
These are exact integer identities, not estimates or classifier predictions.

Unconditional counts (`context_bits == 0`) are independent of the segment shift:
all predecessor rows merge into context zero and the corrections cancel. Reuse
that model across shifts at a fixed precision. More generally, counts do not
depend on precision. Reuse the same corrected counts while constructing the
admitted precisions; normalize separately for each distinct distribution and
precision. This never authorizes reuse of encoded segment streams across shifts,
whose state resets, offsets and tails differ.

Keep one immutable base table, disposable correction storage and bounded reusable
projection storage. Subtraction requires an existing count; reject underflow or
mass overflow and discard faulted scratch before reuse. Unobserved contexts must
remain absent, including when a boundary correction removes their last sample.
Reuse is bound to exact input bytes and the stage that produced them. A different
transform output requires fresh statistics even when its length is unchanged.
All source and table owners survive actual stream completion. Independent direct
counting must challenge every context width and shift, zero predecessors, segment
tails and disappearing contexts. Omitted-work and deliberately incorrect
corrections must fail that same oracle before this path can be adopted.

### Model-to-stream integration

Keep normalization, cumulative-model construction and entropy coding as distinct
bounded operations with explicit status and extent admission. A valid nonempty
row must produce a monotonic cumulative distribution starting at zero and ending
at exactly `2^precision`. Empty rows remain absent. An upstream failure must not
publish a usable model, and singleton mass 65,536 remains a full-width integer
until its implicit wire representation is serialized.

Qualification must connect actual device outputs: normalization consumes the GPU
counts, model construction consumes the GPU frequencies, encoding consumes those
models, and decoding consumes the encoder's actual workspace and returned extents.
CPU-built models and streams are comparison oracles, never substitute inputs for
that integrated proof. Compare the complete serialized payload, including sparse
rows, offsets and states, against an independent encoder. Also compare raw
transition counts directly: a singleton's normalized model could otherwise hide
an incorrect histogram mass. Use safe omitted-work and arithmetic controls to
establish that these observations detect failures at the intended stage.

## Adoption criteria

The isolated reference, independent public-domain
[rANS oracle](https://github.com/rygorous/ryg_rans/tree/c9d162d996fd600315af9ae8eb89d832576cb32d)
and HIP prototype are development evidence, not a released format implementation
or proof of a compression advantage. Keep exact source, tool, input and output
identities with their local records.

Before production adoption:

1. Qualify independent encoding and decoding across admitted precisions, context
   families, segment tails and singleton models. Exercise malformed tables,
   offsets and states, maximum bounds, cancellation, failure cleanup and reuse.
   Prove dispatch with exact oracle output and deliberately omitted-work controls.
2. Independently construct direct and every admitted inner-stage archive frame.
   Reject downgraded versions before dispatch or publication; preserve old archive
   readback and numeric-effort behavior.
3. Run normal provenance, complexity, scanner and affected correctness gates.
   HIP-host static analysis does not constitute device-kernel validation.
4. When benchmark execution is authorized, use the existing licensed RAM-only
   corpus controller and independent held-out workload classes. Count every
   model, stream, frame and archive byte, retain losses, and verify original bytes.
   Ties retain the preceding winner. Qualify comparative speed separately.

Revise or reject the design if its complete costs do not justify adoption.
Design approval grants no new runtime dependency, scanner disposition, change
to numeric efforts or permission to publish a release.
