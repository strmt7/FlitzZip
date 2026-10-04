# Security Finding Review — 3 October 2026

## Unpublished Dictionary And Quality Repairs

The current local batch repairs a reproduced dictionary-shrinking defect:
the previous selector could request an 808-byte suffix from only 680 initialized
bytes because it reused a finalized dictionary size as a source-content bound.
The rewritten selector keeps initialized content and finalized output extents
separate, bounds every candidate before forming its suffix, guards candidate
growth, and retains the full-dictionary fallback. Failed allocations now return
a genuine error code and release both selection buffers.

Three regression groups exercise the exact generated selection function and
the production finalizer. The prior selector fails all three groups; the new
selector passes the reproducer, 36 content/seed boundary cases, real dictionary
compression/read-back, allocation failures and both finalizer failures. The
tests observe source extents before finalization and check complete ownership
cleanup. The upstream archive remains unchanged.

Separate quality repairs replace 92 disabled-code or pseudocode comments with
algorithm explanations and compile two assertion-only helpers only where
assertions can use them. Complete debug/release encoder compilation checks
preserve the helpers in assertion-enabled builds. A fresh manually traced
CPU-only CodeQL database for frozen tree
`e82fac187b75739b3e60bd79720a604953ec17cb` ran the hosted suite combination,
183 unique queries, with no analysis errors. All 92 targeted comment reports
and both unused-helper reports are absent. This is local evidence for 94
quality repairs, not GitHub closure or a vulnerability count.

That analysis retains 96 raw reports, including 78 in Zstandard and one in
the CMake compiler-probe header. The other 17 concern separately retained
dependencies and must be reconciled with historical GitHub dispositions.
CodeQL reports extraction of 354 of 385 C/C++ files; this CPU-only invocation
does not qualify HIP device analysis. All six critical pointer reports remain.

After that frozen analysis, seven large dependency functions received explicit
contracts and explanations of their algorithm stages. Exact source hashes and
compiled-token comparisons bind the comment-only transformations. Fresh,
previous-constructor, previous-selector and previous-comment migration,
idempotence, source drift, interrupted writes and provenance checks pass.
The newer comments are not covered by that earlier CodeQL database.

The literal decoder has subsequently been separated into header parsing,
Huffman dispatch, raw/RLE decoding and a small routing switch. The original
buffer-placement, table-reuse and error paths remain explicit. Before the
rewrite, public-DLL tests established 39 raw/RLE header cases and actual
compressed/repeated Huffman blocks. The same five bounds-test groups, including
small-chunk streaming, pass against both the retained original DLL and the
rewritten DLL. Fresh patch/drift/migration and header contracts also pass.
The rewritten native receipt is
`2cbc3c4e0f4bcac38ec911e573db5fb4f46801af9ab2f284a3c46a8b5eeb983d`.
This structural repair targets alert 1534.
Performance timing is deferred after a host sample reported 21% CPU use.

A second independently traced frozen tree,
`f5f9854762eb3bfee29e8d20703a803c28f4f74c`, ran all 183 queries without
analysis errors. It retains 88 raw results. The seven targeted documentation
reports and alert 1534's literal-decoder switch report are absent. CodeQL
reports extraction of 354 of 386 C/C++ files; hosted closure and HIP-device
qualification remain separate requirements.

The v0.5–v0.7 literal decoders were then split by literal kind. All 12 moved
algorithm bodies retain their complete ordered compiled-token sequence.
Exact upstream golden frames for versions five through eight, 117 synthetic
legacy raw/RLE header cases, chunked streaming and malformed-size rejection
pass against both the retained original DLL and the rewritten DLL. Version
four remains a rejection control. These later changes target alerts 1536,
1537 and 1539. Their reports are absent from the third local analysis below;
hosted closure remains unverified.

On 4 October, inspection of the remaining legacy stream switches exposed a
separate allocation-failure defect. Each buffered decoder published a new
capacity before its allocation succeeded, leaving a null buffer paired with
nonzero capacity after failure. Six independent injected-failure tests reproduce
that invalid ownership state in the preceding production sources without an
unsafe retry. The rewritten buffer acquisition obtains every required
replacement before releasing or publishing either owner. All six regressions
pass, together with five growth-failure cases that preserve both existing
owners and capacities, successful reinitialization/read-back, and complete
cleanup. No allocator API or warning was renamed to change scanner results.
The read, load and flush stages are now separate small functions, retaining
version-specific hint and consumption semantics. The stream changes also
target alerts 1535 and 1538. Their reports are absent from the third local
analysis below; hosted closure remains unverified.
The latest focused HIP native receipt is
`f4daf1670a0f239afbb7f1a155711987a18a7f634fffccbe73721171aa6b2478`.

