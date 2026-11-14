# Compatibility Compression Stream Contracts

## Shared CPU Writers

Zstandard streams with an exact size of at least 16 MiB use up to four
compression workers at product efforts 1-6. Selection is capped by one worker
per complete 8 MiB of input, one per four reported logical processors, and
four workers overall. Fewer than four reported processors, unknown sizes,
smaller inputs, and efforts 7-9 use synchronous
compression. High-effort operation preserves its larger history and avoids
multiplying job buffers. Both standalone and TAR.ZST creation report the
selected worker count. This is CPU compression.

The runtime is built with threading enabled from the pinned upstream source.
Its stable worker parameter is checked before any compression begins. Input
buffers remain borrowed only until libzstd has consumed or copied them.
Finalization waits for all jobs and captures the completed codec workspace
before releasing the context. Querying live workspace after input has been
accepted in threaded mode throws because upstream pool accounting is incomplete
while jobs are active. The completion snapshot excludes process and caller
overhead and is not a peak-memory measurement. Threaded output remains a single
checksummed standard frame, but job boundaries can change compressed size.
Standalone copying uses a 128 KiB buffer; each stream buffer is 129 KiB, enough
for a maximum-size Zstandard block and encoded framing. Both standalone and
TAR.ZST paths share the same stream implementation and worker admission.
These bounded buffers are not a whole-file staging allocation.

| Stream | Product Writers | Effort Policy |
| --- | --- | --- |
| Gzip | Gzip, TAR.GZ, CPIO.GZ | Miniz levels 1-9 |
| Bzip2 | Bzip2, TAR.BZ2 | Libbzip2 block-size levels 1-9 |
| Zstandard | Zstandard, TAR.ZST | Product levels 1-9 mapped to bounded backend settings |

The standalone Gzip writer now uses the same stream implementation as its
container wrappers. Framing, Deflate state, CRC32/ISIZE accounting, compressor
cleanup, and finalization are no longer maintained in two separate writers.
Its file locking, progress callbacks, temporary output, atomic publication,
and interrupted-operation cleanup remain owned by the standalone adapter.

All three stream constructors reject invalid effort before creating or
truncating a destination. Previously, a rejected level could still change an
existing file. Regression tests first reproduced that behavior in all three
implementations, then verified preservation for existing and missing paths at
levels 0, 10, and both signed integer extremes.

## ZIP Creation Progress

ZIP creation uses the pinned Miniz source callback interface with its existing
64 KiB input windows. Progress within a member describes the preceding completed
input window, so an observer can cancel before the next window is read. Empty
members remain valid and count no source bytes. Each source stays locked until
its entry has been added; no whole-file input staging is introduced.

The callback latches exceptions rather than unwinding through Miniz. After the
C call releases its compression workspaces, the original exception is rethrown.
The archive transaction then removes private output and preserves the existing
destination. Source reads must match the pinned declared length; incomplete input
cannot silently become a successful shorter entry.

The writer retains the original file timestamp, compression flags, input
partition and CRC policy. Complete ZIP byte-parity controls compare against
Miniz's unchanged file interface at all nine efforts for empty and nonempty
sources. Separate cancellation controls check destination preservation, source
handle release and temporary cleanup; independent readers qualify interoperability.
These are functional contracts, not measured throughput or latency claims.

## ZIP Decoded Output Windows

Extraction and read-only verification share the bounded decoded-size, progress
and exception bridge. Extraction supplies a borrowed synchronous stream for its
reserved private file; verification explicitly discards decoded bytes. Successful
writes advance progress within a member, and observer exceptions return through
Miniz before the original exception is rethrown. No exception unwinds through C.

The private output must decode completely, pass Miniz's existing CRC checks and
close successfully. Its archived modification timestamp is restored through the
Windows wide-character file API, including Unicode and extended-length paths.
A timestamp failure is reported before publication. This repairs the narrow-path
timestamp attempt whose error Miniz's file helper ignored.

Only then does the existing guarded per-file commit run. Cancellation or failure
before that commit preserves the previous target's bytes and timestamp and cleans
the reserved output. Earlier completed members can remain after a later failure;
extraction is not an atomic directory replacement. Stored and Deflate, empty and
nonempty members have byte, timestamp, progress and cancellation controls using
archives produced through the independent pinned file interface.

