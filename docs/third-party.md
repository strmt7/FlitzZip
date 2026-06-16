# Third-Party Notices

## miniz 3.1.1

SuperZip vendors miniz release `3.1.1` for standards-oriented ZIP compatibility.

- Upstream: <https://github.com/richgel999/miniz>
- Tag: `3.1.1`
- Commit: `d10b03cc73475af673df40f06e5cefd1d5f940d9`
- License: MIT, preserved at `third_party/miniz/LICENSE`

SuperZip uses miniz for bounded CPU-codec purposes: standards-oriented `.zip`
compatibility, Gzip/TAR.GZ streams, XAR zlib TOC/payload inflation for the
read-only safe subset, and per-block deflate payloads inside CPU-authored
native `.suzip` archives. The `.suzip` required-GPU boundary remains AMD
HIP-only; miniz does not provide GPU acceleration, does not finish required-HIP
decode work, and is not an alternate archive pipeline.

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

## Zstandard 1.5.7

SuperZip bundles the official Zstandard/libzstd release `v1.5.7` Win64 runtime
for standards-oriented Zstandard compatibility.

- Upstream: <https://github.com/facebook/zstd>
- Release tag: `v1.5.7`
- Source archive SHA-256: `7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4`
- Win64 runtime package SHA-256: `acb4e8111511749dc7a3ebedca9b04190e37a17afeb73f55d4425dbf0b90fad9`
- Extracted DLL SHA-256: `8f07e1112ed283e5cd2798833e9a3c32d8961381bc36da04af57a1b0ca9bd40b`
- License: BSD, preserved at `third_party/zstd/LICENSE`

SuperZip uses libzstd for two bounded CPU-codec purposes: single-file
`.zst`/`.zstd` streams and `.tar.zst`/`.tzst` stream filters over the native
TAR adapter. It does not provide GPU acceleration and is not an alternate
SUZIP codec path.

The production copy under `third_party/zstd/` contains only license files and
SuperZip runtime notes. CMake verifies the official Win64 runtime package,
extracts `libzstd.dll` into the build directory, and copies it beside each
SuperZip executable. SuperZip loads that DLL only from the executable directory,
validates runtime version `10507` (`1.5.7`), and never shells out to `zstd.exe`.
The original source archive, official Win64 package, and checksums are stored
under `third_party/upstream/zstd/v1.5.7/` for provenance.

## LZMA SDK 26.01

SuperZip vendors the minimal ANSI-C 7z decoder subset from the official LZMA
SDK release `26.01` for extract-only 7z compatibility.

- Upstream: <https://www.7-zip.org/sdk.html>
- Release archive: <https://www.7-zip.org/a/lzma2601.7z>
- SHA-256: `b860f17f9df3c0524dd2ef2c639ab5e43ad0006b77b8f7bb6d191bf528536885`
- License: public domain, as stated in `DOC/lzma-sdk.txt` inside the upstream
  archive

SuperZip uses this SDK only for read-only `.7z` extraction. It does not ship or
execute the SDK sample tools, does not call `7z.exe`, and does not use the SDK
as a SUZIP codec or GPU path. The patched production copy is under
`third_party/lzma_sdk/`, while the unmodified upstream archive and checksum are
stored under `third_party/upstream/lzma-sdk/26.01/`.

## Lhasa 0.5.0

SuperZip vendors Lhasa release `0.5.0` for extract-only LHA/LZH compatibility.

- Upstream: <https://github.com/fragglet/lhasa>
- Release tag: `v0.5.0`
- Release commit: `450172de282c8f8730696f4370a57cf49bfabf22`
- Source archive SHA-256: `1ae8d82d37fc12ec2c52c520b6528ec61268e243f33eca4446b440e182c66d91`
- License: ISC, preserved at `third_party/lhasa/COPYING.md`

SuperZip uses Lhasa only as an in-process LHA/LZH decoder and metadata reader.
It does not call Lhasa's extraction helper and does not shell out to `lha` or
other host tools. The SuperZip adapter validates every decoded entry path,
rejects symlinks, checks payload CRC/size before destination writes, and
publishes files through the standard verified temporary-file path.

The production copy under `third_party/lhasa/` carries narrow local hardening
patches documented in `third_party/lhasa/README.SUPERZIP.md`. The unmodified
upstream source archive and checksum are stored under
`third_party/upstream/lhasa/0.5.0/` for provenance.

## wimlib 1.14.5

SuperZip bundles the official wimlib release `1.14.5` Windows x64 runtime for
extract-only standalone WIM compatibility.

- Upstream: <https://wimlib.net/>
- Release package: <https://wimlib.net/downloads/wimlib-1.14.5-windows-x86_64-bin.zip>
- Win64 runtime package SHA-256: `2f446d6fa3866582175f1a22a7be198eeee0aec7aba5b4e04ad25c99eae2d265`
- Extracted DLL SHA-256: `ba853ee1e3fc5f5798581f02e8e066ba07a0a2375f0bf444fe981431fd508495`
- Active header SHA-256: `76ff273fb0c89fd2fd084f0467b1146afb3228db9372d4193432f13c2871f67e`
- License: LGPLv3 for the library, preserved at
  `third_party/wimlib/COPYING.LGPL.txt`; bundled libdivsufsort-lite notice
  preserved at `third_party/wimlib/COPYING.libdivsufsort-lite.txt`

SuperZip uses wimlib only through an app-local dynamically loaded
`libwim-15.dll` and does not call `wimlib-imagex.exe`, DISM, PowerShell, shell
handlers, or host-installed WIM utilities. CMake verifies the official runtime
package, extracts only the runtime DLL into the build tree, verifies the DLL
checksum, and copies the DLL beside each SuperZip executable. The adapter opens
WIM files read-only, rejects split WIM sets, validates all paths and unsupported
entry kinds before staging, extracts into a private temporary directory, then
publishes approved regular files through SuperZip's verified temporary-file
path.

The active header under `third_party/wimlib/wimlib.h` carries a local
scanner-neutral comment/member-name normalization for declarations SuperZip
does not use as security primitives. The original upstream header remains
unchanged inside the pinned upstream package recorded above.

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
