# Neutron star mode Research

## Scope And Status

Neutron star mode is a lossless native SUZIP compression research feature.
The implementation is a bounded HIP dictionary primitive with separate
application, CLI and benchmark mode routing. Its GUI row follows the nine
numeric efforts and requires native SUZIP, actual cached HIP availability and
the required-GPU preference. Settings store the mode separately from the
numeric effort. Its minimum-byte dictionary primitive retains the independent
LZ4 block representation. Neutron additionally evaluates a closed second GPU
encoding stage over the selected payload; a strictly smaller complete result
uses the [version-nine compound representation](native-suzip-format.md).
Neutron also evaluates exact byte-plane permutations at widths two, four and
eight against that winner, using the closed version-ten representation only
for a smaller complete result. Transform and inverse execute through HIP.
The version-eleven context representation separately codes byte planes so
different field distributions do not share one pooled entropy model. Current
[public-corpus evidence](neutron-public-corpus-benchmark.md) qualifies complete
file sizes and readback on its stated workloads; broader qualification remains open.
Both required-HIP decode stages execute on the GPU. The independent CPU
decoder verifies portability without providing a Neutron creation fallback.
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

The minimum-byte primitive admits at most 1 MiB per batch; archive dispatch
iterates admitted batches inside the existing native block and chunk limits.
Tree, match, decision, writer, size and
output buffers are counted before the shared index reserves GPU memory;
source, radix-sort and packed-output allocations also count toward the
existing 256 MiB per-batch limit and process-wide device admission. The six
parser arrays share one HIP allocation with disjoint, compile-time-aligned
views. They retain their exact admitted byte extents and synchronized lifetime;
normal completion and cancellation release the same owner. This removes five
allocation/free pairs per trial without changing search, kernels, archive bytes
or numeric efforts. Allocation-byte telemetry and the conservative reservation
remain unchanged; fewer runtime calls alone do not establish a speedup. The CPU
receives only completed payloads and bounded status words. It does not compute
or materialize the compression parse.

Qualification compares actual HIP output with an independent CPU oracle that
enumerates every legal literal/match transition on small inputs. Larger
segment and workspace boundaries use independent LZ4 decoding and production
HIP decoding. Cancellation must propagate during separate stages and release
workspace before later work succeeds. Empty input and absent HIP remain
explicit contracts. Product integration requires archive readback, policy and
cancellation checks in addition to the primitive qualification.

## Portfolio And Isolation

Each candidate preserves an exact source identity, complete archive sizes,
CPU/HIP bytewise readback, memory admission and actual HIP telemetry. Neutron
retains the ordinary GPU portfolio winner and replaces it only when a complete
framed candidate is smaller. The numeric-level benchmark protocol rejects a
Neutron result. Transferring an improvement to another level requires controlled
evidence of both smaller archives and faster execution, followed by that level's
consumer and compatibility checks.

