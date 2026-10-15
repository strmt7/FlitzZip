# Code coverage

The README Codecov badge links to measured native CPU line coverage. Coverage
helps locate untested behavior; it does not establish correctness, security,
performance, or a release qualification by itself.

## Measurement

The existing Windows 2022 CI build uses unoptimized `Debug` binaries and matching
PDB symbols, avoiding inlining and removed-code distortions in line mapping.
The Windows 2025 build keeps `Release`; HIP qualification
remains separate. After the selected correctness checks, OpenCppCoverage runs the
complete native test registry and CLI contracts, including child processes,
against the same build. CLI coverage includes help/version entry points and the
existing planning, invalid-input, binary-stdin and exact round-trip contracts,
plus the complete registry-driven archive format matrix. The Debug CI lane runs
that matrix inside collection; the Release lane retains its standalone check.
The matrix also checks option rejection without output changes, literal filenames
after `--`, and CLI SHA-256 against an independently calculated digest.
These correctness workloads retain their bounds; their timing is not performance
evidence. A failed scenario stops collection and retains its diagnostics.
There is no extra compilation or coverage runner. Collector changes select this
hosted consumer; offline collector tests do not silently rebuild the product.

The collector selects first-party `src` files linked into the native test and
CLI executables. It publishes their measured executable lines, including zero-hit
lines. GUI executable-only paths, Python/PowerShell tooling, and GPU device
instructions are not measured by this report. Do not describe this
number as whole-repository, branch, or GPU coverage.

`tools/native_coverage.py` verifies the native build receipt, both measured
executables, their symbols and scenario drivers before and after collection.
`tools/native_coverage_report.py` rejects empty, foreign,
ambiguous, malformed, or source-inconsistent reports and normalizes paths without
changing measured hits. The workflow retains raw XML, normalized XML, diagnostics,
and source/tool identities for seven days. Inspect the `native-cpu-coverage`
artifact when investigating a coverage change or upload failure.

Codecov receives only `out/coverage/native.xml`, using GitHub OIDC and a pinned
action/CLI. Upload failures fail the check. Fork pull requests retain the report
as an artifact and omit the authenticated upload. No repository upload token is
stored in source or workflow configuration.

Codecov applies its documented [report adjustments](https://docs.codecov.com/docs/fixing-reports),
including removing C++ brace-only lines. Its displayed totals can therefore
differ from the retained collector report. Compare like-for-like measurements
and report the actual Codecov result separately from raw collector totals.

`summary.json` lists every measured zero-hit line by file and the native source
files absent from the report. The latter inventory comes from the same frozen
build inputs, not a manually selected coverage list. Review both inventories:
100% of instrumented lines would still not establish whole-repository coverage.
Earlier optimized reports can lack reliable inline-code mappings and are not
directly comparable with Debug reports. Do not manufacture hits from successful
test names or collector heuristics. The collector's
[Debug guidance](https://github.com/OpenCppCoverage/OpenCppCoverage/wiki#release-versus-debug)
explains the mapping limitation.

## Meaningful tests

Choose a missing behavior or concrete failure mode before writing a test. Prefer
independent expected results, external-format interoperability, malformed input,
boundary sizes, lifetime transitions, cancellation, and corruption followed by
valid reuse. Prove a regression test fails with the defect present. Do not copy
the implementation into assertions, remove untested code from reports, or add
tests whose only purpose is increasing a percentage. A coverage increase without
new behavioral evidence is not an engineering improvement.

Container admission tests compare plain and compressed readers at structural
truncation boundaries, preserving existing destination bytes on rejection.
Publication tests use real Windows sharing denial and callback cancellation,
then require successful reuse after cleanup. Native metadata tests exercise
the public verification and extraction paths with independently framed records;
malformed GPU block metadata must be rejected before decoder execution. These
tests complement actual HIP execution and independent format interoperability.

The requested target is complete meaningful coverage. Work from the gap inventory
in coherent behavior groups, retaining malformed-input, independent-oracle and
failure-recovery assertions. Keep device instructions and unmeasured executables
visible until a suitable measurement mechanism is qualified. Resource exhaustion,
driver hangs and destruction of host state are not acceptable coverage techniques.

## Tool provenance and local use

OpenCppCoverage 0.9.9.0 is a development-only GPL-3.0 tool, downloaded from its
[official release](https://github.com/OpenCppCoverage/OpenCppCoverage/releases/tag/release-0.9.9.0).
Its installer size, SHA-256, and complete executable/DLL identities are pinned in
`tools/native-coverage-toolchain.json`. It is installed noninteractively into the
checkout's ignored tool cache and is not distributed with FlitzZip.

On Windows, provision with `py -3 -B -m tools.native_coverage --provision-only`.
`py -3 -B -m tools.native_coverage` requires an existing current CPU-only
`Debug` build; it never changes or rebuilds the ordinary HIP product
configuration. Use a separate checkout when local coverage would otherwise
replace a qualified product build.
