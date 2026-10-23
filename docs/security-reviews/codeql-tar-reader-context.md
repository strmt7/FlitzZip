# CodeQL TAR reader context review

The maintainer authorizes individually proven false-positive dispositions:
“Fix defects; allow exact proven false-positive dispositions.” This review
covers only the two recorded CodeQL 2.27.3 analysis categories for the existing
alert below. Raw reports remain retained; changed identities expire the review.

## Individual decision

### Alert 8422

At f441d284e31d1cbd116fab4ce42f291a35c24a89, `cpp/unused-static-function`
reports `require_tar_writer_path_boundary` at line 721 of
`tests/cpp/test_tar_compat.cpp` as unreachable. Both the CPU and HIP-host
analyses completed successfully and reproduce that exact report. GitHub's
previous dismissal does not admit its changed source or location.

The helper body and its registered
`tar_writer_preserves_ustar_and_pax_filename_boundaries` caller are unchanged
from the preceding reviewed source. The direct call is now at line 831. The
caller still iterates eight explicit USTAR/PAX and Unicode filename fixtures;
every invocation independently checks raw writer records, payload, padding,
footer, statistics and extracted bytes. `TEST_CASE` registers that caller in
the actual native registry, and `test_main.cpp` invokes its function pointer.

The new tests preceding this helper exercise TAR next-header read failures and
second-pass metadata/publication boundaries. Production now distinguishes bad
I/O and premature physical EOF from supported clean termination through one
shared header reader. Metadata types and the two existing reader entry points
are declared in the private `tar_reader.hpp`; this does not change writer-test
registration or remove the helper's direct call.

The complete required-HIP Release qualification passed all seven CTest targets,
independent archive interoperability, the format matrix and sanitizer fuzz smoke.
A separate source-bound Debug collection executed the registered filename test
successfully through the actual native runner. Its TAR-only report measures
production TAR code; it does not claim coverage hits in the test implementation.
These results establish execution of the retained caller and its assertions,
not universal correctness or whole-repository coverage.

The current report contradicts that direct call and observed execution. The
internal cause of CodeQL's reachability inference remains undetermined. Removing
the helper, inlining it, suppressing the rule or changing registration would
discard useful evidence without repairing a demonstrated defect. The committed
ledger therefore renews only this exact report's source, location, scanner,
registration, runner, reader/writer and evidence bindings. All other reports
remain governed by their own identities and acceptance requirements.
