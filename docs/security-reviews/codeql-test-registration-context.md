# CodeQL review of native test registration and LHA context ownership

The maintainer's standing authorization, “Fix defects; allow exact proven
false-positive dispositions,” applies individually to the active decisions below.
The original category rows matched their complete normalized source and
bound HEAD context before review. Reported source, rule, scanner version and
location remain unchanged; the LHA callback consumer changes as described below.
Old approval is identity evidence, not a
transferable safety conclusion. Canonical finding rationales remain linked
under each individual anchor.

## Changed build context and current consumers

The test registration includes test_gpu_huffman_decode.cpp,
test_gpu_neutron_writer.cpp, test_gpu_kernel_rejection.cpp and the Windows-only
test_compression_stream_io.cpp in the existing test object target. Existing
compiler flags, preprocessor definitions, target properties, library links,
reported sources, registration macro and scanner recipes are
unchanged. The source-list additions do not remove or replace these four
consumers. No concrete dependence of the reported local numeric, callback,
reachability or induction invariants on the unrelated fixture bodies was
found. Every original context path remains bound, including CMakeLists.txt;
the LHA consumer and lifetime review are also bound in their changed entirety.
Other decisions and shared evidence remain unchanged.
The direct-device rejection fixtures use the existing private kernel interface,
bounded resident buffers and independent output oracles. They add no product
exports, allocation configuration or scanner settings. Their execution remains
a separate qualification from these unchanged reported-source invariants.

The earlier required-HIP Release build passed all six exact direct consumers
listed under the individual decisions below. Its build receipt is
`b746e93ebedd694ba2e2f8dddef394563b2c016fdcbfd4e7f1f5661dfd1dd6da`;
the test executable's SHA-256 is
`98a2c566d6d412a26427d925f63915a4d442fdc96997bb8527353d3b4b42caf4`.
These historical focused results qualify their recorded source and consumers,
not the current build or the entire release. Current qualification must match
the candidate's own source, tool and output identities.

## Packaging-only context review

The current CMake edit adds only `CPACK_FLITZZIP_PACKAGE_VERSION` and
`CPACK_PROJECT_CONFIG_FILE` immediately before `include(CPack)`. The referenced
`cmake/FlitzZipMsiVersion.cmake` checks numeric version ordering only when CPack
selects WIX. It adds no native source, target, compile definition, compiler flag,
link dependency, allocator setting or scanner exclusion. Its complete bytes are
now bound with the full CMake file. The three decisions below were individually
rechecked in live source: LHA reader/stream destruction still precedes borrowed
context destruction; the registered TAR case still calls its helper for all
eight inputs; the GDI induction variable still increments once across eight
bounded iterations. The settings implementation changes do not enter those
source/caller closures. This static review does not replace current native
consumer execution or fresh hosted analysis.

## Individual decisions

### Alert 8386

This previous source decision is retired. The settings implementation now uses
fraction decomposition, and neither scanner category has an active admission
for the old report. The [numeric review](settings-numeric-validation.md#alert-8386)
retains its history. Current analysis and scalar-boundary tests must qualify
the changed implementation independently.

### Alert 632

[LHA borrowed callback context](lha-callback-lifetime.md#alert-632) remains valid for CodeQL 2.27.3
`cpp/stack-address-escape` at `third_party/lhasa/lib/lha_input_stream.c`,
85:2–85:25.

lha_input_stream_new borrows its opaque context until stream release.
Production LhaReaderSession frees reader and stream before its heap-owned
LhaInputContext and the ifstream inside it. The outer pinned source outlives
both synchronous sessions; the context also holds the sealed byte extent,
delivered count and sticky error state. The callback never throws through C;
native refusal before the sealed end is an error even when the CRT sets EOF.
Failure unwinding preserves that order. Test inputs outlive their stream
owners. The direct callback regression resets the stream while its stack
context is live and observes exactly one original close callback. Callback-
table copying is a separate repaired contract and does not change context
ownership.

Current exact Release consumer(s):

- `=lha_stream_owns_callback_configuration`
- `=lha_extraction_rejects_native_read_failure_before_publication`

### Alert 8422

[TAR registered helper reachability](codeql-tar-reader-context.md#alert-8422) remains valid for CodeQL 2.27.3
`cpp/unused-static-function` at `tests/cpp/test_tar_compat.cpp`,
789:6–789:38.

The registered tar_writer_preserves_ustar_and_pax_filename_boundaries case at
line887 directly calls require_tar_writer_path_boundary at line899 for eight
fixtures. The unchanged TEST_CASE registrar and runner execute it. Independent
USTAR/PAX header, padding, footer, payload, statistics and extracted-byte
checks remain intact. CodeQL's internal reachability-inference cause remains
unknown; no relocation or suppression is proposed.

Current exact Release consumer(s):

- `=tar_writer_preserves_ustar_and_pax_filename_boundaries`

### Alert 8431

[GDI finite retry-loop induction](gdi-rejection-loop.md#alert-8431) remains valid for CodeQL 2.27.3
`cpp/constant-comparison` at `tests/cpp/test_gdi_back_buffer.cpp`,
177:37–177:49.

The local uint64 attempt starts at zero, increments once per completed
iteration, is never reassigned and is not captured by paint callbacks.
Assertion failure exits the test. Eight distinct colors and progressive
allocation counts require eight iterations, followed by a ninth valid
allocation/readback and explicit release. Added unrelated translation units do
not alter this local induction variable. CodeQL's inferred constant range
remains unexplained.

Current exact Release consumer(s):

- `=gdi_back_buffer_releases_cached_surface_after_native_destination_failure`

## Acceptance limits

Both existing categories remain explicit: `/language:c-cpp` and
`/language:c-cpp/configuration:hip-host`. This is an individual false-positive
context review, not a production repair or fresh hosted-analysis pass. Raw
reports, scanner coverage and every unmatched finding remain intact and
blocking. Changed source, callers, location, scanner identity or bound context
expire this decision; canonical current-analysis matching and final exact-commit
hosted audit remain required.
