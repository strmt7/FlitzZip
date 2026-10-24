# CodeQL review after two test-registration additions

The maintainer's standing authorization, “Fix defects; allow exact proven
false-positive dispositions,” applies individually to the four decisions below.
All eight original category rows matched their complete normalized source and
bound HEAD context before review. Source, rule, scanner version, location and
existing callers remain unchanged; old approval is identity evidence, not a
transferable safety conclusion. Canonical finding rationales remain linked
under each individual anchor.

## Changed build context and current consumers

The sole changed original bound context is CMakeLists.txt: it adds
test_gpu_huffman_decode.cpp and test_gpu_neutron_writer.cpp to the existing
flitzzip_tests source list, adding their object files to that target. Existing
compiler flags, preprocessor definitions, target properties, library links,
reported sources, direct callers, registration macro and scanner recipes are
unchanged. The source-list additions do not remove or replace these four
consumers. No concrete dependence of the reported local numeric, callback,
reachability or induction invariants on the unrelated GPU fixture bodies was
found. Every original context path remains bound, including CMakeLists.txt;
only this context document is added. Other decisions and shared evidence are
unchanged by this renewal.

The current required-HIP Release build passed all five exact direct consumers
listed under the individual decisions below.
Build receipt: `21634c97bbf26099caa0e0084f52517111db6f3dee3d8533f62e3c6ba3be586a`; executable-input digest:
`5ed12d98519470e9d8a90802f7e2dac54d79dc83cf5ebd3fbf11b04bb17e47c6`. Root retained complete bounded process results and
verified the same build identity before and after all five selected consumers.
The retained qualification digest is `9baa4e8cac8d8ade24da96cc35366c9dd80ad4066ae3adc30f598429407d0b50`. The earlier Debug
attempt refused a stale configuration and ran no tests; no Debug result is
claimed for this review.

## Individual decisions

### Alert 8386

[Settings numeric validation](settings-numeric-validation.md#alert-8386) remains valid for CodeQL 2.27.3
`cpp/equality-on-floats` at `src/app/settings_json.cpp`,
163:55–163:83.

SettingsJson::integer checks type, finiteness, exact integrality and int range
before narrowing. Exact truncation comparison intentionally rejects fractional
scalar values; epsilon comparison would weaken admission. The typed-scalar and
whole-number regressions cover near-integer fractions, out-of-range values,
endpoints, exponent notation, negative zero and clamping.

Current exact Release consumer(s):

- `=settings_json_requires_exact_scalar_types`
- `=settings_json_clamps_representable_whole_numbers`

### Alert 632

[LHA borrowed callback context](lha-callback-lifetime.md#alert-632) remains valid for CodeQL 2.27.3
`cpp/stack-address-escape` at `third_party/lhasa/lib/lha_input_stream.c`,
85:2–85:25.

lha_input_stream_new borrows its opaque context until stream release.
Production LhaReaderSession frees reader and stream before its owned ifstream;
failure unwinding preserves that order. Test inputs outlive their stream
owners. The direct callback regression resets the stream while its stack
context is live and observes exactly one original close callback. Callback-
table copying is a separate repaired contract and does not change context
ownership.

Current exact Release consumer(s):

- `=lha_stream_owns_callback_configuration`

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