Before the literal rewrite, the HIP Release build and complete native driver
passed: seven CTest
targets, CPU and required-HIP corpus read-back at seven block sizes, rejection
cases and controller correctness checks. Timings are unqualified. Their native
receipt is
`72d94b5bfc7963e4e5b6859cea16b40a6b94e1d592966eed61710ce33ceda6b8`.
The declaration/contract audit, changed-file hygiene, language lint and policy
security scan also passed for that earlier batch. The complete driver and
selected gates must be refreshed after the literal rewrite.

Publication remains blocked. Local DevSkim reports direct allocations and
copies in the tracked dependency fragments; its preflight has not passed.
The refreshed 25-file preflight retains 15 DevSkim results: six allocations
and nine copies. Gitleaks reports no findings. The raw results remain
unresolved and the selected verifier stops at this gate. The seven existing
DevSkim findings and 180 unapproved hosted findings are not disposed of by this
batch. No scanner exclusion, rule suppression, dismissal, push or release has
occurred. Packaging, remaining selected gates, fresh hosted analyses and final
post-push acceptance are still required before beta review.

Private evidence is retained in
`out/codeql-repair-20261003/source-manifest.json`,
`out/codeql-repair-20261003/summary.json`,
`out/codeql-repair-20261003/results.sarif`,
`out/cover-selection-baseline-20261003.json`,
`out/cover-contracts-product-migration-20261003.json` and
`out/cover-contracts-native-full-20261003.json`, plus
`out/literal-refactor-native-20261003.json`,
`out/literal-stream-native-20261003.json` and
`out/literal-stream-baseline-dll-20261003.json`.
Newer private evidence is retained in
`out/codeql-repair-literals-20261003/summary.json`,
`out/legacy-literal-token-equivalence-20261003.json`,
`out/zstd-legacy-fixture-provenance-20261003.json`,
`out/legacy-literal-header-original-dll-20261003.json`,
`out/legacy-stream-allocation-all-sites-baseline-20261004.json` and
`out/legacy-stream-growth-native-20261004.json`.

The complete native driver has subsequently passed for receipt
`3d8be061ec33e166e179a2b41d4c79cbd8d25a115cf68284a5214e60ba3c2fa6`:
seven CTest targets, exact CPU/required-HIP corpus read-back across seven block
sizes, rejection controls and controller correctness checks. Updated language
lint passes after correcting one overlong CMake path expression. The selected
publication verifier remains failed; these independent correctness checks do
not override its 15 unresolved DevSkim findings. Private records include
`out/legacy-stream-full-native-20261004.json`,
`out/legacy-stream-lint-fixed-20261004.json` and
`out/legacy-stream-plan-20261004.json`.

A third freshly traced frozen tree,
`1558f04e9659192796dff9412d5c6749ea2749e9`, ran the same 183 queries without
analysis errors. It retains 88 raw reports: the five targeted legacy-switch
reports are absent, but five new quality reports appeared in the test setup
(three source-file includes and two buffer helpers reported as unreachable).
CodeQL reports extraction of 358 of 396 C/C++ files. The test setup now compiles
the canonical production translation units directly, with private conditional
read-only buffer inspection, instead of including implementation files. The
two helper reports require reachability diagnosis and a refreshed analysis;
this batch is not declared scanner-clean.

The private allocation header's existing guard now covers its dependencies as
well as its function definitions. Repeated C11/C++20 inclusion and exact
previous-revision migration pass, while the separate staged-public-header
contracts remain intact. The bufferless modern decoder now separates header,
block and checksum operations from its stage route. All six moved operation
bodies preserve their ordered compiled tokens. Direct bufferless tests passed
before the rewrite and after it, including raw/RLE and compressed/repeated
Huffman blocks, checksum corruption, skippable frames, truncated input and
output bounds. These changes target alerts 1636 and 1533; fresh scanner
closure has not yet been measured.

All five focused dependency targets pass with the revised test architecture,
guard, bufferless decoder and migration from the previous stream revision.
Their HIP-enabled native receipt is
`033ede3e5b42dd2b473bdfe2b32a43867028a531657cc5ce0b738612d9bb8dda`.
The latest full-native receipt above precedes these changes. Exact private
evidence includes `out/codeql-repair-legacy-20261004/summary.json`,
`out/bufferless-baseline-and-test-hooks-20261004.json`,
`out/bufferless-decoder-token-equivalence-20261004.json` and
`out/bufferless-refactor-native-20261004.json`. None of these local results
changes the hosted inventory or satisfies publication acceptance.

A fourth freshly traced frozen tree,
`bd92b51feb62c84c974608f4635aded1cafe49fd`, ran all 183 queries without
analysis errors and retains 83 raw reports. Alerts 1533 and 1636's targeted
reports and all three implementation-file inclusion reports are absent.
The two reservation-helper reports persist. A diagnostic using the same
reachability predicates shows both a reachable and an unreachable recorded
version of each v0.5/v0.6 helper, with one caller for the reachable version.
The instrumented allocator changes these definitions while their callers
share a source path. Fault-test sources now have separate build paths and
must hash identically to the complete generated production translation units.
Their decoder algorithms, allocator failure controls and raw scanner inputs
remain intact; the effect on fresh scanner results is still unverified.

