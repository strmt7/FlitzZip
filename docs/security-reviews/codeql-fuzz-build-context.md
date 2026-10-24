# CodeQL fuzz-build context review

The maintainer authorizes individually proven false-positive dispositions:
“Fix defects; allow exact proven false-positive dispositions.” The decisions
below preserve the exact CodeQL 2.27.3 rule, source, location and analysis
categories recorded in the committed ledger. Raw reports remain retained.
Fresh hosted analysis and the final exact-commit audit remain required.

## Build context

The callback and numeric contracts below are enforced in their source, not by
assuming that CMake remains unchanged. The ordinary archive targets retain their
owned synchronous callback boundaries. Fuzzing and development-only device
profiling have separate opt-in builds; instrumentation cannot be installed as
the product. None of those build roles extends a borrowed archive context's
lifetime or changes the integer-validation predicate.

The complete required-HIP Release CTest suites exercised the numeric, callback
and TAR boundary regressions. Final qualification must still cover the accepted
source, build roles and hosted analyses. These observations do not establish
universal correctness or whole-repository coverage.

## Individual decisions

### Alert 632

The [callback-lifetime review](lha-callback-lifetime.md) still applies to the
unchanged source and callers. `LhaReaderSession` frees the reader and stream
before destroying its input owner. Test readers destroy their stream before
the borrowed memory source. The active callback-ownership regression passed.
The current reader's validation and publication passes preserve those lifetimes.

### Alert 8386

The [numeric validation review](settings-numeric-validation.md) still applies.
Finiteness, exact integrality and `int` range checks precede narrowing. The
unchanged typed-scalar and representable-whole-number regressions passed.
The predicate remains independent of the selected archive or build role.
