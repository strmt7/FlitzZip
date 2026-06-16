# bzip2 1.0.8

This directory contains the library source files from the official bzip2 1.0.8
release. SuperZip builds them as an in-process static library for `.bz2` and
`.tar.bz2` compatibility support.

Upstream archive provenance is stored under:

`third_party/upstream/bzip2/1.0.8/`

The expected SHA-256 for `bzip2-1.0.8.tar.gz` is:

`ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269`

Keep this production copy close to upstream. The checked-in text files are
line-ending and trailing-whitespace normalized for repository hygiene; the
unmodified upstream tarball above remains the byte-for-byte provenance source.

SuperZip carries a small production hardening delta in this directory:

- `bz_internal_error.c` provides the embedding hook declared by libbzip2.
- The production copy always uses the no-stdio assertion/logging path used by
  SuperZip's `BZ_NO_STDIO` build.
- Allocator wrappers check multiplication overflow and avoid needless guarded
  frees.
- Streaming state keeps an internal `bz_stream` snapshot and exports caller
  fields at API boundaries, instead of storing caller stack addresses in heap
  state.
- The resumable decompressor uses a compact dispatcher with labels rather than
  a giant switch body.

Other SuperZip-specific behavior belongs in `src/bzip2/`, not in patched bzip2
internals, unless a security fix requires a documented vendor patch.