The synchronous compressor now separates loading, block encoding and output
flushing, preserving stable-buffer remainder handling and end-of-frame reset.
Thirty buffered/stable input/output and explicit-flush cases pass before and
after the rewrite, with independent one-shot, bufferless and chunk-stream
read-back. One migration test initially caught incorrect pipeline ordering:
the compressor rewrite ran before reconstruction of a historical comment-only
fixture. Ordering was corrected and all five dependency contracts pass.
The exact revision recognizer is now separate from atomic publication;
source drift and output-hash rejection remain required. Updated lint and the
changed-function contract/complexity gate pass. This targets alert 1531;
fresh scanner closure remains unmeasured.

The complete native driver passes for the compressor revision and patch-writer
refactor at receipt
`069aeec080da8edb584cfa99ec225164ea92622ba9d0b5ead1097f9f423566f8`:
seven CTest targets, exact CPU/required-HIP corpus read-back at seven block
sizes, rejection controls and controller correctness. Timings are unqualified.
Later fault-source separation passes all five focused targets. Additional
stable-output, one-byte header and magicless stream controls pass before the
remaining decompression-stream rewrite. Their latest native build receipt is
`2a1748c0dbde73190d12e13c60853e9a9b9fe12848937005ea19a535e825ba7a`.
Private evidence includes `out/codeql-repair-bufferless-20261004/summary.json`,
`out/codeql-helper-root-reachability-20261004.json`,
`out/compression-stream-prerewrite-control-20261004.json`,
`out/compression-stream-structural-equivalence-20261004.json`,
`out/compression-stream-refactor-migration-native-20261004.json`,
`out/compression-stream-full-native-20261004.json`,
`out/canonical-fault-source-native-20261004.json` and
`out/decompression-stream-state-baseline-20261004.json`. Hosted closure and
publication acceptance remain outstanding.

The buffered decompression stream now separates header accumulation, the
complete-frame shortcut, memory preparation, input reads/loads, output flushing
and final cursor accounting. Legacy routing, stable output, window limits,
hostage-byte accounting and native hints remain explicit. Position validation
now precedes buffer-offset pointer formation. Source inspection also found
undefined null-pointer arithmetic in permitted empty-buffer paths. Cursor
distance helpers return zero for null empty extents; stable output no longer
advances a null cursor by zero or orders equal null cursors. These are source
correctness repairs; the before/after controls both pass and do not demonstrate
an observed crash in the preceding DLL.

Ten direct dependency test groups now cover invalid positions including
`SIZE_MAX`, null empty input/output, output backpressure and recovery,
one-byte stable-output empty-frame decoding, checksums, magicless frames and
the preceding literal/legacy/training boundaries. The same exact executable
passes against the retained pre-stream-rewrite DLL. All five dependency
contracts pass for native receipt
`7276c5347810735b03b59e2507ca6df23cde34c8f01782f158564e436e96ac11`.
The complete native driver then passes at receipt
`eba3079b62ea8417a5d53f3e0b7a77478e5721680938e7645f8b9fa5299ae926`:
seven CTest targets and all preceding CPU/required-HIP corpus and rejection
controls. Updated lint and the changed-function gate pass. This also targets
alert 1532; scanner closure remains unmeasured.

The next attempted CodeQL build for frozen tree
`030ed9515c5189878023515187ef5833cda1fe51` failed before analysis.
MSBuild reported `MSB4166`: child nodes 2 and 3 exited prematurely. Its
referenced temporary diagnostic directory no longer exists, so the underlying
failure cause is unproved. A native build was overlapping that analysis
without established aggregate memory admission; heavy checks will be serial,
with persistent task-owned diagnostics and revised compiler-memory admission
on the next fresh run. No result from the incomplete database counts as
scanner closure. The last successful CodeQL tree remains the 83-result tree
above and precedes these later stream and fault-source changes.

Private evidence includes `out/decompression-stream-structural-equivalence-20261004.json`,
`out/stream-empty-offsets-transformations-20261004.json`,
`out/decompression-stable-empty-transformations-20261004.json`,
`out/stable-empty-original-control-20261004.json`,
`out/stable-empty-cursor-native-20261004.json`,
`out/streams-full-native-20261004.json` and
`out/codeql-repair-compression-20261004/create.log`. Publication preflight
was refreshed for 28 files: hygiene passes, Gitleaks reports no findings and
15 DevSkim results remain unresolved. The actual selected verifier stops at
that gate; its private receipt is `out/streams-selected-verifier-20261004.json`.
No hosted alert, push or release was changed by this work.

