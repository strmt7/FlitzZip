# Private Entropy Fuzzer Build Context

This review renews only the affected existing individual records under the
maintainer's exact-source authorization. Reported source files, declarations,
locations, rules, categories and scanner versions remain unchanged. All preceding
context paths and individual evidence remain bound. No new report receives a
disposition, and no scanner coverage or public format gate changes.

## Build And Consumer Boundary

The CMake addition is confined to `FLITZZIP_BUILD_FUZZERS`. Its new standalone
target compiles the canonical portable `rans_block.cpp` and one libFuzzer entry.
Include directories, HIP-off definition and sanitizer compile/link flags belong
only to that target. Existing target definitions, source membership, compiler
options, linkage and private HIP module ABI remain unchanged. The ordinary
Windows/HIP product build does not enable this optional target.

The ClusterFuzzLite script adds the same standalone source pair using the
existing compiler flag arrays. It retains every existing compilation, dictionary,
option and target check. No shared compiler flags or allocator definitions change.
The local smoke adds the new target without removing existing consumers. Command
controls check exact source membership and fail before later links if compiling
the canonical entropy parser fails. Both the image build and local smoke run the
independent seed contracts; changes to either seed tool select the hosted fuzzer.

The fuzzer calls the existing admitted view and portable decoder. It retains full
production parser bounds and caps only fuzz materialization at 1 MiB. Independent
output/model guards remain checked after successful or rejected coded streams.
Four valid seeds restore independently specified constant bytes; five corruptions
exercise terminal states, offsets, missing contexts, probability mass and
truncation. The sanitizer seed oracle checked actual byte equality and rejection.
Both compiler languages retain sanitizer instrumentation. Host flags must come
from the pinned provider, including its C++ standard-library selection, when
validating actual compiled consumers.

This is portable parser qualification, not GPU instruction coverage, decoded CRC
qualification or archive adoption. The fuzzer adds no production encoder, fallback,
model download or archive-version assignment. The rANS writer stays inactive.

## Preserved Individual Decisions

- **8348:** The miniz Windows/fuzz allocator branches and matching resize/release
  remain unchanged. The new standalone target does not compile or call miniz.
- **632, both recorded categories:** Lhasa's borrowed input context and owning
  session destruction order remain unchanged. The new target retains no LHA
  callback or input context.
- **8422, both recorded categories:** The TAR boundary helper retains its direct
  registered caller. No native test registration is removed or changed.
- **8431, both recorded categories:** The GDI test's induction, provider, color
  comparisons and readback remain unchanged; the new target includes no GUI code.
- **703 and 8452–8459:** The CRC qualifier/reads, decoded bounds, prefix packing
  reads and sparse qualifier remain unchanged. Their component bindings and
  required-HIP consumers remain present.
- **10240–10241:** The decoder function qualifier and frequency-dependent state
  recurrence remain unchanged. Portable fuzzing neither rewrites those device
  declarations nor replaces their actual GPU consumers.
- **10246–10259:** The individually reviewed model/stream declarations, qualifiers
  and direct reads remain unchanged. The new portable target does not change
  model normalization, kernel submission or stream encoding.

Every decision remains separate in the ledger, including distinct analysis
categories for the same alert. Static caller review and the recorded sanitizer
checks do not substitute for current affected native, packaging or hosted gates.
Changed identities and all unmatched findings remain blocking.
