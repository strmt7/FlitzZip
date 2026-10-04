# Remaining Individual Finding Review - 4 October 2026

Current status: historical review, superseded as an acceptance mechanism by
the [source remediation redesign](security-source-remediation-redesign.md).
The maintainer subsequently required source remediation of these reports.
All 54 matching alerts now remain blocking. The following text records the
earlier decisions, assumptions, source identities and finite test evidence.
It does not prove harmlessness, describe the redesigned source or establish
closure. In particular, approvals of quality observations are not repairs.

Status: the maintainer approved these exact 54 individual dispositions on
October 4. The approved ledger supplements the separate 27 DevSkim source sites;
raw reports and hosted states remain intact. The complete reference inventory is
the post-push analyses at `7b3b14f` and `fcd29ea`; all eight affected workflows
passed for the SDK follow-up. The caller context was renewed after the separate
SDK header/parser refactor and its 7z regression. All 54 reviewed operations retain
their exact approved source hashes, rules and regions; no Zstandard callers,
patch recipes, generated library sources or analysis configuration changed.
The current native input identity is
`37077f6b861fbe5812c2cdfbd86eccddee21a439fc0fb7895da9e8c2062e3a82`.
Its caller/analysis context is
`f59f6387d84dbf1d7629e4caa282d399c0f22e519ee7640ac0f65d7df1f9b052`.

There are 54 approved individual decisions: 53 CodeQL reports and one DevSkim
allocation report. Every entry below retains its alert identity, rule, full
reported region and normalized whole-file source hash. The approved scope is
these exact operations and their inspected callers, not a directory, query or
dependency exemption. A different source, caller contract, scanner version,
region or analysis context requires renewed review. Complete raw reports must
remain available and every unmatched finding must stay blocking.

## Critical Pointer Reports: 1501-1506

These approved false-positive dispositions apply to the repaired source. They
do not excuse the earlier reproduced COVER shrinking defect, raw-block size
overflow, legacy history defects or dictionary setup failures. Those received
production repairs and retained old-source controls before this review.

The current local CodeQL result contains 23 flow variants across these six
locations: four final-byte reads, four eight-byte reads, three four-byte reads,
four first-header-byte writes, four second-header-byte writes and four RLE
payload writes. The raw SARIF is retained in
`out/codeql-repair-dictionary-20261004/resumed-results.sarif`; a source/sink
projection is in `out/current-critical-flow-review-20261004.txt`. Repeated
configurations are retained as separate variants.

For 1501-1503, the originating allocations are the ordinary COVER/FASTCOVER
dictionary buffers and the intercepted COVER test buffer. Both builders begin
with tail equal to capacity. Each copied segment is bounded by the remaining
tail before tail decreases. Therefore tail stays within [0, capacity], and
the initialized suffix has exactly capacity minus tail bytes. Its end is the
allocation end. Allocation failures take cleanup without calling selection.

`COVER_selectDict` preserves that initialized input extent independently of
the finalized output size. It passes the original extent to the first
finalization; every shrinking candidate is bounded by initialized content and
selected backwards from its end. The next candidate multiplication requires
a value no greater than half the initialized content size. Finalization hashes
only the supplied input extent before entropy analysis or output relocation.
The 680-byte input/808-byte finalized-dictionary reproducer now rejects the
old overextended input geometry and preserves a valid full-dictionary fallback.

xxHash processes stripes only when at least 32 input bytes remain. Each stripe
contains four eight-byte reads. Tail processing uses length modulo 32, permits
eight-byte reads only with at least eight remaining bytes, four-byte reads only
with at least four, and final-byte reads only with a positive remainder. When
tail equals capacity, input length is zero and none of these reads execute.
Alignment-specific variants select their appropriate aligned or unaligned
reader; the byte extent is unchanged. These guards supply the correlation
between the allocation, suffix pointer and length missing from the report.

For 1504-1506, `COVER_checkTotalCompressedSize` allocates precisely the output
capacity subsequently passed to compression. Missing allocation/context/CDict
takes cleanup. Frame-header writing rejects a capacity below the maximum
header size; its successful result is within that capacity. Errors return
before advancing the output or subtracting the header size. The subsequent
frame/block paths carry the remaining capacity with the advanced pointer.

