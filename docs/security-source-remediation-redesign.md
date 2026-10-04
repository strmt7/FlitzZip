# Source Remediation Redesign

The maintainer requested this redesign on 4 October 2026 after challenging the
admission of 81 source reports. The earlier confirmations approved 21 source,
six SDK and 54 hosted dispositions. They did not fix those operations. Matching
hashes establish identity, not memory safety. The old reviews remain historical
evidence; their verdicts no longer allow an outstanding report to pass.

## Production Boundaries

The first implementation replaces the dictionary selector's independent suffix
pointer and size with `COVER_dictContent_t`: allocation base, allocation capacity
and initialized offset travel together. Selection rejects a missing base,
offset beyond capacity and extents beyond `PTRDIFF_MAX` before pointer formation
or allocation. Finalized output capacity remains independent from readable
input. Shrinking candidates must fit the initialized suffix before their source
is formed. Both COVER and FASTCOVER use the representation. The transformation
requires known complete source identities and preserves the upstream archive.

The checked selector now runs in a C++20 translation unit with `std::span` input
regions and move-only array owners. Error returns automatically release both
temporary dictionaries. Success transfers exactly one array to the private C
record; its release uses the matching array deallocator. COVER's separate best
dictionary continues to own its own copy. The canonical test selector retains
serial allocator/finalizer interposition; the product DLL uses array allocation
and deallocation. Both builds execute the same selection and geometry logic.

The selector's native regression supplies malformed geometry and requires an
ordinary parameter error, no finalizer call and no retained allocation. Existing
shrinking, allocation/finalizer faults, guarded output and independent readback
controls remain. This is stronger geometry enforcement; scanner closure remains
unverified until fresh analysis of these generated production sources.

The standalone sanitizer control uses C++ array ownership and stream output.
Its valid final-byte access and deliberate one-past-allocation access remain.
Qualification must still observe ASan termination and a heap-overflow diagnostic;
the rewrite cannot remove or catch the expected fault.

Dictionary evaluation now shares the selector's C++ ownership boundary. Packed
sample offsets must match a checked accumulation of their sizes before any
sample subview is formed. Train/finalization counts, null nonempty inputs,
nonfinite split points and unrepresentable extents are rejected before allocation
or finalizer access. Finalized dictionaries must fit their actual allocation.
Compression output and aggregate size are independently bounded, and compression
contexts, prepared dictionaries and output arrays have matching scoped cleanup.
Both dictionary builders reject sample-size accumulation overflow through the
existing input-size limit. Tests retain allocation failures and independently
decompress every evaluated frame; declared C allocations still require live
caller storage and are not proved by metadata alone.

The packed Huffman fill now validates all symbol ranks and table capacity before
publishing typed entries. It no longer writes wide integer values through
smaller-entry pointers. FSE table construction and state/sequence consumers
share an explicit header and checked region description. Invalid descriptors
are rejected before construction shifts or region pointers; the packed ABI is
unchanged. Direct controls check ranked entries, canaries, malformed metadata,
RLE geometry, boundary symbols and all supported descriptor sizes.

The serial fault fixture now retains exclusive array owners instead of a raw
address registry with manual allocation/release. Null, foreign and repeated
release attempts retain every other owner. Its controls fill the entire record
budget and verify exhaustion, failed resets with live storage and exact cleanup.
Production and interposed legacy owners have separate C symbols, preserving
observable failures while removing ambiguous duplicate definitions from linked
analysis. Architecture-specific range checks remain active where their operand
ranges require them; wider targets retain the owner's capacity admission.

The buffered legacy decoders now retain one opaque C++ ownership tree for input
and output storage. Their C state has no independent allocation capacities:
stages borrow the actual buffer geometry from that owner. Default arrays use
matching array allocation and deallocation; custom callbacks and their opaque
state remain paired through construction, growth and destruction. Both needed
replacements are acquired before either old allocation is released, so a failed
growth preserves both identities, capacities and initialized bytes.

Header, input and output transfers validate complete readable and writable
extents, offsets and counts before pointer formation. The shared C++ transfer
uses bounded views and preserves both overlap directions. Decoder stages map
failure to their own version's ordinary error codes; public entry points reject
missing counters, null nonempty extents and unrepresentable pointer geometry
before forming cursors. Header consumption, output wrap and backpressure retain
the version-specific stream behavior.

Direct controls compare overlapping transfers against an immutable-input
oracle, require unchanged canaries on invalid requests, and inject every owner,
initial buffer and growth allocation failure. Existing pinned legacy frame,
history, public backpressure and patch-migration controls remain. These controls
must pass in both the HIP product qualification and the canonical ASan build;
this document does not establish their result or hosted alert closure.

Reusable agent policy contains procedures and contracts. Incident inventories,
dates, individual dispositions and qualification results remain in audit
records. The security skill no longer carries the historical disposition counts
or depends on this particular redesign being the active task.

The COVER and FASTCOVER optimizers now share an explicit private C++ work-group
owner. It owns the synchronized best result, optional worker pool and private
C context storage. Successful initialization commits the matching C cleanup
callback; an initializer that already rolled back cannot trigger a second
cleanup. Context replacement acquires storage before joining and releasing its
predecessor, so rejected geometry and allocation failure retain prior ownership.