## Standalone Extraction Cancellation

Standalone Gzip, Bzip2, Zstandard, XZ, Lzip and LZMA-Alone extraction report
synchronous progress between bounded decoder/output windows. A callback can
cancel without waiting for the entire file to decode. Bzip2, XZ, Lzip and
LZMA-Alone retain 64 KiB windows; Zstandard retains its 128 KiB transfer limit.
These boundaries do not interrupt an individual codec call.

Window checkpoints preserve the existing compressed-input progress units;
repeated snapshots are cancellation opportunities, not additional completed
bytes. Final codec validation and output close still precede guarded publication.
Callback failures before publication preserve the existing target, remove private
output and release source handles. A later retry starts with fresh decoder state.
Completion notifications occur after publication and do not undo committed files.

Fixed multi-window fixtures exercise absent and existing destinations, original
callback exceptions, cleanup and successful byte-exact recovery. Independently
encoded XZ, Lzip and marked/unmarked LZMA-Alone frames complement the standard
writer-based Gzip, Bzip2 and Zstandard controls. These are correctness tests,
not latency or throughput measurements.

## Stored Container Cancellation

AR, ARC, ARJ and ISO expose their initial validation phase before creating output.
That phase has unknown totals: metadata records and, where required, bounded
payload CRC reads provide cancellation opportunities without claiming completed
extraction bytes. Format checks and complete path-set validation remain required.
The synchronous scan borrows the original observer, preserving stateful callbacks.

Their stored-file extraction checks cancellation between the existing 64 KiB copy
windows and after closing private output, before guarded publication. Payload bytes
advance once per successful copy; completion notifications follow publication.
Earlier published members can remain after a later cancellation. AR creation uses
the same bounded copy checkpoints and checks again before publishing its complete
archive, preserving an existing archive when interrupted.

Shared public-API controls cover validation and copying separately, existing and
missing destinations, callback state, source-handle release, cleanup and exact
retry results. The controls use independently authored stored ARC, ARJ and ISO
framing alongside the AR writer. They do not establish latency or throughput.

## Shared IEEE Integrity Backend