A sixth, serial, fresh CodeQL run completed for frozen tree
`420d6cac59f9e1b0b825d1dbab4ecbd0e8c11b57`. All 183 queries completed without
analysis errors; extraction reports 355 of 397 C/C++ files. The two remaining
modern stream-switch reports (1531 and 1532) and both duplicate test-helper
reachability reports are absent. No new raw reports appear relative to the
preceding successful analysis; 79 raw reports remain, including the six
critical pointer reports. This is local source-bound analysis, not hosted
closure or HIP device qualification. The failed fifth run remains retained
and does not contribute an acceptance claim. Private records are
`out/codeql-repair-streams-20261004/source-manifest.json`,
`out/codeql-repair-streams-20261004/summary.json` and
`out/codeql-repair-streams-20261004/results.sarif`.

Direct dictionary-merger controls subsequently passed against the preceding
implementation: tail/front overlap, containment, adjacency, excluded entries,
equal-savings ties, first/last rank movement, shifted-content inclusion and
failed content matching. The test compiles the exact generated type and
complete helper/merger region verbatim; it records both source and region
identities. Its bridge only marshals bounded fixtures. All five dependency
contracts passed at baseline receipt
`54c4d233d3168a40dd3f0b3ae1f064072458c5ae1f5c8009bf81e7fd6af4dd0e`.
The next merger rewrite separates candidate iteration from selected-item
ranking; its production and scanner verification are still pending. The
33-file publication verifier continues to stop on 15 unresolved DevSkim
findings, with no Gitleaks findings. Evidence is retained in
`out/dictionary-merge-baseline-native-20261004.json`,
`out/dictionary-merge-role-equivalence-20261004.json` and
`out/dictionary-merge-selected-verifier-20261004.json`.

## Latest Individual Metadata Triage

At `ed3ea0f`, five DevSkim results were individually reviewed and dismissed as
false positives with separate GitHub audit comments. This is a disposition,
not automatic scanner closure or a vulnerability fix. No source/test path,
rule or raw result was excluded. Security run `37140763492` has now completed
successfully with all ten jobs passing. The source-condition repair at that
commit automatically fixed alerts 1495–1500 and 1527–1530. Their fresh API
states are `fixed`, with no dismissal, and the exact-SHA lint, Windows CI,
ROCm qualification and Scorecard workflows also pass.

