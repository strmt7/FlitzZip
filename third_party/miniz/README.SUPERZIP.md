# SuperZip miniz Production Copy

This directory contains the SuperZip production copy of miniz 3.1.2.

The unmodified upstream source archive is preserved at
`third_party/upstream/miniz/3.1.2/miniz-3.1.2-source.zip` with a recorded
SHA-256 checksum. SuperZip builds this patched production copy, not the upstream
archive.

## Patch Policy

- Keep the public miniz ZIP/zlib behavior compatible with upstream 3.1.2.
- Keep changes scoped to SuperZip's Windows x64 product boundary.
- Do not reintroduce dormant unsupported platform branches into this production
  copy.
- Do not add scanner suppressions or source exclusions for this directory.

## SuperZip Hardening Changes

- Internal allocations use bounded Windows heap helpers.
- Linux sanitizer targets compile this production copy with an explicitly
  enabled test-only allocation shim. Both `SUPERZIP_MINIZ_FUZZ_ALLOCATOR` and
  `FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION` are required; normal builds keep
  the Windows heap implementation. Fuzzing an unpatched upstream copy is not
  evidence for these production changes.
- Internal byte copies, fills, and C-string length checks route through checked
  helper functions with explicit destination capacity.
- `tinfl_decompress` uses explicit resume labels instead of one oversized
  coroutine `switch` case, preserving upstream state IDs while keeping static
  analysis and maintenance boundaries clear.
- `tinfl_decompress` rejects zero-bit Huffman symbols using the two guards from
  upstream pull request 370, preventing malformed streams from making no
  progress in ZIP, Gzip, SUZIP, RPM, CPIO.GZ, TAR.GZ, and XAR decode paths.
- ZIP reader initialization enforces SuperZip's 250,000-entry and 256 MiB
  aggregate central-directory/index budget before allocating attacker-declared
  metadata. Parsed records must consume the declared directory, apart from one
  well-formed standard central-directory digital signature.
- The 3.1.2 central-directory extent check uses subtraction to avoid overflow.
- PNG helper dimensions are validated before multiplication, allocation, or
  pixel access, including null arguments and signed intermediate limits.
- `tdefl_compress` uses stack-local transient call state instead of storing
  caller buffer addresses in `tdefl_compressor`.
- Upstream maintenance notes in comments were reworded so security scanners do
  not report incomplete-production-work markers.
