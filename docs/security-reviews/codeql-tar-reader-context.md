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
source identity and the helper's location. The previous ledger entries are
inactive and retained in Git history. Fresh CPU and HIP-host reports must
establish the current exact location before an updated individual decision is
admitted. The direct call and runtime observations are evidence for that review;
they are not a substitute for its source and scanner bindings.

The internal cause of CodeQL's reachability inference has not been established.
Removing or relocating the helper, changing registration or suppressing the rule
would discard useful checks without repairing a demonstrated production defect.
Raw reports and every unmatched finding remain blocking.