The ordinary frame loop requires header plus minimum compressed-block space
before a header write. Split-block compression passes the current remaining
capacity into the single-block helper, which rejects a missing three-byte
header before subtracting it. Each successful partition advances by its
returned size; errors propagate before that advance. Raw blocks now compare
payload size against capacity minus header size, avoiding the former addition
overflow. Every target/split/ordinary RLE route reaches the same helper, which
requires four bytes before its three-byte header and payload byte. Thus the
reported first/second header and fourth payload writes have complete extents.

Evidence includes production-DLL guarded output/truncation tests, actual
finalizer observation, shrinking selection controls, raw-writer overflow
controls and canonical MSVC ASan contracts. Finite tests support the inspected
bounds argument; they are not a universal memory-safety proof. The six isolated
ASan targets passed for the preceding SDK identity
`50dc4c45dc352aacab3080aaf4e91b18bf3b2b8e7de5bb7aa5ed18715a127186`;
the reviewed Zstandard sources and their ASan inputs are unchanged. No new broad local analysis
was run during this review.

## Packed Table Reports: 1507-1526

The approved disposition is incorrect element-type inference, rather than an
unbounded offset. Reports 1507-1510 describe the packed FSE representation:
one four-byte header comprises two two-byte values. The state table starts
after those two values, so adding two to a U16 pointer advances four bytes.
The symbol-transform region begins after the header and the U16 state-table
payload, using the corresponding U32-unit count. The RLE table reserves two
state values and places transforms after two U32 cells. The maximum-symbol
reader advances one U16 cell to the second header field. None of these offsets
is a byte count mistakenly applied as a U16 element count.

Reports 1511-1526 concern the X1 Huffman table payload. `HUF_DEltX1` contains
two byte fields; a 64-bit write fills four such entries. Four-entry expansion
uses one write. Eight-entry expansion uses offsets zero and four. Larger
power-of-two expansions use offsets zero, four, eight and twelve in sixteen
entry steps. The offsets count two-byte table entries, not four-byte backing
storage words.

The weight reader rejects invalid weights and table logs, reconstructs the
implied final weight only when the remaining weight is a power of two, and
requires a valid rank-one count. Consequently the weighted lengths sum to the
complete power-of-two decoding table. Rescaling preserves that sum. The X1
builder checks the resulting table log against the destination descriptor;
rank expansion fills that bounded payload. Larger default-case lengths are
at least sixteen and divisible by sixteen. The descriptor and allocation
macros define storage for those entries. Production compressed-frame readback,
malformed-input and ASan contracts remain required; reinterpretation of this
packed layout must receive its own representation review.

## Borrowed Storage Reports: 1656-1659 and 8270-8281

The four optimizer assignments put addresses of stack-owned context/best state
into heap-owned task records. The synchronous branch consumes the record in
the same call. The pool branch increments live jobs before submission. Workers
finish all context reads before reporting completion; afterward they release
only their own task/temporary allocations. The normal context teardown waits
for live jobs; allocation-error teardown also waits through best destruction.
Pool destruction joins actual worker threads before the optimizer returns.
Neither borrowed stack object becomes a returned task or retained global.
The earlier individual lifetime review and current cleanup paths agree.

The nine production legacy assignments borrow dictionary/output storage as
history in v0.5-v0.7. They do not create an owner or extend the lifetime of a
callee-local array. One-shot callers retain dictionary, input and output for
the complete decode. The buffered legacy drivers keep decoder-owned history
buffers alive across calls. Direct bufferless use requires caller-owned
dictionary and previous output to remain live for later history access; that
precondition is explicit in the changed function contracts. Tests retain
their stack arrays through all native accesses. Context release does not
dereference or free caller-borrowed history.

The three intercepted v0.7 assignments have the same generated bytes and
contracts. Two retain live output history. The third borrows raw literal bytes
from the current compressed input only after proving that the direct-reference
branch has its required padding. The block decoder consumes those literals
during the current call; the next block publishes its own literal state before
sequence use. Buffered input belongs to the decoder, and test input remains
alive for the whole call. This does not permit a caller to destroy borrowed
dictionary/history early. Existing 38,964 byte-oracle sequences, golden frames,
dictionary lifetime, output-backpressure and allocation-failure controls
exercise these contracts, including the canonical intercepted implementation.

## Algorithm Progress Reports: 1642, 1643, 1645, 1646

