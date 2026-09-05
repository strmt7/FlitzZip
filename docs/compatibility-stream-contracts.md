# Compatibility Compression Stream Contracts

## Shared CPU Writers

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
themselves zero-copy or establish an end-to-end speedup. The maintainer has re-enabled
timing runs after the busy-host deferral; comparisons must monitor CPU, GPU,
RAM, and storage contention before and throughout measurements.

Independent CLI checks decoded a 262,161-byte mixed binary fixture at all nine
efforts using Python 3.12.14's Gzip, Bzip2, and ZIP readers and the separately
installed `zstandard` 0.25.0 reader. All 36 outputs restored the original bytes.
These bounded filesystem checks establish interoperability, not throughput or
universally distinct sizes: several efforts produced equal sizes on this
fixture, and Bzip2/Zstandard sizes were not uniformly monotonic.

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
[libzstd 1.5.7 API](https://github.com/facebook/zstd/blob/v1.5.7/lib/zstd.h).

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

SUZIP extraction explicitly decodes its existing UTF-8 index paths. ZIP passes
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
checks for SUZIP, ZIP, TAR, TAR.GZ, TAR.BZ2 and TAR.ZST. Independent Python ZIP
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

## Remaining Scope

These contracts do not establish optimal ratios or performance for every
format. ZIP uses its separate miniz container adapter, and Unix Compress has
a separate fixed-policy writer. Uncompressed containers and extract-only
formats retain their documented capabilities. CPU efficiency, size policy,
decoder resource use, and wrapper consistency remain part of the broader
all-format review; successful format-matrix checks alone are not performance
evidence. Dedicated security validation remains deferred as requested.
