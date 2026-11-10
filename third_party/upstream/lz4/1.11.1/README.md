# LZ4 1.11.1 Source Provenance

This is the original official GitHub tag archive for LZ4 1.11.1:

<https://github.com/lz4/lz4/archive/refs/tags/v1.11.1.tar.gz>

The original archive is retained without repacking. `SHA256SUMS` is its complete
byte identity. GitHub may regenerate remote compressed archives or move tags;
neither permits replacing these retained bytes or changing their checksum.

Only `lib/lz4.c`, `lib/lz4.h` and `lib/LICENSE` are imported into production.
All three were independently matched to the publisher's tagged Git tree before
adoption. The raw-block library remains BSD-2-Clause; its license text is unchanged.
The frame library, high-compression library and command-line application are not
compiled into FlitzZip.

Run `py -3 tools/vendor_imports.py --require-canonical` to verify the archive,
checksum and complete imported source set. This does not replace scanners,
behavior tests or release qualification.