ZIP and Gzip use Miniz's documented external-CRC hook to share the existing
capability-aware IEEE backend with FLZIP and 7z. TAR.GZ and CPIO.GZ receive
the same implementation. There is one thread-safe initialization guard, no new
dependency, no CRC32C substitution, and no change to required checks, effort,
or durable publication. The C ABI resets on null initialization and preserves
a non-null empty range's low 32 seed bits. Bitwise-oracle, fragmentation,
alignment, and concurrent-first-use tests verify the contract.
See the [controls and measurement limits](https://github.com/strmt7/FlitzZip/blob/37e8bc1f03bfc02aa74b5ed12d8f303126df67b2/docs/history/benchmarks/2026-10-01-compatibility-crc-hook.md);
an isolated checksum gain is not an end-to-end performance guarantee.

## Lifetime And Byte Identity

`tests/cpp/test_compression_streams.cpp` applies shared checks to all three
codecs at every product level:

- Empty streams and mixed binary data with embedded NULs and repeated ranges.
- Whole writes versus fragments spanning internal buffer boundaries.
- Immediate overwriting of caller buffers after fragmented writes.
- Exact encoded-byte equality across those write partitions.
- Byte-exact decoding and accurate input/output counters.
- Repeated close calls and rejection of writes after close without file changes.

Standalone Gzip additionally has wire-parity tests against the shared stream
at every level, covering empty and nonempty files. These passed before its
duplicate compressor was removed. A callback-interruption test verifies
destination preservation and temporary cleanup.

Gzip and Bzip2 shared writers consume borrowed immutable input synchronously. A local
RAII guard clears its input pointer/count on every exit, including exceptions.
This removes an input staging copy and a 64 KiB buffer per writer while
preserving the existing input-chunk limits. Libbzip2's legacy non-const pointer
is borrowed only during synchronous compression; its pinned implementation
reads caller bytes without modifying them. This does not make the codecs
themselves zero-copy or establish an end-to-end speedup. When measurements are
permitted, comparisons must monitor CPU, GPU, RAM, and storage contention before
and throughout the run. Historical reports do not override a current task's
measurement restriction.

Independent CLI checks decoded a 262,161-byte mixed binary fixture at all nine
efforts using Python 3.12.14's Gzip, Bzip2, and ZIP readers and the separately
installed `zstandard` 0.25.0 reader. All 36 outputs restored the original bytes.
These bounded filesystem checks establish interoperability, not throughput or
universally distinct sizes: several efforts produced equal sizes on this
fixture, and Bzip2/Zstandard sizes were not uniformly monotonic.

## Shared CPU Readers

Standalone Gzip, Bzip2, Zstandard, XZ and Lzip extraction can transfer a bounded decoded
window directly to the output stream through `DecodedInputBuffer`. The reader
continues to own the get area, so standard reads, peek and putback retain their
existing behavior. The transfer adds no buffer, queue, thread or runtime dependency.
It removes the extractor's intermediate plaintext array: 64 KiB for Gzip, Bzip2,
XZ and Lzip, and 128 KiB for Zstandard. Codec history and output-sink buffering remain separate.
This is a dataflow/storage change, not measured throughput evidence.

`transfer_to` writes at most one window. A short positive result is not EOF;
callers continue until zero and still call `finish()` before publication. The
method returns its count without changing `istream` flags or `gcount`. Source
errors propagate; a failed sink leaves the source window unconsumed even if the
sink accepted a prefix. Callers own output rollback and must serialize source
operations, including during sink calls. The standalone adapters retain their
write diagnostics, resource limits, finish validation and publication transaction.

## Compressed-Container Inspection Windows

`DecodedInputBuffer::consume` exposes one bounded owned window to a synchronous
visitor. The visitor must neither retain its span nor reenter the source. It
advances the source only after a successful visit. `transfer_to` uses this same
primitive, preserving its failed-sink, putback, zero-request and exception rules.
A short positive visit is a window boundary; only zero means EOF.

The shared exact-byte consumer uses these windows for decoded streams and bounded
64 KiB reads for ordinary streams. An unreadable stream is rejected before source
work; a zero-length request performs no I/O. Truncation and source/visitor errors
propagate, and earlier windows may already have been consumed. Buffered reads
consume input before invoking the visitor; callers own rollback and must not
assume a failed operation rewinds the stream.

TAR and CPIO payload/padding discard paths now use the common consumer. CPIO CRC
inspection sums unsigned bytes directly from decoded windows; its seekable and
streaming payload writers share the same byte-sum primitive modulo `2^32`. These
changes remove the extra plaintext staging copy from decoded inspection without
skipping decompression, checksums, entry extents or final stream validation.

Extraction output keeps its existing write batching. Borrowing smaller decoder
windows can change sink call counts, so extending direct output is a separate
measurement-dependent step. No throughput, peak stack-memory or whole-codec
zero-copy claim follows from the pointer-identity and byte-exact controls.

## TAR And CPIO Extraction Checkpoints

Plain and filtered TAR/CPIO extraction observe cancellation during the initial
metadata and payload-validation pass, before creating destination directories.
These validation snapshots have unknown totals, represented by zero counters;
they do not claim completion. Record boundaries and bounded reads retain the
observer's original exception. Seekable metadata skips still receive record
checkpoints; compressed payload inspection continues to decode and validate.

Extraction progress counts each successfully written payload window once, with
the existing 64 KiB write batching. A final checkpoint follows successful close
and precedes guarded publication of each file. Cancellation before publication
preserves the previous target and removes private output. Earlier completed
members can remain; extraction is not an atomic directory replacement.

Filtered readers drain any remaining decoded bytes through bounded checkpoints,
then run their original `finish()` validation. A completed member count does not
substitute for final checksum or termination checks. Readers lacking the owned
window interface cannot silently take an unobservable drain path when an
observer is present. No benchmark or cancellation-latency guarantee follows from
these functional checkpoints; a codec refill or filesystem operation can still
do synchronous work between them.

## Exact-Size Zstandard Workspace

The standalone Zstandard writer passes the locked manifest's exact source size
to the shared stream. Libzstd can then size its context for the actual input
instead of an unknown-length stream. Unknown-size callers, including TAR.ZST,
retain the existing streaming path. No extra input buffering, codec dependency,
checksum removal, or CPU/GPU routing change is involved.

The optional size is an enforced contract: zero means empty, excess writes
are rejected before consumption, short input cannot finalize, and the reserved
unknown-size sentinel is rejected before the destination is opened. The size
remains omitted from the frame, preserving the streaming framing policy and
avoiding extra header bytes. This uses the stable
[libzstd 1.5.7 API](https://github.com/meta/zstd/blob/v1.5.7/lib/zstd.h).

RAM-only comparisons with the pinned runtime, 64 KiB input/output chunks, and
the existing effort parameters measured these codec-owned context allocations:

| Input Bytes | Effort | Unknown-Size Context Bytes | Exact-Size Context Bytes |
| ---: | ---: | ---: | ---: |
| 257 | 9 | 270,008,975 | 189,204 |
| 65,536 | 9 | 270,008,975 | 1,988,336 |
| 262,161 | 9 | 270,008,975 | 10,224,288 |
| 2,097,169 | 9 | 270,008,975 | 70,779,552 |

These are runtime-reported context allocations, not process resident memory
or speed measurements. `workspace_bytes()` exposes that same bounded metric
on the production stream and reports zero after context release. Regression
tests cover all nine efforts, whole/fragmented writes, caller-buffer reuse,
empty/tiny/boundary sizes, size mismatch, adapter wire parity, and a 16 MiB
context ceiling for the tested inputs up to 262,161 bytes.

An independent comparison decoded 144 archives across 72 baseline/candidate
cases: four fixtures, both Bzip2 and Zstandard, and all nine efforts, writing
4,409,822 payload bytes in total. All Bzip2 archives were byte-identical.
Zstandard can choose different parameters when size is known: most compared
sizes were unchanged, but one 65,536-byte repeated-record fixture at effort 7
grew from 16,410 to 16,420 bytes. This deterministic size tradeoff is not host
timing noise. The change materially reduces small-file codec allocation; it
does not guarantee smaller output on every input or establish optimal ratios.

## TAR Size And Filename Contracts

TAR.ZST now supplies the exact serialized TAR size to the shared Zstandard
stream. Counting and writing use the same immutable manifest, header/PAX
serializer, padding rules, and two-block footer. Counting does not read file
payloads or buffer an archive. Regression coverage compares every effort
against standalone exact-size compression of the same TAR, including empty
directories, 511/512/513-byte boundaries, and long UTF-8 PAX paths. This extends
size-aware workspace selection to TAR.ZST; it is not a new measured speedup.

That coverage exposed an existing Windows filename conversion defect. PAX
paths now retain their declared encoding through both extraction passes;
UTF-8 is converted explicitly rather than through the host ANSI code page.
Writers emit PAX path metadata for short non-ASCII names as well as long names.
The shared publication transaction likewise decodes its internally generated
UTF-8 inventory explicitly. Unmarked USTAR/GNU names and PAX `hdrcharset=BINARY`
retain the prior host-code-page interpretation. Unknown local PAX character
sets are rejected rather than guessed. Global PAX overrides remain outside
the existing supported subset. This does not establish Unicode support for
every legacy adapter or change their default encoding.

The encoding boundary follows the
[PAX character-set contract](https://docs.oracle.com/cd/E88353_01/html/E37839/pax-1.html).

## Native, ZIP, And CLI Unicode Boundaries

FLZIP extraction explicitly decodes its existing UTF-8 index paths. ZIP passes
UTF-8 filesystem paths into the bundled miniz Windows stdio implementation,
which already opens wide-character paths. ZIP writers retain the EFS filename
flag. Readers decode EFS names as UTF-8 and unmarked names as CP437 before
archive-wide path validation; decoded metadata remains resource-bounded. This
follows the [ZIP language-encoding contract](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT),
not the current Windows ANSI code page. Optional Unicode path extra-field
overrides and arbitrary vendor-specific legacy encodings remain outside this
increment's verified reader scope.

The CLI now starts with Windows UTF-16 arguments, converts them losslessly to
UTF-8 for option parsing, and explicitly reconstructs native filesystem paths
for create/extract/verify/identify operations. Format detection uses UTF-8
filename keys and locale-independent ASCII extension folding. Its extension
tests cover every registered display row, including the intentional rule that
generic `.bin` files need a valid MacBinary signature. An unused TAR diagnostic
label was removed because its eager code-page conversion could fail otherwise
valid Unicode archive reads.

Native tests cover Unicode source/archive/destination paths, empty ZIP entries,
all nine ZIP efforts, overwrite, CP437, and native CPU/available required-HIP
roundtrips. The CLI format matrix adds Unicode process arguments and entry-name
checks for FLZIP, ZIP, TAR, TAR.GZ, TAR.BZ2 and TAR.ZST. Independent Python ZIP
decoding covers all nine writer efforts; independently produced unmarked ZIPs
exercise all 128 high-byte CP437 values. These are correctness results, not
compression-speed measurements or proof of Unicode support in every adapter.
The reproduced CPIO/CPIO.GZ and AR Unicode defects remain open.

## Extract-Only Unicode Boundaries

The 7z and WIM readers already convert library-provided UTF-16 names to UTF-8.
Their final filesystem joins now explicitly decode that UTF-8 rather than
passing it through the host ANSI code page. WIM's private staged-file lookup
uses typed UTF-8 paths too; single-image output remains unprefixed and
multiple images retain their existing `image-N/` prefixes. XAR's existing XML
UTF-8 names, including decoded character references, use the same explicit
filesystem encoding. This does not add support for other XML encodings or
change any legacy adapter's encoding policy.

Shared file and directory publication, plus these three adapters, now format
native path diagnostics as UTF-8. A valid Unicode output name therefore does
not replace an intended overwrite refusal with a code-page conversion error.
The helper is for diagnostic text only, not path validation or normalization;
invalid native encoding and allocation failures may still throw.

Regression evidence includes:

- A 219-byte LZMA2 fixture generated and independently extracted by
  [py7zr 1.1.3](https://pypi.org/project/py7zr/1.1.3/), embedded as test source
  in `tests/cpp/test_sevenzip_unicode_fixture.hpp`. It contains
  `caf\u00e9/\u65e5\u672c-\U0001f680.txt` with the ASCII payload
  `sevenzip unicode payload\n`, `empty-\u03a9/`, and `empty-\u00e9.txt`.
  The escapes here identify exact Unicode characters, not literal archive
  backslashes. The fixture was generated with `SevenZipFile` in write mode,
  `FILTER_LZMA2` preset 1, and `write(path, relative_posix_name)` for each
  sorted source entry. Python is not required to run the embedded fixture.
- WIM fixtures generated during native tests by the pinned wimlib 1.14.5
  writer API, with ACL capture disabled, no compression, one writer thread,
  and integrity checking. Tests exercise one and two images through the
  product reader; no WIM writer is exposed as a product feature.
- XAR stored and zlib fixtures mixing literal UTF-8, decimal and hexadecimal
  character references, supplementary characters, and the XML ampersand
  entity. Exact restored paths and payloads are checked for both encodings.
- Shared publication tests verify UTF-8 error text, preservation of existing
  files on refusal, explicit replacement, and cleanup of private staging.

The registry-driven CLI format matrix uses these Unicode fixtures for 7z,
WIM, and XAR. It checks automatic and explicit format routing, exact restored
trees, overwrite refusal, explicit overwrite, and registered aliases, in
addition to the independent native test assertions.

These are tiny filesystem correctness fixtures, not performance benchmarks.
The Windows libarchive 3.8.8 7z writer crashed during the initial independent
fixture-generation attempt; py7zr generated the retained fixture instead.
No libarchive-wide compatibility or speed conclusion follows from that crash.

## Native-library Extraction Cancellation

CAB, 7z and WIM report preparation separately from file publication. Preparation
snapshots have unknown totals and zero published-byte and completed-entry counts.
Metadata scans sample cancellation between records. CAB also samples its Windows
FDI validation pass; 7z samples SDK input reads and seeks; WIM uses its pinned
library's progress API during integrity checking and image staging. WIM identifies
native image work as `Decoding WIM image`.

Each native callback catches and retains the original observer exception, returns
the library's error or abort status, and rethrows only after the native call
returns. Observers are synchronous borrows; copying a stateful observer must not
split its state. Per-call SDK bindings are restored before their local progress
owners expire, and WIM progress contexts outlive their native handles.

CAB CRT transfers and copies from the existing 7z decoded cache use 64 KiB
windows. WIM keeps its existing 256 KiB staging-copy windows. Successful payload
writes advance the publication byte counter once. Final close and cancellation
notification precede guarded file publication; entry completion follows it.
Cancellation before publication preserves that target and removes private output.
Earlier completed files can remain after a later interruption.

These are cooperative checkpoints. Individual library decode and operating-system
calls are not forcibly interrupted. Decoder policies, allocation limits, checksum
validation, format capabilities and source ownership remain in force. These
contracts make no measured speed or cancellation-latency claim.

## LHA Preparation And Payload Cancellation

LHA samples the caller's original observer during metadata preparation and
between bounded native payload reads. Preparation has unknown totals and zero
published-byte and completed-entry counters. Validation drains each payload
through the existing 256 KiB buffer size, then retains Lhasa's original CRC and
decoded-length check. The publication pass reuses its output buffer for final
draining and validation, rather than adding a second payload workspace.

Observers run outside native C calls, so their original exceptions propagate
without crossing the library boundary. Successful writes advance payload progress
once. A final checkpoint follows close and precedes guarded publication;
completion follows commit. Interruption preserves an existing target, removes
private output and permits a fresh retry. Individual native calls remain
cooperative; this contract makes no cancellation-latency or speed claim.

## RPM Preparation And Final Entry Notifications

RPM exposes metadata reading, payload preparation and payload decoding as separate
preparation phases. They borrow the original observer, report unknown totals and
zero published counters, and propagate its original exception. Metadata reads
use bounded windows and sample index-record checks. Compressed and decoded
payload staging retain their existing 256 KiB transfer buffers. A cooperative
checkpoint precedes each window, and private payload cleanup covers cancellation
and decoder failure. Every compressed route retains its own `finish()` validation
before the prepared CPIO stream is admitted to extraction.

CPIO and TAR writers publish one final entry-count snapshot after their trailer
or footer is written. This precedes the enclosing archive's close and publication;
it reports completed input work, not durable archive publication. Their seekable
extractors publish the final count after the last member is committed. A callback
exception there propagates while already committed files remain in place.
Existing per-entry notification frequency is preserved.

## Encoded Text Transfer Progress

Base64, BinHex, UUencode and XXEncode share physical-input progress accounting.
It includes encoded payloads, consumed metadata, CR/LF bytes and permitted trailing
whitespace. Snapshots retain encoded-input units rather than treating decoded
output length as input work. Line readers return their exact physical extent
without changing line-length limits, stripped text or EOF/error handling.

The adapters borrow the original synchronous observer and report between bounded
64 KiB input windows. UUencode and XXEncode no longer publish a snapshot for every
short payload line. BinHex counts work through its preamble, decoding and trailing
validation, including discarded resource forks. Individual parsing/decoding calls
remain cooperative; this is not a measured latency guarantee.

Decoder framing, CRC and trailer checks remain required. A final input checkpoint
follows successful output close and precedes guarded publication. An observer
exception preserves an existing target, removes private output and releases the
source; a fresh retry restores the exact expected bytes. Entry completion follows
publication. Final byte counters equal the physical source length for both LF
and CRLF fixtures, including unterminated final lines where the format allows them.

Shared controls use independent known payload encodings and handcrafted BinHex
frames. They cover payload/trailer/pre-publication cancellation, original observer
state and exceptions, cleanup/retry, exact counters and bounded callback counts.
Callback work counts do not establish a measured throughput improvement.

## Remaining Scope

These contracts do not establish optimal ratios or performance for every
format. ZIP uses its separate miniz container adapter, and Unix Compress has
a separate fixed-policy writer. Uncompressed containers and extract-only
formats retain their documented capabilities. CPU efficiency, size policy,
decoder resource use, and wrapper consistency remain part of the broader
all-format review; successful format-matrix checks alone are not performance
evidence. Dedicated security validation remains deferred as requested.
