# Native Test Execution Role Review

This review applies the maintainer's authorization to retain individually proven
false positives while repairing defects. It renews only the four existing reports
below in their existing categories. Complete reported source, location, rule and
scanner identities must still match their retained raw reports and ledger rows.
The changed runner, role declarations, inventory and driver are additional review
inputs; this review does not approve a new finding or replace hosted analysis.

CTest labels now declare execution roles without changing source membership,
compiler definitions or default qualification. The restricted driver admits every
suite before selecting correctness labels and checks actual HIP readiness before
and after the native cohort. Unknown or ambiguous suites fail before execution.

The test registry now distinguishes correctness and measurement cases. Static
registrars retain direct references to the original functions. Ordinary execution
and exact component selection retain both roles. Explicit correctness-only
execution defers the five tests that invoke successful benchmark pipelines and
the separate corpus/controller script. It does not remove them from ordinary
or final qualification. Unknown registration roles fail before any case executes.
The reported TAR and GDI cases retain the default correctness role. Compiler,
allocator and platform definitions, production sources and CMake membership do
not change in this batch.

## Individual Decisions

### Alert 8348

The unchanged `malloc` remains in miniz's explicit fuzz/sanitizer allocator
branch; ordinary Windows allocation still selects the process heap. The role
metadata and restricted driver neither change allocator definitions nor install
an allocator or callback. Matching allocation, resize and release remain intact.
The [allocator review](miniz-sanitizer-context.md#alert-8348) remains applicable.

### Alert 632

The reported Lhasa stream still borrows its opaque handle. Production
`LhaReaderSession` owns the input and releases the C reader and stream in its
destructor body before member destruction releases that input; constructor
failure retains its existing cleanup. The role registry changes no callback,
handle lifetime or LHA test body. Those tests remain correctness cases and remain
registered in the ordinary and restricted cohorts. The exact
[callback lifetime review](lha-callback-lifetime.md#alert-632) still applies.

### Alert 8422

`require_tar_writer_path_boundary` still has its direct call in the registered
TAR boundary test. The updated macro still passes that test function directly
to the registry, with the default correctness role. Neither the helper nor its
caller, source membership or platform conditions changes; neither execution mode
excludes that caller. The preceding
[individual review](neutron-compaction-context.md#alert-8422) remains applicable.

### Alert 8431

The exact GDI test still initializes its unsigned induction variable at zero,
increments once per completed iteration and checks eight successive colors and
allocation counts. Its body, provider and lifetime checks are unchanged. The
updated registrar keeps the direct function reference and default correctness
role, so the restricted path retains the test. The exact
[loop review](gdi-rejection-loop.md#alert-8431) remains applicable.

These are source/caller decisions. Runner controls, native execution and fresh
scanner results remain separate qualification obligations. Raw reports and every
other finding remain subject to their existing gates.