These are approved justified quality dispositions. In the lazy and optimal
match searches, setting the comparison budget to zero immediately precedes a
break and deliberately skips the later dictionary search after reaching the
input end. The for-loop decrement does not execute after break, so it cannot
underflow the zero budget. Retaining a positive budget would request a search
whose input end has already been reached.

The two suffix-sort reports concern run consumption. Inner loops consume an
entire equal-rank/nonnegative or complemented-negative run, while the outer
step proceeds to the next unconsumed position. The predecessor check and run
sentinel govern the descent; i never increases inside either run. A second
counter advancing once per outer iteration would not express this algorithm's
progress. The public legacy dictionary trainer and existing suffix-ranking
consumer controls cover this representation. These reports do not justify
accepting unrelated loop-body counter mutation.

## Header/API Reports: 1635, 1637, 1638, 1640, 1687, 1740

The four library headers deliberately support staged inclusion: public APIs
first, followed by static-only or inline implementation opt-in. Distinct
sections have their own guards. A whole-file pragma-once wrapper would prevent
the second stage and break the compiled C/C++ header contracts. xxHash's
Zstandard adaptation macros precede its public header section by design.

The CMake ABI file is a pinned tool's private compiler-probe input, included
once by its standalone ABI translation unit. Its definitions encode compiler
metadata in an inspected probe binary. It is not installed or linked into the
archive application as a reusable product header. This is a justified upstream
tool-quality disposition, not permission to exclude tools from scanning or
edit an installed CMake distribution.

`ZSTD_defaultCMem` is a header-defined default allocator value used by the
canonical legacy driver and first/repeated-inclusion allocation contracts.
Each translation unit has its own constant; a consumer that only uses other
header APIs can have an unused copy. Removing the declaration would break
actual default-allocation consumers. It is not mutable retained state.

## Preserved Research Note: 1634

This is approved acceptance of a quality observation, not a claim that the
comment is absent or its optimization research completed. The upstream note
describes historical GCC-versus-Clang performance for the 32-bit SSE2 XXH3
branch. SuperZip targets Windows x64, and this Zstandard header defines
`XXH_NO_XXH3`. No runtime security repair follows from deleting the note.
The historical benchmark is not a current SuperZip performance claim.

## Serial Fault Allocation: 1734

The approved DevSkim disposition concerns `malloc(bytes)` in the deterministic
C fixture, not an unchecked arithmetic expression at that call. It adds no
header size or element multiplication. Successful allocations are registered
in a fixed 32-slot ownership table; full tracking or injected/real allocation
failure returns null. Reset refuses to abandon live allocations, release frees
each registered address once, and invalid/double releases are counted without
repeating free. Fixture calls are serial. Allocation-size arithmetic in the
actual decoder remains subject to its separate production review and tests.
Replacing malloc with a renamed primitive would not repair an integer error.

## Exact Findings and Source Identities

The tables below are generated from the complete preserved inventory. Source
digests normalize Git CRLF to LF. Zstandard generated source and its fault
clone derive from the pinned archive and canonical patch inputs covered by the
native identity above. Admission also requires renewed review if any native
input changes, so caller or recipe changes cannot inherit these decisions.
CodeQL producer version is 2.27.1, category `/language:c-cpp`; DevSkim producer
version is `1.0.100+ea92e6f3cc`, category `devskim`. A new analysis must have the
same relevant bytes and exact full regions before any disposition is applied.

