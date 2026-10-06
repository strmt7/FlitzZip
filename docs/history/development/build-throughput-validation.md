# Build Throughput Validation

## Qualified Scope

Standard-library precompiled headers reduce repeated parsing in the MSVC
`superzip_core`, `superzip_test_objects` and `SuperZip` targets. Each target owns
its private PCH because its definitions and compiler settings can differ.
Project headers, vendor headers, C translation units and HIP device compilation
retain their existing compilation paths. Explicit source includes remain required.

The Windows 2022 CI lane builds the full app with
`tools/build.ps1 -CpuOnlyValidation -DisablePrecompiledHeaders`; the other
Windows lane and ordinary HIP-enabled builds use PCH. This retains complete
source/header independence without adding another full build. Build receipts
bind the configured PCH strategy and reject stale qualification after it changes.
Mixed-language, target-local character/macro and disabled-PCH contracts run through
the change-aware verifier and component-contracts workflow.

## Local Timing Evidence

The isolated pilot compiled the actual GUI, core, their dependencies and native
test objects from source input identity
`394c56e1c74d9e865cb8441385d4b479606ea605f5793f2af7b59bbc8c7cfaf7`.
Both strategies used CMake 4.4.3, MSVC 19.51.36260.0, C++20, Release settings
and seven memory-admitted compiler workers. HIP was disabled for this build
throughput experiment; these are not product GPU qualification or codec benchmarks.

Two sequential confirmation pairs reversed the strategy order and rebuilt from
clean outputs. The owned timing processes used normal priority. Available RAM
stayed above 29 GiB; pre-build CPU activity was 7.1–10.8%, and sampled disk
activity stayed below 12%. This is local evidence on one host, not a promise
about every machine or GitHub runner.

| Pair | Without PCH | With PCH | Elapsed time reduction |
| --- | --- | --- | --- |
| PCH first | 47.007 s | 32.062 s | 31.8% |
| PCH second | 47.118 s | 30.290 s | 35.7% |

The initial pilot also passed, but its timing lacked simultaneous host samples
and is not included in the confirmation table. A failed sampling harness was
discarded and corrected before these confirmation measurements. No runtime
compression, archive-size or hosted CodeQL speed improvement is inferred.

## Security Extraction Evidence

CodeQL CLI 2.27.1 and `codeql/cpp-all@12.1.1` traced actual MSVC builds of an
independent C++20 contract, with and without the same standard-header cohort.
Both databases contained identical fixture function locations/parameter counts
and call edges: four functions and eight calls, including an inline project-header
function, a cross-translation-unit call, `std::span` consumers and a Windows API.
This establishes that extraction mechanism; the full exact-source hosted
CodeQL analysis remains required for repository acceptance. No compiler cache,
query removal, partial database or older-source SARIF substitution is used.

The implementation follows [CMake's private PCH support](https://cmake.org/cmake/help/latest/command/target_precompile_headers.html).
GitHub's [CodeQL test-environment guidance](https://codeql.github.com/docs/codeql-overview/codeql-changelog/codeql-cli-2.22.0/)
requires real database creation for PCH extraction tests; removal of legacy
`semmle-extractor-options` flags does not prohibit traced PCH builds.

## Product Qualification

The production change passed all 21 checks selected by the change-aware
verifier, including the HIP-enabled Release build, seven native CTest contracts,
CPU/HIP corpus correctness, scanner checks, six isolated MSVC AddressSanitizer
contracts, MSI identity and portable packaging. The native source identity is
`3fa68fab623ac01449d1f6036b07d347221c460ead0a9ef9c289bd6c0bdba962`;
the successful build receipt is
`6a892364c9bac7bb339e0f56254d262fdbc42b218e6e46d3e643141320b7e597`.
These results qualify local correctness. Exact-commit hosted workflows and
the post-push audit remain separate acceptance requirements.

Ignored pilot artifacts are under `out/build-throughput-pch/`; canonical
contracts are under `tests/cmake/precompiled_headers/`. Re-measure meaningful
compiler, header-cohort or build-strategy changes before extending these claims.
