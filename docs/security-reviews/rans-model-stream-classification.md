# Individual GPU Model And Stream Classifications

The maintainer authorized: "Fix defects; allow exact proven false-positive
dispositions." The decisions below concern only the exact reported declarations
and their actual reads. The [committed ledger](../../.github/scanner-false-positive-reviews.csv)
binds each alert's repository, rule, CodeQL version, analysis category, location,
complete source, relevant callers, tests, configuration and this evidence.
Changed identities remain unresolved. Raw reports and instance pages are retained;
no query, source, test, qualifier or coverage path is removed.

These reports were produced by CodeQL 2.27.4's HIP-host analysis of
`8de9ccb6b4a3f8f98ffb45ec0d6df3c71d6ca443`. Their two shader sources are unchanged
at the connected host-encoder revision
`8f61a1b30717ffd2634bc7f27117efa0fd2e46a9`. The diagnostics describe unread static
variables. The reported source instead contains HIP function qualifiers or local
declarations with direct reads, identified separately below. This establishes
the narrow classification error; it does not identify the extractor's internal
cause or discredit unrelated findings.

The bound HIP-enabled native qualification connects actual GPU statistics,
normalization, models, streams and independent readback. Controls include exact
rational model expectations, full-width singleton mass, corrupted model/order and
stream ranges, guarded primitive buffers, every supported option, complete-frame
loss bounds, cancellation/recovery and the maximum decoded extent. The connected
host operation also uses the canonical GPU reader and independent bitwise decoded
CRC checks. These tests qualify their recorded inputs; they do not establish
general memory safety, representative compression gains or public format adoption.
Host CodeQL analysis is not evidence of device instruction coverage.

## Arena Unit Boundary

The host arena binder now advances from each typed array's end. Only the padded
stream slots use a byte extent. The independently checked storage plan retains
the same regions and sizes, and readback controls include consecutive source
lengths across the segment boundary. This expresses element units directly and
addresses the separate suspicious-`sizeof` report in the host binder through a
source refactor. That report receives no disposition here; its closure requires
fresh analysis of the changed source. Existing decoder/statistics decisions
retain their exact reported shader bytes and direct uses; their caller binding
also includes this reviewed arena change.

## Model Source

### Alert 10253

In `src/gpu/hip_rans_models.hip.cpp` at line 237, `target` is a function-local
constant. Line 238 immediately compares the reduced frequency sum with it before
any CDF or reciprocal is published. The target distinguishes an absent row from
the exact nonempty mass `2^precision`. Removing that read would damage model
admission. This decision covers only that declaration, not another mass check.

### Alert 10254

At line 9 of the same source, `__device__` qualifies the function
`reduce_model_totals`; it does not declare an object. Calls at lines 54, 159,
173, 188 and 236 consume its reduction in preparation, normalization and model
construction. Its barriers and full-width mass reduction remain present.
Removing the qualifier would break the device-call contract. This decision
covers only the reported function qualifier.

### Alert 10255

At line 189, `target` is the local normalized mass `1U << precision`. Line 193
reads it to check both excessive mass and the residual-rank bound. Line 200 reads
it again when distributing the deficit to eligible symbols. These are actual
reads, not an unused static declaration. The exact-mass and largest-remainder
controls independently exercise the resulting distribution.

### Alert 10256

At line 183, `frequency` is a local constrained quota. It is read into the
reduction at line 186 and into the final frequency output at line 200. Its
zero-count, one-frequency floor and apportioned cases remain distinct. Removing
those uses changes the normalized model. The decision concerns only this local
declaration and its two consumers.

### Alert 10257

At line 182, local `apportioned` is read immediately at lines 183, 184 and 185 to
select the quota, eligibility and remainder. Line 199 also reads it for the
remainder rank, and line 200 uses it to decide whether a deficit unit is assigned.
Those decisions preserve absent symbols and the one-frequency floor. The source
does not contain the unread static variable described by this report.

### Alert 10258

At line 181, local `product` is the full-width count multiplied by remaining
model mass. It is read at line 182 for floor eligibility, line 183 for the
integer quotient and line 185 for the remainder. Full-width arithmetic avoids
narrow multiplication before division. This decision does not admit a different
overflow or scaling report; it binds only this declared value and its reads.

### Alert 10259

At line 160, local `metadata` copies the completed row description. Lines 161–162
read its status, source mass and alphabet size against independent reductions.
Line 180 passes it to the floor-budget calculation; later admission again reads
its source mass. These uses are required before normalization is published.
The declaration is neither static nor unread.

## Stream Source

### Alert 10246

In `src/gpu/hip_rans_encode.hip.cpp` at line 168, local `end` is the next dense
offset. Line 170 passes it to `rans_stream_range_is_valid`, which establishes
ordering, containment and exact stream length before copying. The copy loop
then uses the admitted `result.stream_bytes`, which equals `end - start`.
Removing the endpoint's admission would weaken the destination extent contract.
Only this exact reported declaration is covered.

### Alert 10247

At line 167, local `start` is the current dense offset. It participates in the
same range-admission call at line 170 and in the destination address at line
177. The admitted adjacent offsets must describe exactly
the completed stream. This is a used local value, not an unread static object.

### Alert 10248

At line 166, local `result` copies the completed segment metadata. Line 170 passes
it to range admission; line 177 reads its source offset for the actual copy.
The helper checks success status and source-slot containment before that read.
Removing the result's use would disconnect compaction from the encoder's
completed stream. The exact malformed-result and range controls remain active.

### Alert 10249

At line 165, local `slot_end` is the independently derived source-slot endpoint.
Line 170 reads it through range admission. The helper uses that endpoint to prove
the completed stream's extent before any source copy. It is required even when
the dense offsets are valid: those offsets alone cannot authorize an arbitrary
source address. This decision binds only that endpoint declaration and use.

### Alert 10250

At line 80, `remainder` is a local output parameter for `rans_quotient` at line
82. The next-state expression at line 85 reads the resulting value. Quotient
admission and the state bounds remain unchanged. Removing the remainder changes
the rANS recurrence and invalidates independent decoding. This decision does
not admit another arithmetic or pointer diagnostic.

### Alert 10251

At line 71, local `threshold` bounds renormalization. The loop condition at line
72 and the post-loop admission at line 81 both read it. The bounded emissions,
output-slot check and state admission remain present. The value is necessary to
maintain the encoder/decoder state invariant, not an unused static declaration.

### Alert 10252

At line 9, `__device__` qualifies `rans_source_shape`, a function rather than an
object. Its predicate is called at lines 111 and 153 by encoding and compaction
admission. It bounds the source and segment shift before derived arithmetic.
Removing the qualifier or the function would damage the device interface.
This decision applies only to that reported qualifier and producer.
