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

Gzip's shared writer consumes borrowed immutable input synchronously. A local
RAII guard clears its input pointer/count on every exit, including exceptions.
This removes an input staging copy and a 64 KiB buffer per writer while
preserving the existing input-chunk limits. It does not make Deflate itself
zero-copy or establish an end-to-end speedup. The maintainer has re-enabled
timing runs after the busy-host deferral; comparisons must monitor CPU, GPU,
RAM, and storage contention before and throughout measurements.

Independent CLI checks decoded a 262,161-byte mixed binary fixture at all nine
efforts using Python 3.12.14's Gzip, Bzip2, and ZIP readers and the separately
installed `zstandard` 0.25.0 reader. All 36 outputs restored the original bytes.
These bounded filesystem checks establish interoperability, not throughput or
universally distinct sizes: several efforts produced equal sizes on this
fixture, and Bzip2/Zstandard sizes were not uniformly monotonic.

## Remaining Scope

These contracts do not establish optimal ratios or performance for every
format. ZIP uses its separate miniz container adapter, and Unix Compress has
a separate fixed-policy writer. Uncompressed containers and extract-only
formats retain their documented capabilities. CPU efficiency, size policy,
decoder resource use, and wrapper consistency remain part of the broader
all-format review; successful format-matrix checks alone are not performance
evidence. Dedicated security validation remains deferred as requested.