The private minimum reduction first combines cost/position keys within each
hardware wavefront, then combines staged minima across wavefronts. Supported
32- and 64-lane paths preserve exact tie ordering; other widths use the exact
shared-memory reduction. The operation remains bounded by the same parse tiles,
cancellation checkpoints and memory admission.
[AMD's reduction guidance](https://rocm-handbook.amd.com/projects/amd-rocm-optimization-guide/en/latest/patterns/examples/reduction.html)
describes this two-phase pattern. Supporting a width in source does not establish
runtime qualification on every device.

HIP classifies completed match graphs before dynamic programming. A graph with
no usable match has only its literal parse; the GPU publishes and emits that
exact result. Host scheduling receives bounded activity words and skips only
proved inactive work. This is a proof about the admitted graph, not a file-type
heuristic. Competitive graphs retain the complete minimum-byte search.

## Research Priorities

The current [standard-format references](neutron-public-corpus-benchmark.md#standard-format-size-reference)
show the remaining workload-specific gap: Canterbury needs 723,429 complete
Neutron bytes, versus 543,236 for TAR/bzip2 and 494,376 for TAR/XZ. Pythia14M
benefits from separate byte contexts, but one trained checkpoint does not
establish general effectiveness. Measure each proposal against complete framed
costs, not kernel activity, unframed entropy estimates or a paper's headline.

The self-hosted Crawl4AI research batch retrieved twenty primary-source pages;
selected method and limitation passages informed these hypotheses. The full
page captures and access metadata remain local. These are research directions,
not implemented codecs or measured SuperZip speedups:

- **Separate predictable fields from residual bits.**
  [DFloat11](https://arxiv.org/html/2504.11651),
  [exponent concentration](https://arxiv.org/html/2510.02676),
  [Invariant Bit Packing](https://arxiv.org/html/2605.30728) and
  [Unweight](https://research.cloudflare.com/nikulin2026/) exploit structure in
  trained numerical representations. Test reversible constant-bit/bit-plane
  contexts with explicit masks, outliers and table overhead. Preserve headers,
  signed zero, NaN payloads and every original bit. Existing low-precision model
  results do not authorize quantization of input files.
- **Improve context and entropy coding.**
  [ZipNN](https://github.com/zipnn/zipnn) and
  [Falcon](https://arxiv.org/html/2511.04140) motivate exact representation
  selection. A byte permutation alone leaves a pooled histogram unchanged;
  separate contexts must justify their tables and offsets. Add an independent
  decoder and malformed-frame checks before introducing a representation.
- **Account for the decoder model.**
  [Nacrith](https://arxiv.org/html/2602.19626) combines predictors with arithmetic
  coding, but its reported text sizes exclude a large shared model and its paper
  discusses training-data overlap. A self-contained archive comparison must
  include required model bytes and deterministic cross-host reconstruction.
  No neural-model runtime or implicit external decoder dependency is adopted.
- **Distinguish archive cost from accelerator microbenchmarks.**
  [RAS](https://arxiv.org/html/2511.04684) reports a simulated hardware design;
  [absolute-offset LZ work](https://arxiv.org/html/2607.18541) measures a match
  phase with a different offset representation. Neither establishes end-to-end
  HIP speed or a new cost rule for Neutron's fixed two-byte LZ4 distances.
  Include acquisition boundaries, transfers, all coding stages and decoding.

Use the existing licensed public corpora and exact RAM-only engine for each
candidate. Select transforms from original bytes and measured complete costs,
never filenames, extensions or corpus identities. Test text, mixed documents,
models and already-compressed data; retain losing and failed observations
locally. Additional dependencies and format changes follow the existing
approval and compatibility contracts.

## Exact Dictionary Work Admission

The current experiment uses the complete verified match graph to bound the
number of source bytes that every legal LZ4 parse must emit as literals. A
parallel prefix maximum forms the union of match intervals; uncovered positions
must remain literal bytes. For `L` such bytes, the payload costs at least
`L + 1 + ceil(max(0, L - 14) / 255)`. Every sequence has a token, every additional
sequence has a two-byte offset, and each extension byte accommodates at most
255 further literals after the fourteen-byte extension-free allowance. Adding
another sequence costs more than the extension saving it can provide.

Only when the sum of these conservative segment prices reaches a complete
group's exclusive payload limit can that dictionary candidate be discarded.
The limit excludes its offset-table framing and comes from the preceding
measured winner. Competitive groups retain the full existing optimal parser;
ordinary numeric efforts are untouched. HIP performs the graph analysis and
literal materialization, with the existing workspace and per-launch limits.
Regression coverage includes competitive output, losing nonempty graphs,
partial tails, budget admission and cancellation/recovery. Pruning changes no
representation and admits no heuristic omission of a competitive graph.

## Independent Entropy Contexts

The version-eleven closed context frame trials two byte planes independently
through the existing plain HIP portfolio. Its reader supports widths two,
four and eight, derived plane extents and exact tail reconstruction. Mixed
raw/fill/pattern fixtures, malformed tables, version admission and independent
CPU/HIP readers cover this boundary. Every context's framing participates in
selection; the preceding winner remains when the complete candidate does not
improve size. Numeric efforts remain unchanged.

The [current complete-checkpoint study](benchmarks/data/neutron-pythia14m.json)
records Pythia14M sizes and byte-exact required-HIP readback. More contexts may
reduce unframed entropy yet lose after extra tables and offsets. Bit-field
contexts remain prospective; neither histogram estimates nor an upstream
inference benchmark is an encoded SuperZip archive.

## Prospective Match-Sequence Bound

A stronger lower bound can additionally count unavoidable match-token and
offset costs. Private oracle observations remain local; this is a prospective
derivation, not a production implementation or GPU performance result.

Let `L` be the uncovered literal count, `N` the source size, and `M` the largest
legal verified match length. With `m` matches, literal count is at least
`max(L, N - m * M)`, while tokens and offsets cost at least `3 * m + 1`.
Ignoring extensions relaxes the price conservatively. For `M > 0`, write
`N - L = q * M + r`; the relaxed minimum is
`L + 3 * q + 1 + min(r, 3)`. Taking the maximum of that price and the existing
literal-extension lower bound is still conservative. With no legal match,
the exact literal-only price applies. Production adoption would require
admitted GPU maximum-length evidence, independent price validation, competitive
payload equivalence, cancellation and actual-corpus qualification.
