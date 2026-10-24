# TAR Writer Reachability Review

## Individual Decision

### Alert 8422

CodeQL previously reported `require_tar_writer_path_boundary` as an unused
static function. Its direct caller,
`tar_writer_preserves_ustar_and_pax_filename_boundaries`, is registered through
`TEST_CASE` and executed by the native runner. Eight USTAR/PAX and Unicode
fixtures check independent writer records, payload, padding, footer, statistics
and extracted bytes. The complete required-HIP Release registry executed this
caller successfully.

The diagnostic-oracle changes in the same translation unit changed its complete
source identity and the helper's location. Fresh CodeQL 2.27.3 CPU and HIP-host
analyses of commit `78049ac9d90e8b219894a64595fba5834ae37953` both identify
line 789, columns 6 through 38. The registered test at line 887 directly calls
the helper at line 899 for all eight fixtures. The current complete required-HIP
Debug registry also executed the caller successfully during host coverage
collection. The source, test registration, direct call and scanner configuration
were individually re-reviewed before renewing the exact current ledger entries.
Previous source identities remain historical evidence, not current admission.

The internal cause of CodeQL's reachability inference has not been established.
Removing or relocating the helper, changing registration or suppressing the rule
would discard useful checks without repairing a demonstrated production defect.
Raw reports and every unmatched finding remain blocking.
