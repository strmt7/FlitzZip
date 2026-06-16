# Third-Party Notices

## miniz 3.1.1

SuperZip vendors miniz release `3.1.1` for standards-oriented ZIP compatibility.

- Upstream: <https://github.com/richgel999/miniz>
- Tag: `3.1.1`
- Commit: `d10b03cc73475af673df40f06e5cefd1d5f940d9`
- License: MIT, preserved at `third_party/miniz/LICENSE`

SuperZip uses miniz for two bounded CPU-codec purposes: standards-oriented
`.zip` compatibility and per-block deflate payloads inside CPU-authored native
`.suzip` archives. The `.suzip` required-GPU boundary remains AMD HIP-only;
miniz does not provide GPU acceleration, does not finish required-HIP decode
work, and is not an alternate archive pipeline.

The production copy under `third_party/miniz/` carries narrow local hardening
patches. The unmodified upstream 3.1.1 source archive and checksum are stored
under `third_party/upstream/miniz/3.1.1/` for provenance. Do not edit the
upstream archive; production fixes belong in `third_party/miniz/` and must be
covered by tests and security scanning.

## bzip2 1.0.8

SuperZip vendors bzip2/libbzip2 release `1.0.8` for standards-oriented Bzip2
compatibility.

- Upstream: <https://sourceware.org/bzip2/>
- Release archive: <https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz>
- SHA-256: `ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269`
- License: bzip2 license, preserved at `third_party/bzip2/LICENSE`

SuperZip uses libbzip2 for two bounded CPU-codec purposes: single-file `.bz2`
streams and `.tar.bz2`/`.tbz`/`.tbz2` stream filters over the native TAR
adapter. It does not provide GPU acceleration and is not an alternate SUZIP
codec path.

The production copy under `third_party/bzip2/` is intended to stay close to
upstream. The only SuperZip-owned file in that directory is
`bz_internal_error.c`, a required link shim for libbzip2's embedding hook. The
unmodified upstream 1.0.8 source archive and checksum are stored under
`third_party/upstream/bzip2/1.0.8/` for provenance.

## XZ Embedded

SuperZip vendors the XZ Embedded decoder at upstream commit
`ae63ae3a36ed01724674e8f3d750dc47bf125410` for extract-only XZ compatibility.

- Upstream: <https://github.com/tukaani-project/xz-embedded>
- Commit date: 2024-12-30
- Source archive SHA-256: `def1cc76f59db117245670f334599d70406c5f8a3ea99fb79763a84803420abf`
- License: 0BSD, preserved at `third_party/xz_embedded/COPYING`

SuperZip uses XZ Embedded only for single-file `.xz` extraction and
`.tar.xz`/`.txz` stream filters over the native TAR adapter. It is decode-only,
does not provide GPU acceleration, and is not an alternate SUZIP codec path.

The production copy under `third_party/xz_embedded/` contains only the
upstream-documented userspace embedding files plus narrow SuperZip integration
hardening. The upstream source archive and checksum are stored under
`third_party/upstream/xz-embedded/ae63ae3a36ed01724674e8f3d750dc47bf125410/`
for provenance.

## AMD HIP SDK

SuperZip is built against AMD HIP SDK for Windows. AMD's Windows HIP deployment
guidance says the HIP runtime is supplied by the AMD GPU driver, while ISV
packages may distribute additional dynamically linked HIP SDK libraries they
use. SuperZip currently links only against the HIP runtime import library and
does not redistribute the HIP runtime DLL.

The release manifest records the runtime DLL name observed at build time so the
installer, portable package, and support diagnostics can report dependency state
clearly.

## GPU Compression Libraries

Alternative GPU-compute and compression backends are evaluated in
`docs/compression-backend-evaluation.md`. The production boundary remains AMD
HIP. hipCOMP-core is tracked as a research candidate only while its upstream
README labels it early-access technology preview and not recommended for
production workloads.
