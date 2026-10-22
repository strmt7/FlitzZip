# Current CodeQL Diagnostic Validation

The maintainer authorizes individually proven false-positive dispositions:
“Fix defects; allow exact proven false-positive dispositions.” This review
applies to the exact CodeQL 2.27.3 reports bound in the committed ledger. It
does not accept other reports or transfer approval to another scanner version.

Both complete CPU and HIP-host analyses completed successfully. Their original
SARIF reports each contain the individual diagnostics reviewed below. The raw
reports, exact alert responses and source comparisons remain in local review
artifacts. All seven Release CTest suites and the source-bound Debug registry
passed; the latter executed the registered native tests with HIP enabled.
These results qualify the exercised consumers, not universal correctness.

For every retained decision, the complete finding source and relevant context
were rechecked. The previous context still matches except for the numeric
settings decision: its caller file gained log-field escaping and an error check
for an invalid settings parent, and the workflow gained verified Semgrep
dependency submission. Neither change alters numeric conversion or CodeQL's
query/build configuration. The revised ledger binds those current bytes.

## Individual Decisions

### Alert 632

The [LHA callback-lifetime review](lha-callback-lifetime.md) still applies.
`lha_input_stream_new` copies the callback table and borrows only the documented
opaque context. Compiled callers destroy the stream before that context.
Synchronous read, skip and close callbacks do not escape the owned session.
The callback-table snapshot repair and fragmented/error/termination regressions
remain intact. This report does not identify another lifetime defect.

### Alert 838

The [Gzip borrow review](current-source-findings.md#alert-838) still applies.
`InputBorrow` clears `next_in` and `avail_in` during ordinary return and exception
unwinding. Bounded input is consumed synchronously before the caller storage
expires; no worker retains it. Stream and exceptional-sink regressions passed.

### Alert 839

The [Bzip2 borrow review](current-source-findings.md#alert-839) still applies.
The synchronous compressor drains each input chunk and its local owner clears
the borrowed pointer/count on every exit. The C ABI's non-const input pointer
does not change its read-only use or extend its lifetime. Stream and failure
regressions passed.

### Alert 853

The [staged LZ4 interface review](current-source-findings.md#alert-853) still
applies. Public, optional static-linking and implementation phases have separate
guards. A whole-file guard would prevent the supported public-first, static-later
inclusion sequence. Existing C and C++ inclusion consumers retain that contract.

### Alert 854

The [fast literal-length review](current-source-findings.md#alert-854) still
applies. Both operands are `size_t`; `read_variable_length` returns the typed
`SIZE_MAX` error sentinel on invalid input. The comparison rejects that reachable
error before using the length. It is not an unsigned comparison with a negative
signed range. Malformed-length and output-preservation controls remain active.

### Alert 855

The [fast match-length review](current-source-findings.md#alert-855) still
applies. The same typed sentinel must be rejected before extending a match.
Removing this check would admit a decoder error, not repair dead code.

### Alert 856

The [safe literal-length review](current-source-findings.md#alert-856) still
applies. Truncated extension bytes can return the typed unsigned sentinel; the
check precedes length arithmetic and copying. Independent truncation/canary
controls retain this boundary.

### Alert 857

The [safe match-length review](current-source-findings.md#alert-857) still
applies. The unsigned sentinel comparison is reachable and protects subsequent
decoded-length use. The reported signed-range inference contradicts the actual
types and error return.

### Alert 8347

The [PPMd input-lifetime review](current-source-findings.md#alert-8347) still
applies. `SzDecodePpmd` owns the decoder and callback bridge together. All range
decoding ends before either allocation is freed and the function returns.
Allocation refusal, callback lifetime, I/O failure, truncation and empty-output
consumers remain covered. The separately repaired empty-output arithmetic defect
is not admitted by this decision.

### Alert 8386

The [numeric validation review](settings-numeric-validation.md) still applies.
`SettingsJson::integer` requires a finite, exactly integral `double` in the
Windows `int` range before casting. Exact comparison with `std::trunc` is the
validation operation; an epsilon would incorrectly accept fractions. The
deserializer's bounded integer calls are unchanged. Endpoint, adjacent-fraction,
negative-zero and type/error regressions passed, as did real GUI preference
migration and persistence checks.

### Alert 8422

`cpp/unused-static-function` labels `require_tar_writer_path_boundary` unreachable
in `test_tar_compat.cpp`. The function is called directly by
`tar_writer_preserves_ustar_and_pax_filename_boundaries`, which is registered
through the actual `TEST_CASE` registrar and executed by the native runner.
The test iterates eight explicit filename fixtures, including USTAR boundaries,
PAX overflow and Unicode. Each helper invocation checks independent raw archive
records, payload, padding, footer, statistics and extracted bytes.

The current Debug executable ran that exact registered test successfully. The
complete source-bound registry also passed. These observations contradict the
reported reachability result; the precise internal cause of CodeQL's inference
has not been established. Removing the helper or its registration would remove
useful coverage. Neither code relocation, inlining, rule suppression nor a
production change is warranted by this diagnostic. This decision covers only
the exact helper, call site, registration, runner, writer and scanner identities
bound in the ledger. Original CPU and HIP-host results remain retained.
