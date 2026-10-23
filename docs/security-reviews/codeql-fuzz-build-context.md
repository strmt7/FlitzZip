# CodeQL fuzz-build context review

The maintainer authorizes individually proven false-positive dispositions:
“Fix defects; allow exact proven false-positive dispositions.” The decisions
below preserve the exact CodeQL 2.27.3 rule, source, location and analysis
categories recorded in the committed ledger. Raw reports remain retained.
Fresh hosted analysis and the final exact-commit audit remain required.

## Build context

Every previously bound source, caller, test and scanner input still matches its
reviewed bytes except `CMakeLists.txt`. Its changes add only the AR target and
private instrumentation options inside `if(FLITZZIP_BUILD_FUZZERS)`. That option
remains off by default. No production target, existing test registration,
numeric conversion, Lhasa lifetime, query suite or CodeQL build changes.

The complete required-HIP Release CTest suites passed, including the specific
numeric, callback and TAR boundary regressions described below. The real Linux
sanitizer smoke also passed with the new AR target and existing targets.
These observations qualify the exercised consumers; they do not establish
universal correctness or whole-repository coverage.

## Individual decisions

### Alert 632

The [callback-lifetime review](lha-callback-lifetime.md) still applies to the
unchanged source and callers. `LhaReaderSession` frees the reader and stream
before destroying its input owner. Test readers destroy their stream before
the borrowed memory source. The active callback-ownership regression passed.
Adding the separate AR fuzz target does not change those lifetimes.

### Alert 8386

The [numeric validation review](settings-numeric-validation.md) still applies.
Finiteness, exact integrality and `int` range checks precede narrowing. The
unchanged typed-scalar and representable-whole-number regressions passed.
The new fuzz-only target cannot change this validation contract.

### Alert 8422

The [TAR reachability review](codeql-current-validation.md#alert-8422) still
applies. The helper retains its direct call inside the registered filename
boundary test, and that test passed in the current native suite. Its CMake
registration is unchanged. The fuzz-only additions do not make the helper
unreachable or justify removing useful independent archive checks.