| Source | Reported file | Normalized SHA-256 |
| --- | --- | --- |
| S01 | `build/tests/zstd_legacy_fault_sources/zstd_v07.c` | `2fe88d3ccb50c4c0cf26da2aff6554d67af968f2a40b930ba8ac212f12fe72c8` |
| S02 | `build/zstd-v1.5.7-source/zstd-1.5.7/lib/common/fse.h` | `5235ce1e512bf80204d013b1f4cecd776fdaa9effddb48308bce08f6d0b84499` |
| S03 | `build/zstd-v1.5.7-source/zstd-1.5.7/lib/common/mem.h` | `4dd5fe76fa30a020cd4b3af3134ae143920300a1fe54baac1a5da73297e66783` |
| S04 | `build/zstd-v1.5.7-source/zstd-1.5.7/lib/common/xxhash.h` | `327a38e032aa6daccd0145fce664d6c65d001290955fd9c9d47aedd6fcc9cb41` |
| S05 | `build/zstd-v1.5.7-source/zstd-1.5.7/lib/compress/fse_compress.c` | `6dfd0803dcb03b6ce85fc180cef7ea306fe2eb91fa97e01f2511775fbbe733ea` |
| S06 | `build/zstd-v1.5.7-source/zstd-1.5.7/lib/compress/zstd_compress_internal.h` | `94551c31098dd5029efd64d7fc3e7688a75e5d774c59cc9f2f1b82fc4ecb4662` |
| S07 | `build/zstd-v1.5.7-source/zstd-1.5.7/lib/compress/zstd_compress_sequences.c` | `3f4334c19ed770c4007bc12df4098026d4eb1a6fbebfbf36df58054e7babc3e9` |
| S08 | `build/zstd-v1.5.7-source/zstd-1.5.7/lib/compress/zstd_lazy.c` | `8f44e994583aacf9ed95004614d7fada1f54f470473d52080fc10836ba3faffe` |
| S09 | `build/zstd-v1.5.7-source/zstd-1.5.7/lib/compress/zstd_opt.c` | `4952125ea426e028d0aba6c94175debe49d941e41bc873a52f66d868399b0d32` |
| S10 | `build/zstd-v1.5.7-source/zstd-1.5.7/lib/decompress/huf_decompress.c` | `cbac3cc9f9490f35c031587267193f419aefa6a6de352673e2ed008befc34d80` |
| S11 | `build/zstd-v1.5.7-source/zstd-1.5.7/lib/dictBuilder/cover.c` | `01cdcb0f00af494a327b9fe8763d82b1d1bbbc63cc824669d26362dfa199126c` |
| S12 | `build/zstd-v1.5.7-source/zstd-1.5.7/lib/dictBuilder/divsufsort.c` | `a2d8d333895d0e54db6af52cc618c60007a0e919a42e58523a3a4dc4d3ddfd9b` |
| S13 | `build/zstd-v1.5.7-source/zstd-1.5.7/lib/dictBuilder/fastcover.c` | `0b8b511e6370e89cb257d1a4b4d8e77add5537ffb999cfe5e7e59e454c93f38f` |
| S14 | `build/zstd-v1.5.7-source/zstd-1.5.7/lib/legacy/zstd_v05.c` | `bd627a804a8fd98dcc580cef65b3f190cac2f01e6136a9d5f0cc485ff36d08aa` |
| S15 | `build/zstd-v1.5.7-source/zstd-1.5.7/lib/legacy/zstd_v06.c` | `4d8f737c5613cc1abbee283b383e3b97768448f244282511fef502c2b774baed` |
| S16 | `build/zstd-v1.5.7-source/zstd-1.5.7/lib/legacy/zstd_v07.c` | `2fe88d3ccb50c4c0cf26da2aff6554d67af968f2a40b930ba8ac212f12fe72c8` |
| S17 | `build/zstd-v1.5.7-source/zstd-1.5.7/lib/zdict.h` | `65f365e97b6c7b4ba47f1dfa0d76c9ab5fce62ce8c5394390d76c2136c1d1384` |
| S18 | `build/zstd-v1.5.7-source/zstd-1.5.7/lib/zstd.h` | `9b4bc8245565c98ccfc61c07749928b57e7c0f6fddb0530c4f6aa1971893d88b` |
| S19 | `out/tools/cmake-4.4.3/cmake-4.4.3-windows-x86_64/share/cmake-4.4/Modules/CMakeCompilerABI.h` | `ea65dd2748e5187089b209f0a4260a44a6190c20ec4513b8031e5bc4f963b539` |
| S20 | `tests/zstd/fault_allocator.c` | `8d90ca576dfdf3d643cb43a12cf34f6793a576ccc290a613b1e9d2fa9117194f` |

