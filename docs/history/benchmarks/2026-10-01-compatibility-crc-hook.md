# Compatibility CRC Backend Review

This is a development checkpoint, not a release benchmark or universal
performance claim. Measurements used uncommitted source on `main`; public
comparison graphs are unchanged.

## Root Cause And Implementation

The pinned Miniz backend previously computed IEEE CRC one byte at a time.
Native SUZIP and 7z already shared the optimized, pinned LZMA SDK backend.
Miniz now uses its official
[`USE_EXTERNAL_MZCRC` extension](https://github.com/richgel999/miniz/blob/3.1.2/miniz.c)
to call that backend. No upstream source or provenance archive changed.
ZIP creation/extraction and Gzip streams, including TAR.GZ and CPIO.GZ, receive
the change. Deflate search, effort settings, Adler32, metadata, required
integrity checks, and durable publication remain unchanged.

One checksum library owns the existing backend and initialization guard.
Miniz depends on it without a reverse dependency into the archive core.
The C ABI hook uses finalized 32-bit seeds: null requests the documented
initial value; a non-null empty range preserves the low 32 seed bits.
Its shared call cannot throw or allocate. CPU selection remains owned by the
[existing SDK](https://github.com/ip7z/7zip/blob/26.03/C/7zCrc.c), not a
host-specific instruction assumption or CRC32C substitution. Sanitizer builds
compile the same hook and reuse one instrumented checksum object.

## Controls And Method

An ignored resident-input probe compares the original Miniz and shared calls
on identical deterministic input. Five alternating rounds follow a warmup,
hashing 64 MiB per sample at 64 KiB, 1 MiB, and 16 MiB call sizes. All results
match. Median shared-backend throughput was approximately 5.9 times the scalar
throughput. This isolates checksum cost, not application or GPU speed.

The application diagnostic uses normal CLI commands at numeric effort 5,
one warmup, five timed rounds, alternating tool order, and 500 ms pauses outside
timing. Every filesystem case is below 64 MiB, using the pinned Silesia subset
from the [comparison protocol](../../comparative-benchmark-methodology.md).
Every created archive is independently extracted and hashed. Extraction timing
uses the same official 7-Zip-produced reference archive for all binaries.
Every new/control creation sample has identical archive bytes and SHA-256,
not merely equal rounded ratios. The interrupted pre-pause run is not counted.

The platform was Ryzen 9 9950X (16 cores/32 threads), Radeon RX 9070 XT,
Windows 11 Pro build 26300, 68,305,387,520 bytes of RAM at 5200 MT/s, and
Lexar NM790 NVMe storage. These format operations use CPU, not HIP.
Coarse CPU/GPU/RAM/storage samples before, after, and between rounds are
retained; they do not prove uninterrupted headroom during each command.

| Build | CLI SHA-256 |
| --- | --- |
| Clean `c21b435` control | `b9a67ef6e3bc235c1a8d0445536fb4fbdd681841096de7edf836dd66a766c648` |
| Shared Miniz CRC candidate | `bcbf8a4b995e62463b3ab9be48f7f4bb9c6e70c1bbed328c28b253ea879fcdb8` |
| Official 7-Zip 26.03 | `edbee35370e14030e4c785cf88200f42dc651c1eb4217c1e3963c38a12f099b0` |

Evidence: `out/crc-probe/control-c21b435.jsonl` and
`out/benchmarks/miniz-crc-paired-resumed-20261001.json`.

## Application Diagnostics And Limits

These are median whole-command seconds across five rounds, including startup,
codec work, filesystem I/O, checks, and publication. Numeric effort 5 is
explicit, not a claim of equal internal policy across tools.

| Case | Input Bytes | New/Control Archive Bytes | Create Control / New | Extract Control / New | 7-Zip Create / Extract | 7-Zip Archive Bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ZIP mixed files | 51,770,558 | 19,263,206 | 0.972697 / 0.876934 | 0.346916 / 0.284258 | 1.012983 / 0.176731 | 18,318,370 |
| Gzip mozilla | 51,220,480 | 19,298,754 | 0.980638 / 0.915521 | 0.297342 / 0.248626 | 2.435483 / 0.161638 | 18,560,411 |
| Gzip nci | 33,553,445 | 3,230,361 | 0.276022 / 0.240822 | 0.170206 / 0.138801 | 1.842189 / 0.055997 | 3,038,883 |

Every paired creation sample favors the candidate. Extraction is noisier:
paired ratios range from 1.001 to 1.276 for ZIP, 0.600 to 1.313 for Gzip
mozilla, and 1.036 to 1.366 for Gzip nci. Mozilla's last two rounds contain
large delays in both variants, with the candidate slower. Those samples remain
in the medians and records. ZIP snapshots show substantial storage activity
and page-in counts. Windows disk-time counters can exceed 100, and page-in
counters include file-backed activity; neither alone proves SSD saturation
or memory-pressure paging.

These medians are diagnostic, not publication-ready speed claims. Slow
intervals need profiling and a calmer confirmation, not removed durability or
favorable-sample selection. The tested CLI does not request output quarantine:
source inspection confirms one adapter call and one per-file publication.
No flush or validation was removed. 7-Zip still extracts these cases faster
and produces smaller archives; the CRC change does not resolve every codec gap.

## Correctness And Remaining Gates

The unchanged checksum snapshot passes all 22 full local gates, including
533 HIP-enabled tests, independent interoperability, the 36-format matrix,
all-page GUI smoke and screenshot review, 14 sanitizer fuzz targets, and
packaging. An isolated CPU-only build passes 532 tests. A fresh-process
first-use concurrency test also passed. Independent bitwise-oracle tests cover
four seeds, sixteen alignments, word/tail boundaries, every split of a streaming
fixture, null initialization, and platform-width seed truncation.

The 10 GiB Mixed RAM sweep at effort 5 passes all seven block sizes in both
CPU/GPU modes. All 14 archive and payload sizes match the previous `c21b435`
sweep; all runs report RAM-only operation and zero archive disk writes.
GPU runs record actual HIP launches and finite positive event time.
This is a wire/resource/backend regression check, not a native speed claim:
it is one sweep, partly overlapping a development index refresh. Its timings
are not promoted as performance evidence.

New-commit hosted validation, remaining code-scanning triage, the Semgrep/PyJWT
dependency blocker, broader CPU/GPU improvements, clean-source graphs, final
documentation, and release acceptance remain open. No release was created.