| Alert | Verified metadata role |
| --- | --- |
| [8245](https://github.com/strmt7/SuperZip/security/code-scanning/8245) | The Gitleaks policy quotes the public Git-blob SHA-256 of `tests/cpp/test_sdk_byte_access.cpp`. |
| [8246](https://github.com/strmt7/SuperZip/security/code-scanning/8246) | The policy quotes the public Git-blob SHA-256 of `tests/cpp/sdk_byte_access_checks.hpp`. |
| [8247](https://github.com/strmt7/SuperZip/security/code-scanning/8247) | The corpus descriptor records the observed public dataset-member digest. |
| [8248](https://github.com/strmt7/SuperZip/security/code-scanning/8248) | The corpus descriptor records the observed public UCI archive digest. |
| [1755](https://github.com/strmt7/SuperZip/security/code-scanning/1755) | The graph renderer registers the required SVG XML namespace; it does not fetch that identifier. |

The checksum review bound each alert's current path, highlighted source line,
rule and commit to the complete committed file. Both source digests match their
Git blobs. The authentic acquisition receipt matches the corpus descriptor's
archive/member sizes and hashes and records complete-member CRC verification.
The digests are observed reproducibility values, not credentials or
publisher-signed values. The same-commit secret job passed Gitleaks history,
Gitleaks working-tree and TruffleHog history scanning before disposition.

The SVG review checked the complete renderer's namespace registration and XML
construction, its imports and absence of HTTP-client or URL-fetch operations.
[W3C's namespace definition](https://www.w3.org/TR/SVG2/struct.html#Namespace)
requires `http://www.w3.org/2000/svg`; changing that identifier to HTTPS would
change the XML namespace. Python's
[namespace registration API](https://docs.python.org/3/library/xml.etree.elementtree.html#xml.etree.ElementTree.register_namespace)
registers serialization prefixes and does not download a document.

The new review helper initially compared raw Windows CRLF bytes with a Git
blob, although the existing production reviewer already normalizes only CRLF
to LF. The helper was corrected to use that same contract, while retaining
exact committed identities. A second binding check caught escaped dots in
the Gitleaks regex and the corpus member/archive alert ordering; comments
were corrected against live source locations before any external mutation.
Neither failed check changed an alert or repository line-ending policy.

The inventory after metadata disposition and before CodeQL completion had
192 records and 190 unapproved findings. After CodeQL's ten automatic fixes,
the fresh inventory contains 182 records: 173 CodeQL, seven DevSkim and two
approved Scorecard results. Thus 180 unapproved findings remain, including
all six critical pointer reports. Every open CodeQL/DevSkim instance identifies
`ed3ea0ffe29b00e25e941d3fcf4804463d9fb4c3`.
The six fixed-width SDK copies and serial test allocator remain open;
metadata dispositions do not extend to them. Private complete reviews and API
responses are retained in `out/metadata-finding-dispositions-20261003.json`
and `out/metadata-finding-disposition-results-20261003.json`. Individual fixed
states and the successful hosted run are frozen in
`out/zstd-condition-hosted-closure-20261003.json`. The fresh post-scan audit
still fails with `Unapproved code-scanning alerts are open: 180`; scanner
success and these individual closures do not establish beta acceptance.

## Earlier Hosted Review

This review concerns source `8cb3f4d` and
[security run 37134688991](https://github.com/strmt7/SuperZip/actions/runs/37134688991).
All ten security jobs succeeded. The required post-push audit failed because
195 unapproved alerts remain open. Scanner execution and finding acceptance
are separate gates; this document does not dismiss alerts or authorize release.

## Current Hosted Inventory

The complete incident-history audit retrieved 8,248 records: 197 open, 7,245
fixed and 806 dismissed. Every open record identifies the reviewed source SHA.
Open findings comprise 183 CodeQL, 12 DevSkim and two approved Scorecard records.
The deployment API reported zero records.

No open DevSkim result remains under `docs/benchmarks/data/`. The history still
contains 6,342 fixed and 749 dismissed records in that directory. These are
historical totals, not the number fixed by this batch. Earlier manual metadata
dispositions remain visible; they must not be described as automatic closure.
The guarded passive-data boundary preserves secret scanning and rejects code,
tests, acquisition configuration, redirects and unrecognized report roles.

The 12 remaining DevSkim findings identify four public digest values in scanned
configuration, six fixed-width SDK copies, one test allocator and the SVG XML
namespace. Their source paths remain scanned. The exact four digest reviews
used by local publication preflight do not qualify the hosted alert gate or
extend to any source or test finding.

## Dependency Identity And Reused Dynamic Evidence

Eight relevant generated Zstandard files were freshly compared byte-for-byte
with the pinned 1.5.7 source archive: `cover.c`, `fastcover.c`, `zdict.c`,
`pool.c`, `xxhash.h`, `mem.h`, `zstd_compress.c` and
`zstd_compress_internal.h`. The archive also matches CMake's recorded SHA-256.
The provenance archive was not modified.

The production `libzstd.dll`, guard-page test source and native build recipe
match the earlier successful qualification. The latest retained CTest log
also records all three guard-page groups passing. The overall native receipt
has changed: an unrelated `src/cli/memory_benchmark.cpp` edit changed the CLI
binary. Reuse is limited to the unchanged Zstandard component, not the complete
build or old package identity. No new native build or benchmark was needed.

The existing production-DLL tests cover 74 guarded dictionary suffix lengths,
96 training configurations and 99 frame input/effort combinations at 12 output
capacities. They verify unreadable guard pages, prefix canaries and byte-exact
controls. Finite dynamic tests support the source review; they do not prove
every exported API safe or settle an alert without its caller contract.

## Optimizer Worker Lifetimes

These four `cpp/stack-address-escape` reports concern heap-owned task records
borrowing stack-owned optimizer state:

| Alert | Assignment in the generated 1.5.7 source | Inspected ownership boundary |
| --- | --- | --- |
| [1656](https://github.com/strmt7/SuperZip/security/code-scanning/1656) | `cover.c:1255`, `data->ctx = &ctx` | The iteration waits before destroying `ctx`. |
| [1657](https://github.com/strmt7/SuperZip/security/code-scanning/1657) | `cover.c:1256`, `data->best = &best` | All jobs finish before `best` is destroyed or its owning call returns. |
| [1658](https://github.com/strmt7/SuperZip/security/code-scanning/1658) | `fastcover.c:717`, `data->ctx = &ctx` | The iteration waits before destroying the FASTCOVER context. |
| [1659](https://github.com/strmt7/SuperZip/security/code-scanning/1659) | `fastcover.c:718`, `data->best = &best` | The same synchronized completion and destruction contract applies. |

`COVER_best_start` increments the live-job count before submission. Without a
pool, the worker executes synchronously. With a pool, `COVER_best_wait` holds
the mutex and waits in a condition-variable loop until the count reaches zero.
Every iteration waits before context destruction. Allocation-error cleanup
calls `COVER_best_destroy`, which waits before destroying the shared state,
and only then destroys the context and joins/frees the pool.

Both worker cleanup paths call `COVER_best_finish` after their last context
access. That function updates shared state and signals completion while holding
the mutex. Subsequent worker cleanup releases only worker-owned storage; it
does not dereference `ctx` or `best`. `POOL_free` joins the worker threads before
freeing the pool. An early completion signal therefore does not allow a later
borrowed-state access after destruction.

The reported assignments are valid borrows on these inspected paths; they do
not demonstrate a dangling pointer. No ownership rewrite, added allocation,
rule suppression or alert dismissal was made. These four alerts remain open.
The separate 18 legacy decoder lifetime reports require their own caller and
borrowed-dictionary/output-buffer review.

## Pointer-Boundary Review Map

The six critical pointer alerts still remain open. The retained baseline SARIF
contains 21 flow variants, with the following per-alert mapping to the
[existing bounds review](modernization-audit-2026-10-02.md#bounds-review-progress):

| Alert | Reported operation | Variants | Boundary evidence |
| --- | --- | ---: | --- |
| [1501](https://github.com/strmt7/SuperZip/security/code-scanning/1501) | `xxhash.h:3457`, final byte read | 4 | Dictionary suffix extent ends at the allocation boundary; byte reads require positive remaining length. |
| [1502](https://github.com/strmt7/SuperZip/security/code-scanning/1502) | `xxhash.h:3347`, aligned eight-byte read | 4 | Stripe/tail processing requires the complete eight-byte extent before each read. |
| [1503](https://github.com/strmt7/SuperZip/security/code-scanning/1503) | `xxhash.h:2817`, aligned four-byte read | 2 | Four-byte tail processing requires at least four remaining bytes. |
| [1504](https://github.com/strmt7/SuperZip/security/code-scanning/1504) | `mem.h:305`, first header byte | 4 | Ordinary, raw and split frame callers check header/output capacity before writing; errors propagate before advancing output. |
| [1505](https://github.com/strmt7/SuperZip/security/code-scanning/1505) | `mem.h:306`, second header byte | 4 | The same per-caller header checks cover the second byte. |
| [1506](https://github.com/strmt7/SuperZip/security/code-scanning/1506) | `zstd_compress_internal.h:668`, RLE header | 3 | The shared RLE helper rejects capacity below four before the three-byte header and payload byte. |

The product loader resolves compression/decompression functions, not training
exports. This limits product reachability, but does not justify excluding
training code or declaring the whole dependency safe. No scanner rule or
source path was removed. The remaining findings and final acceptance gate
stay unresolved until their individual reviews and required dispositions
are complete.

## Retained Local Evidence

### October 4 Local Continuation

#### Legacy Dictionary Failure Propagation

The previous continuation stopped before validating an ignored setup result.
Public production-DLL controls now demonstrate that malformed entropy
dictionaries still produced successful output in v0.5, v0.6 and v0.7. The
one-shot entry points ignored `decompressBegin_usingDict` errors and proceeded
to frame decoding. Separately, v0.5/v0.6 read a four-byte dictionary marker
before proving that four initialized bytes existed. Guard-page controls for
one-byte raw dictionaries both terminate with access violation `3221225477`
on the preceding DLL; v0.7's existing short-content branch passes.

The canonical patch now replaces the complete three one-shot functions and
the two short-dictionary loaders. Setup errors return before frame output,
and marker reads require a complete marker extent. Short dictionaries retain
raw-content semantics. Six public-DLL regressions cover all shipped versions,
malformed entropy dictionaries at three sizes, unchanged output on error,
same-context recovery, and 1-3 byte dictionaries ending at an unreadable page.
Old-source results remain in `out/dictionary-old-source-regressions-20261004.json`
and `out/legacy-dictionary-baseline-20261004.json`. Every existing historical
revision and the preceding history rewrite migrate to the same final bytes;
unknown source remains rejected and the upstream archive is unchanged.

The HIP build and all seven CTest targets pass in
`out/dictionary-full-native-20261004.json`, together with 14 CPU/required-HIP
corpus read-back cases, nine rejection controls and independent standard-writer
interoperability. All six isolated ASan targets, including valid/overflow
instrumentation qualification, pass in `out/dictionary-sanitizers-20261004.json`.
Both bind native inputs
`70bc8379378fd228843e4153f700500b43facb81a0a58f09c68fdd10854633d7`, with product
build receipt `d77b2869c34364cbc3a414be531e9d9acc0d90e35fed965145420c69c391fb0b`.
Changed-language lint, changed-function contracts and 31 recurrence controls
pass. The initial new CMake template failed line-length lint; the repaired
source passes. Fresh source-bound CodeQL remains required. No finding closure
or publication acceptance is claimed.

#### Completed Dictionary Analysis Continuation

The interrupted dictionary extraction left an unfinalized database and no
completion receipt or SARIF result. After confirming its process had stopped,
the continuation verified all 819 acquisition inputs against the frozen
snapshot and live checkout. It completed the traced build in that same database
using the documented [trace-command](https://docs.github.com/en/code-security/reference/code-scanning/codeql/codeql-cli-manual/database-trace-command)
and [finalize](https://docs.github.com/en/code-security/reference/code-scanning/codeql/codeql-cli-manual/database-finalize)
operations. No compiled outputs were restored from another analysis. The
original incomplete logs remain retained, and the completed run is explicitly
labelled as resumed extraction.

Both full suites completed all 183 queries with no query-analysis errors at
frozen tree `9f80e60964d32e305eee4ede406839309753e87e`. The raw result has 70
findings, with every rule count unchanged from the preceding history analysis.
It records 284 error-free compilations and 853 source/header files without
extraction errors; these counts include dependencies and compiler headers and
do not establish whole-repository or HIP device coverage. The memory-contained
run peaked at 11,179,905,024 committed bytes within its 15,665,725,440-byte Job
Object ceiling. The completion receipt is
`out/dictionary-codeql-resumed-analysis-20261004.json`, with a complete raw
finding inventory and comparison in
`out/dictionary-analysis-comparison-20261004.json`.

The six critical reports still require individual resolution. Their current
flow counts are separate from the historical 21-variant baseline above:

| Reported boundary | Current flow variants |
| --- | ---: |
| xxHash final byte read | 4 |
| xxHash eight-byte read | 4 |
| xxHash four-byte read | 3 |
| First frame/block header byte | 4 |
| Second frame/block header byte | 4 |
| RLE payload byte | 4 |

The canonical HIP receipt validator passes for unchanged native inputs
`70bc8379378fd228843e4153f700500b43facb81a0a58f09c68fdd10854633d7` and receipt
`d77b2869c34364cbc3a414be531e9d9acc0d90e35fed965145420c69c391fb0b`. The earlier
native, corpus, interoperability and ASan results remain evidence for those
same bytes; they were not rerun or relabelled as new results. The selected
verifier passes changed-file hygiene for 67 paths and then stops at scanner
preflight with zero Gitleaks findings and 21 unresolved DevSkim reports, saved
in `out/resume-verifier-20261004.json`. Fresh GitHub retrieval still shows 182
open alerts, including six critical pointer reports and two permitted
Scorecard policy alerts, in
`out/dictionary-resumed-hosted-open-alerts-20261004.json`. No source finding
disposition, hosted closure, push or publication acceptance is claimed.

#### Earlier History Verification

The resumed batch integrates bounded history and sequence execution in the
three shipped legacy versions. Canonical old-source controls accepted a
`SIZE_MAX` literal length as a zero-byte result while writing output in all
three helpers; v0.7 also formed a history endpoint from an encoded block error.
These are direct helper controls, not evidence of a complete malformed archive
reaching every injected length. The repaired helpers reject the extents before
output writes and keep error results out of history. A byte-by-byte LZ oracle
checks 38,964 valid prefix, dictionary-only, dictionary-crossing and overlapping
sequences, with capacity canaries and separate malformed-length tests.

The HIP build and all five dependency contracts pass in
`out/history-native-migration-repaired-20261004.json`. The initial new test
failed on Windows' `max` macro; its expression was corrected. The historical
migration fixture then rejected a supersession relationship that accepted an
old stream revision as its replacement. That edge was narrowed to the actual
later history revision, and all historical fixtures pass. The expanded migration
matrix took 58.68 seconds; its timeout retains bounded headroom for the
instrumented/shared-host consumer. Changed-language lint and 25 source-policy
negative controls pass. Full native validation and independent standard-writer
interoperability subsequently pass in `out/history-full-native-20261004.json`,
including all seven CTest targets, 14 CPU/required-HIP corpus read-back cases
and nine rejection controls. The corpus controller is correctness evidence;
its timings remain unqualified. All six isolated ASan targets, including the
valid and heap-overflow instrumentation controls, pass in
`out/history-sanitizers-20261004.json`. Both consumers bind native inputs
`af6ebcf82c40cd07bf901aaf53b049539e8c7c8449e6b8f609aa0e19cd43f9b8`; the
product build receipt is
`fb446fbf8f40a92bff65440c069f5cac63309778b97530eaebfebc8067b5bac6`.
CodeQL extraction for frozen tree
`1905bade740435b3842e2c027f7c9aa6ea8da995` failed with `MSB4166` (child node 2)
and `MSB6006` (`CL.exe` exit code 4). It produced no usable analysis. Retained
MSBuild diagnostics and recent application/resource events did not establish
the cause. A fresh retry for tree `ce9e4c13802d491b9772339ecaecf13976429d10`
completed all 183 queries in both full suites with no analysis errors and 70
raw findings, including all six critical pointer reports. It ran strictly
alone, reserved 6,144 MiB per traced compiler, and recorded Job Object memory
peaks. No compiled outputs or prior database were reused. This analysis
predates the dictionary repair above; hosted and publication gates remain
unresolved. The completed receipt is
`out/history-codeql-retry-analysis-20261004.json`.

The source-policy guard now rejects commented-out patch-dispatch calls, with
three additional executable-line mutation controls. The selected offline tool
contracts pass in `out/history-tool-contracts-20261004.json`; the 25-control
source-policy check was rerun after that receipt. MSI identity and portable
package smoke also pass, the latter in `out/history-package-smoke-20261004.json`.
Prior package and stage artifacts were preserved before packaging. These are
local validation results, not an installed-MSI smoke or an approved release.

The selected verifier stops at scanner preflight with 21 unresolved DevSkim
findings and no Gitleaks findings. Each has a retained source hash, location and
boundary assessment in `out/history-preflight-individual-review-20261004.json`;
the current metadata policy admits no source/test finding dispositions. The raw
reports remain intact, and no primitive, rule or path was substituted to reduce
the count. Fresh GitHub retrieval retains 182 open alerts, including six
critical pointer reports and two permitted Scorecard policy alerts, in
`out/history-hosted-open-alerts-20261004.json`. No publication or clean-security
claim follows from the local tests.

The local working tree is ahead of the published analysis and is the primary
development state for this review. A fresh frozen-source analysis at tree
`d43e596667a2b1d4a67aa76ea58ad1b7dea2ad22` ran both security-extended and
security-and-quality suites (183 queries, CLI 2.27.1, cpp-queries 1.9.0), with
no analysis errors. It returned 86 raw findings. Three loop-counter reports
disappeared after the dictionary merger and synchronization rewrites, but the
merger's new test probe imported ten unused constants through a broad private
header. The probe import was narrowed; closure still requires a new extraction.
These local results do not change published alert states or prove readiness.

A subsequent independent extraction at tree
`f275778ce5a279b4a0e2cb0a32a0645ff8c94cde` also completed all 183 queries without
analysis errors and returned 86 findings. Its extraction covered 357 of 405
C/C++ files; the standalone sanitizer project was not part of that CPU-only
product build. Narrowing the dictionary probe alone did not close the ten
unused constants: the raw-block bounds test also imported the complete private
compression header. That consumer now uses an identity-bound canonical writer
probe with only its required upstream declarations. The complete product
sources remain analyzed. Closure of these reports still requires extraction
from the revised source; no alert or analysis result was filtered.

The continuation also found two concrete source defects. Public legacy stream
dispatch replaced a caller's null output pointer even when its capacity was
zero. Three production-DLL regression cases failed on that mutation before the
repair; all now pass. The shipped v0.5-v0.7 buffered implementations also formed
and subtracted null endpoints. Those operations were rewritten with zero-safe
cursor handling and preserved stall/recovery semantics. Direct tests passed on
both revisions; no observed null-arithmetic crash is claimed. Optional v0.4's
historical behavior is retained outside SuperZip's shipped legacy boundary.

An existing raw-block overflow regression then crashed with access violation
`3221225477` on the preceding source. `srcSize + ZSTD_blockHeaderSize` wrapped
before the capacity check. The complete writer now checks fixed overhead and
compares payload with remaining capacity before any access. The same regression
passes after the repair, alongside valid exact/short-capacity controls.

The HIP-enabled build and all five dependency contract targets passed in
`out/raw-block-repair-native-20261004.json`. Build receipt SHA-256 is
`27f1a638ce70efcf133c3d14019d914d3daf380f843405077dba072e3b74ee62`.
The public DLL SHA-256 in the independent pointer-identity check is
`cca05e63e7e7b6302d0134e9ab408f7eca8767d354147e8487293bc587ff4cce`.
Exact patch migration, header contracts, selector/failure-ownership tests and
guarded production-DLL tests passed. New recurrence policy and its negative
controls are documented in `engineering-learning-loop.md`; they do not replace
remaining scanning, direct consumers or final release gates.

The full native test driver subsequently passed all seven CTest targets and
its CPU/required-HIP corpus controls, retained in
`out/continuation-full-native-20261004.json`. The isolated Windows MSVC ASan
qualification also passed six targets, including working positive and
heap-overflow negative controls, in
`out/zstd-sanitizer-qualification-20261004.json`. The shared library/test target
definitions were extracted for reuse by both builds; the product build must
be reverified after that CMake change. Sanitizer validation supplements
product qualification and does not prove all finding closure.

After the shared-module export was repaired, the HIP product build and all five
dependency contracts passed in
`out/shared-zstd-product-contracts-repaired-20261004.json`. The sanitizer driver
then passed all six targets in
`out/zstd-sanitizer-bound-receipt-20261004.json`, including the positive and
heap-overflow negative qualification controls. Its independent success receipt
binds unchanged native inputs
`315e0f44bd41809ddc925c202dc598402231f00d7becaa70d9ef08e40afe994c`
and driver
`76297f9e8b40b2a55ae3a8b087e33f7b40230a9cc294d292437c5bdbf7e72727`.
The later raw-writer probe change requires renewed consumer validation; these
receipts remain evidence for their exact preceding bytes.

The selected verifier remains failed at scanner preflight: 15 DevSkim findings
in the preceding rewrite fragments, with no Gitleaks findings. No source scan,
rule, primitive or path was hidden to obtain acceptance. Six critical CodeQL
reports and the remaining individual findings remain unresolved. No release
was published and no final acceptance is claimed.

Machine-local evidence is ignored and deliberately omitted from packages:

- `out/beta-role-scope-complete-history-20261003.json`: complete incident inventory.
- `out/beta-role-scope-history-audit-20261003.json`: original failing audit result.
- `out/beta-zstd-review-current-provenance-20261003.json`: archive/source/DLL/test
  hashes and the explicit component-only evidence-reuse limit.
- `build/Testing/Temporary/LastTest.log`: existing production-DLL guard-page results.

Revalidate current source, binary and incident identities before reusing this
dated review for a later revision. Neither a green scanner job nor this review
is final beta acceptance.
