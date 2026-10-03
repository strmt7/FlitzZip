# Security Finding Review — 3 October 2026

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

Machine-local evidence is ignored and deliberately omitted from packages:

- `out/beta-role-scope-complete-history-20261003.json`: complete incident inventory.
- `out/beta-role-scope-history-audit-20261003.json`: original failing audit result.
- `out/beta-zstd-review-current-provenance-20261003.json`: archive/source/DLL/test
  hashes and the explicit component-only evidence-reuse limit.
- `build/Testing/Temporary/LastTest.log`: existing production-DLL guard-page results.

Revalidate current source, binary and incident identities before reusing this
dated review for a later revision. Neither a green scanner job nor this review
is final beta acceptance.
