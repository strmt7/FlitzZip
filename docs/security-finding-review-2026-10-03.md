# Security Finding Review — 3 October 2026

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