Logical completion and actual worker return are distinct. The group waits for
`POOL_joinJobs` before releasing context, then waits for the best-result record
before destroying its synchronization. A regression deliberately signals
logical completion before the worker's final context access and requires both
real workers to return before destruction. Serial allocation-failure,
uncommitted-initialization and bounded dictionary-publication controls exercise
the same owner. Production guarded training exercises both exported optimizers
with real pools. These source and consumer contracts do not establish hosted
alert closure until fresh analysis completes.

## Remaining Source Work

The earlier inventory contains 28 DevSkim and 53 CodeQL source reports. Its
reported source locations, including build-derived dependencies and the CMake
compiler probe, are retained in the historical ledgers. These groups require
different production contracts:

| Report group | Required engineering boundary | Required verification |
| --- | --- | --- |
| Dictionary/hash and block-output pointers | Complete allocation geometry, initialized input and independently bounded output | Direct selection/finalizer and guarded production compression tests, fresh flow analysis |
| Packed FSE/Huffman table offsets | Typed table regions, element counts, alignment and descriptor capacity | Representation and malformed-table controls, decoder readback and sanitizer tests |
| Optimizer task pointers | An explicit owner covering every submitted worker until join, including failure cleanup | Serial and worker-pool consumers, allocation-failure and lifetime tests |
| Legacy dictionary/output/literal references | Lifetime and initialized extents enforced at actual decoder use | All supported legacy versions, independent sequence oracle and history/backpressure tests |
| Allocation/copy API reports | Matching allocation/deallocation ownership and both readable/writable extents, with ordinary error propagation | Fault injection, truncation, overlap, aliasing/alignment and canaries |
| Algorithm progress | Explicit progress and comparison-budget state without changing the algorithm | Direct match-search and suffix-ranking consumers |
| Staged headers/default allocator | Public/static/inline inclusion and allocator contracts preserved | C and C++ first/repeated inclusion and real allocator consumers |
| Upstream quality/probe observations | Actual upstream interface or research work, with truthful provenance | Relevant compiler and direct-consumer contracts |

None of these rows is marked complete by this document. Replacing a primitive
name, moving a file out of coverage, discarding an old-source reproducer or
adding whole-file guards to staged headers cannot close a row. New runtime
dependencies still require the ordinary maintainer approval.

## Acceptance And Recurrence

`scanner_preflight.py` scans frozen changed publication bytes with pinned
detectors before expensive qualification. `review_findings` keeps exact source
review matches informational and counts them as unresolved. Public checksum
metadata retains its separate exact identity contract; source code cannot use
that metadata path. Raw reports are never filtered.

`github_post_push_audit.ps1` requires zero open alerts from its complete hosted
inventory. It has no source or Scorecard admission path. Resolved and dismissed
history remains available, but a dismissal is not evidence of source repair.
Branch controls and external attestations require actual governance evidence;
source rewrites cannot truthfully satisfy them.

For each related batch, run the change-aware planner once, complete the affected
production/consumer contracts, and preserve failures before widening coverage.
Use completed receipts only for unchanged inputs and their actual recorded
scope. Native changes require new HIP qualification; no permanent build or
scanner disablement is authorized. Analyse the coherent source batch, preserve
stable SARIF categories and paths, and require fresh source closure before
reporting a finding fixed. Intermediate pushes must report pending or failed
gates as such. Final acceptance requires the accumulated change range.

No static analyser or repository instruction guarantees that future agents will
introduce no defects. Enforced contracts, retained negative controls, dependency
provenance and automatic changed-input checks are the recurrence controls.

## Research Basis

[CodeQL's pointer guidance](https://codeql.github.com/codeql-query-help/cpp/cpp-invalid-pointer-deref/)
supports validating bounds before pointer access; its reports require source
inspection rather than severity-based assumptions. Its
[stack lifetime guidance](https://codeql.github.com/codeql-query-help/cpp/cpp-stack-address-escape/)
identifies retaining stack addresses as an ownership concern.
[Microsoft's checked-copy contract](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/memcpy-s-wmemcpy-s?view=msvc-170)
does not supply the actual source extent and changes error behavior, so a blind
`memcpy_s` substitution cannot establish a complete repair.
[GitHub source-resolution guidance](https://docs.github.com/en/code-security/how-tos/manage-security-alerts/manage-code-scanning-alerts/resolve-alerts)
distinguishes source resolution from dismissal, while its
[SARIF guidance](https://docs.github.com/en/code-security/reference/code-scanning/sarif-files/sarif-support)
requires stable result identity to avoid unnecessary duplicate alert creation.

The [C++ algorithm contract](https://eel.is/c++draft/alg.copy) excludes a copy
destination starting inside the source range. The checked transfer handles
identical ranges as a no-op and uses backward copying for rightward overlap;
leftward overlap uses forward copying. The immutable-input oracle covers each
case. [C++ ownership guidance](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#r1-manage-resources-automatically-using-resource-handles-and-raii)
supports scoped resource handles, while [NIST SSDF](https://csrc.nist.gov/pubs/sp/800/218/final)
supports repairing root causes and integrating recurrence prevention into
development. None of these references establishes repository certification.
