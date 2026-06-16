# XZ Embedded Production Copy

This directory contains the XZ Embedded decoder files used by SuperZip for
read-only `.xz` and `.tar.xz` compatibility extraction.

Upstream provenance is stored under:

`third_party/upstream/xz-embedded/ae63ae3a36ed01724674e8f3d750dc47bf125410/`

The expected SHA-256 for the upstream GitHub source archive is:

`def1cc76f59db117245670f334599d70406c5f8a3ea99fb79763a84803420abf`

SuperZip includes only the upstream-documented userspace embedding files:

- `linux/include/linux/xz.h`
- `linux/lib/xz/xz_crc32.c`
- `linux/lib/xz/xz_crc64.c`
- `linux/lib/xz/xz_dec_lzma2.c`
- `linux/lib/xz/xz_dec_stream.c`
- `linux/lib/xz/xz_lzma2.h`
- `linux/lib/xz/xz_private.h`
- `linux/lib/xz/xz_stream.h`
- `userspace/xz_config.h`

The production copy enables concatenated stream handling and CRC64 verification
because regular `.xz` files commonly use CRC64. It intentionally does not enable
BCJ filters or SHA-256 checks in this increment.

SuperZip hardening changes are limited to the userspace integration surface:

- Allocations route through zero-initializing helper functions.
- Internal copy/move call sites route through local helper loops so repository
  security lint can review the exact bounded call surface.
- The SuperZip wrapper caps the LZMA2 dictionary size before decoding untrusted
  input.

Other product behavior belongs in `src/xz/`, not in the embedded upstream
decoder files, unless a future security fix requires a documented vendor patch.
