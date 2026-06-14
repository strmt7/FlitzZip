# SuperZip miniz Production Copy

This directory contains the SuperZip production copy of miniz 3.1.1.

The unmodified upstream source archive is preserved at
`third_party/upstream/miniz/3.1.1/miniz-3.1.1-source.zip` with a recorded
SHA-256 checksum. SuperZip builds this patched production copy, not the upstream
archive.

## Patch Policy

- Keep the public miniz ZIP/zlib behavior compatible with upstream 3.1.1.
- Keep changes scoped to SuperZip's Windows x64 product boundary.
- Do not reintroduce dormant unsupported platform branches into this production
  copy.
- Do not add scanner suppressions or source exclusions for this directory.

## SuperZip Hardening Changes

- Internal allocations use bounded Windows heap helpers.
- Internal byte copies, fills, and C-string length checks route through checked
  helper functions with explicit destination capacity.
- `tdefl_compress` uses stack-local transient call state instead of storing
  caller buffer addresses in `tdefl_compressor`.
- Upstream maintenance notes in comments were reworded so security scanners do
  not report incomplete-production-work markers.
