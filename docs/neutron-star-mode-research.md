# Neutron Star Mode Research

## Scope And Status

Neutron Star Mode is a lossless native SUZIP compression research feature.
The implementation is a bounded HIP dictionary primitive with separate
application, CLI and benchmark mode routing. Its GUI row follows the nine
numeric efforts and requires native SUZIP, actual cached HIP availability and
the required-GPU preference. Settings store the mode separately from the
numeric effort. The primitive retains the existing independent LZ4 block representation,
so it does not require a new archive block kind or change decoder semantics.
It is not a claim of universal minimum size, best-in-class compression or an
unmeasured GPU speedup.

## Minimum-Byte Parsing

The existing dictionary encoder uses a greedy parse. The stronger primitive
materializes verified matches from the exact-prefix index with at most 1,024
candidates and 65,535 compared bytes per position. Its matches remain inside
64 KiB independent segments and extend at most 65,535 bytes. A larger search budget
does not imply exhaustive discovery of every possible earlier match.

For a segment of length N, F(i) is the least encoded byte cost for its suffix
starting at i. M(j) is the least cost of a sequence starting with a match at j,
including its token, two-byte distance, match extension and following suffix.
The recurrences count complete LZ4 sequence costs:

```text
M(j) = 3 + min over valid lengths L [F(j+L) + extension(L-4)]
F(i) = min [1 + N-i + extension(N-i),
            min over j >= i [M(j) + j-i + extension(j-i)]]
extension(x) = 0 if x < 15, otherwise 1 + floor((x-15)/255)
```

Every selected match starts at least twelve bytes before the end and leaves
five final literal bytes, as required by the
[LZ4 block specification](https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md).
The longest verified match at a position supplies every shorter usable prefix;
all distances have the same two-byte wire cost. Choosing one verified longest
match therefore preserves the shortest parse of the admitted match graph.

Match extension costs are constant over intervals of at most 255 lengths. A
GPU minimum tree answers each interval exactly. For long literal runs, write
j = 255q+r and retain minima of M(j)+j+q by residue. Three residue intervals
account for the extension-cost carry relative to i. Runs shorter than fifteen
bytes are considered individually. This avoids a quadratic scan of future
match positions without dropping any admitted transition.

Descending parse tiles contain at most 32 positions per segment. Search
launches contain at most 4,096 source positions; output launches emit at most
64 sequences per segment. Each launch synchronizes before a caller checkpoint
can cancel. Windows driver timeout policy is unchanged. These are work bounds,
not a guarantee of a particular runtime on every GPU.

## Resource And Correctness Contracts

The input admission limit is 1 MiB. Tree, match, decision, writer, size and
output buffers are counted before the shared index reserves GPU memory;
source, radix-sort and packed-output allocations also count toward the
existing 256 MiB per-batch limit and process-wide device admission. The CPU
receives only completed payloads and bounded status words. It does not compute
or materialize the compression parse.

Qualification compares actual HIP output with an independent CPU oracle that
enumerates every legal literal/match transition on small inputs. Larger
segment and workspace boundaries use independent LZ4 decoding and production
HIP decoding. Cancellation must propagate during separate stages and release
workspace before later work succeeds. Empty input and absent HIP remain
explicit contracts. Product integration requires archive readback, policy and
cancellation checks in addition to the primitive qualification.

## Improvement Rounds And Isolation

Each Neutron round preserves an exact source identity, complete archive sizes,
CPU/HIP bytewise readback, memory admission and real HIP telemetry. Changes to
this mode do not change numeric effort policies. The ordinary numeric-level
benchmark protocol rejects a Neutron result. A later transfer to another level
requires controlled evidence of both smaller complete archives and higher
speed, followed by that level's consumer and compatibility checks.

The first round replaces greedy dictionary parsing with minimum-byte parsing
over its admitted match graph. The second round integrates this trial into the
native portfolio, retaining each ordinary GPU winner and replacing it only
when the complete framed block is smaller. The third round expands only Neutron
matches to the full 16-bit range and covers the additional extension-cost
plateaus with bounded lane strides. Further rounds must measure their
incremental effect against the preceding Neutron implementation; extra search
work alone is not evidence of improvement.

The fourth round changes only Neutron's private integer-minimum reduction.
Hardware wavefronts independently reduce cost/position keys using HIP shuffles,
then the first wavefront combines their staged minima. Two block barriers
replace nine per reduction. The actual hardware width admits the 32- and
64-lane paths; other widths retain the exact shared-memory reduction. Match
search, parse tiles, cancellation checkpoints, memory limits, deterministic
tie ordering and complete archive selection remain unchanged. Qualification
must retain exact output and exercise winners across wavefront boundaries.
[AMD's reduction guidance](https://rocm-handbook.amd.com/projects/amd-rocm-optimization-guide/en/latest/patterns/examples/reduction.html)
describes the two-phase reduction pattern. Real-device results remain specific
to the tested architecture; a supported code path does not establish testing
on every AMD GPU.

The fifth round classifies each completed match graph on HIP. A graph without
any usable match has exactly one legal parse: a literal-only sequence. The GPU
publishes that decision and its exact cost, then the existing GPU writer emits
and validates it. Host scheduling receives only bounded zero/one activity words.
It schedules parse tiles through the longest active segment; inactive segments
do not enter the dynamic program. An activity mask and the exact parse launch
count bind telemetry to source geometry. The gate is a proof over the admitted
graph, not a data-type heuristic or an omitted compression candidate. Active
graphs retain the complete previous minimum-byte search.

## Further Ratio Work

The application portfolio must retain level-nine GPU candidates and select
using complete block costs, including framing, so a stronger dictionary trial
cannot enlarge the chosen archive. Compact canonical entropy metadata and
reversible byte/bit-plane transforms are further research directions. Typed
floating-point transforms must preserve every original bit, including NaN
payloads and signed zero; lossy model quantization is outside this mode.

[AMD's HIP performance guidance](https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/performance_guidelines.html)
informs bounded parallel work and transfer accounting. The author manuscript
[A High-Throughput GPU Framework for Adaptive Lossless Compression of Floating-Point Data](https://arxiv.org/abs/2511.04140)
suggests adaptive bit-plane research, but its reported results are not SuperZip
or AMD HIP benchmark evidence. No external compression runtime, neural model
or new product dependency is adopted by this primitive.