| Alert | Rule | Source | Full region (line:column, end exclusive) |
| --- | --- | --- | --- |
| [1501](https://github.com/strmt7/SuperZip/security/code-scanning/1501) | `cpp/invalid-pointer-deref` | S04 | 3457:18-3457:24 |
| [1502](https://github.com/strmt7/SuperZip/security/code-scanning/1502) | `cpp/invalid-pointer-deref` | S04 | 3347:40-3347:60 |
| [1503](https://github.com/strmt7/SuperZip/security/code-scanning/1503) | `cpp/invalid-pointer-deref` | S04 | 2817:40-2817:60 |
| [1504](https://github.com/strmt7/SuperZip/security/code-scanning/1504) | `cpp/invalid-pointer-deref` | S03 | 305:9-305:25 |
| [1505](https://github.com/strmt7/SuperZip/security/code-scanning/1505) | `cpp/invalid-pointer-deref` | S03 | 306:9-306:30 |
| [1506](https://github.com/strmt7/SuperZip/security/code-scanning/1506) | `cpp/invalid-pointer-deref` | S06 | 670:5-670:16 |
| [1507](https://github.com/strmt7/SuperZip/security/code-scanning/1507) | `cpp/suspicious-pointer-scaling` | S02 | 434:28-434:34 |
| [1508](https://github.com/strmt7/SuperZip/security/code-scanning/1508) | `cpp/suspicious-pointer-scaling` | S05 | 531:30-531:33 |
| [1509](https://github.com/strmt7/SuperZip/security/code-scanning/1509) | `cpp/suspicious-pointer-scaling` | S05 | 75:36-75:39 |
| [1510](https://github.com/strmt7/SuperZip/security/code-scanning/1510) | `cpp/suspicious-pointer-scaling` | S07 | 49:41-49:47 |
| [1511](https://github.com/strmt7/SuperZip/security/code-scanning/1511) | `cpp/suspicious-pointer-scaling` | S10 | 488:33-488:35 |
| [1512](https://github.com/strmt7/SuperZip/security/code-scanning/1512) | `cpp/suspicious-pointer-scaling` | S10 | 495:33-495:35 |
| [1513](https://github.com/strmt7/SuperZip/security/code-scanning/1513) | `cpp/suspicious-pointer-scaling` | S10 | 496:33-496:35 |
| [1514](https://github.com/strmt7/SuperZip/security/code-scanning/1514) | `cpp/suspicious-pointer-scaling` | S10 | 496:33-496:44 |
| [1515](https://github.com/strmt7/SuperZip/security/code-scanning/1515) | `cpp/suspicious-pointer-scaling` | S10 | 504:37-504:39 |
| [1516](https://github.com/strmt7/SuperZip/security/code-scanning/1516) | `cpp/suspicious-pointer-scaling` | S10 | 504:37-504:48 |
| [1517](https://github.com/strmt7/SuperZip/security/code-scanning/1517) | `cpp/suspicious-pointer-scaling` | S10 | 504:37-504:52 |
| [1518](https://github.com/strmt7/SuperZip/security/code-scanning/1518) | `cpp/suspicious-pointer-scaling` | S10 | 505:37-505:39 |
| [1519](https://github.com/strmt7/SuperZip/security/code-scanning/1519) | `cpp/suspicious-pointer-scaling` | S10 | 505:37-505:48 |
| [1520](https://github.com/strmt7/SuperZip/security/code-scanning/1520) | `cpp/suspicious-pointer-scaling` | S10 | 505:37-505:52 |
| [1521](https://github.com/strmt7/SuperZip/security/code-scanning/1521) | `cpp/suspicious-pointer-scaling` | S10 | 506:37-506:39 |
| [1522](https://github.com/strmt7/SuperZip/security/code-scanning/1522) | `cpp/suspicious-pointer-scaling` | S10 | 506:37-506:48 |
| [1523](https://github.com/strmt7/SuperZip/security/code-scanning/1523) | `cpp/suspicious-pointer-scaling` | S10 | 506:37-506:52 |
| [1524](https://github.com/strmt7/SuperZip/security/code-scanning/1524) | `cpp/suspicious-pointer-scaling` | S10 | 507:37-507:39 |
| [1525](https://github.com/strmt7/SuperZip/security/code-scanning/1525) | `cpp/suspicious-pointer-scaling` | S10 | 507:37-507:48 |
| [1526](https://github.com/strmt7/SuperZip/security/code-scanning/1526) | `cpp/suspicious-pointer-scaling` | S10 | 507:37-507:52 |
| [1634](https://github.com/strmt7/SuperZip/security/code-scanning/1634) | `cpp/fixme-comment` | S04 | 4665:5-4679:8 |
| [1635](https://github.com/strmt7/SuperZip/security/code-scanning/1635) | `cpp/missing-header-guard` | S02 | 1:1-1:1 |
| [1637](https://github.com/strmt7/SuperZip/security/code-scanning/1637) | `cpp/missing-header-guard` | S18 | 1:1-1:1 |
| [1638](https://github.com/strmt7/SuperZip/security/code-scanning/1638) | `cpp/missing-header-guard` | S04 | 1:1-1:1 |
| [1640](https://github.com/strmt7/SuperZip/security/code-scanning/1640) | `cpp/missing-header-guard` | S17 | 1:1-1:1 |
| [1642](https://github.com/strmt7/SuperZip/security/code-scanning/1642) | `cpp/loop-variable-changed` | S08 | 346:25-346:35 |
| [1643](https://github.com/strmt7/SuperZip/security/code-scanning/1643) | `cpp/loop-variable-changed` | S09 | 755:54-755:64 |
| [1645](https://github.com/strmt7/SuperZip/security/code-scanning/1645) | `cpp/loop-variable-changed` | S12 | 1578:47-1578:48 |
| [1646](https://github.com/strmt7/SuperZip/security/code-scanning/1646) | `cpp/loop-variable-changed` | S12 | 1583:51-1583:52 |
| [1656](https://github.com/strmt7/SuperZip/security/code-scanning/1656) | `cpp/stack-address-escape` | S11 | 1247:7-1247:23 |
| [1657](https://github.com/strmt7/SuperZip/security/code-scanning/1657) | `cpp/stack-address-escape` | S11 | 1248:7-1248:25 |
| [1658](https://github.com/strmt7/SuperZip/security/code-scanning/1658) | `cpp/stack-address-escape` | S13 | 717:9-717:25 |
| [1659](https://github.com/strmt7/SuperZip/security/code-scanning/1659) | `cpp/stack-address-escape` | S13 | 718:9-718:27 |
| [1687](https://github.com/strmt7/SuperZip/security/code-scanning/1687) | `cpp/missing-header-guard` | S19 | 1:1-1:1 |
| [1734](https://github.com/strmt7/SuperZip/security/code-scanning/1734) | `DS161085` | S20 | 42:22-42:35 |
| [1740](https://github.com/strmt7/SuperZip/security/code-scanning/1740) | `cpp/unused-static-variable` | S18 | 1880:22-1880:38 |
| [8270](https://github.com/strmt7/SuperZip/security/code-scanning/8270) | `cpp/stack-address-escape` | S01 | 3669:9-3669:25 |
| [8271](https://github.com/strmt7/SuperZip/security/code-scanning/8271) | `cpp/stack-address-escape` | S01 | 3670:9-3670:35 |
| [8272](https://github.com/strmt7/SuperZip/security/code-scanning/8272) | `cpp/stack-address-escape` | S01 | 3343:5-3343:35 |
| [8273](https://github.com/strmt7/SuperZip/security/code-scanning/8273) | `cpp/stack-address-escape` | S14 | 3311:5-3311:22 |
| [8274](https://github.com/strmt7/SuperZip/security/code-scanning/8274) | `cpp/stack-address-escape` | S14 | 3300:9-3300:25 |
| [8275](https://github.com/strmt7/SuperZip/security/code-scanning/8275) | `cpp/stack-address-escape` | S14 | 3301:9-3301:35 |
| [8276](https://github.com/strmt7/SuperZip/security/code-scanning/8276) | `cpp/stack-address-escape` | S15 | 3449:5-3449:22 |
| [8277](https://github.com/strmt7/SuperZip/security/code-scanning/8277) | `cpp/stack-address-escape` | S15 | 3438:9-3438:25 |
| [8278](https://github.com/strmt7/SuperZip/security/code-scanning/8278) | `cpp/stack-address-escape` | S15 | 3439:9-3439:35 |
| [8279](https://github.com/strmt7/SuperZip/security/code-scanning/8279) | `cpp/stack-address-escape` | S16 | 3680:5-3680:22 |
| [8280](https://github.com/strmt7/SuperZip/security/code-scanning/8280) | `cpp/stack-address-escape` | S16 | 3669:9-3669:25 |
| [8281](https://github.com/strmt7/SuperZip/security/code-scanning/8281) | `cpp/stack-address-escape` | S16 | 3670:9-3670:35 |
