# Third-Party Notices

## miniz 3.1.2

SuperZip vendors miniz release `3.1.2` for standards-oriented ZIP compatibility.

- Upstream: <https://github.com/richgel999/miniz>
- Tag: `3.1.2`
- Commit: `77d0dce8627735138c51770d1799a1ef48f2117d`
- License: MIT, preserved at `third_party/miniz/LICENSE`

SuperZip uses miniz for bounded CPU-codec purposes: standards-oriented `.zip`
compatibility, Gzip/TAR.GZ streams, XAR zlib TOC/payload inflation for the
read-only safe subset, and per-block deflate payloads inside CPU-authored
native `.suzip` archives. The `.suzip` required-GPU boundary remains AMD
HIP-only; miniz does not provide GPU acceleration, does not finish required-HIP
decode work, and is not an alternate archive pipeline.

The production copy under `third_party/miniz/` carries local hardening patches
and measured Huffman block-cost selection, documented in
`third_party/miniz/README.SUPERZIP.md`. The unmodified upstream 3.1.2 source
archive and checksum are stored
under `third_party/upstream/miniz/3.1.2/` for provenance. Do not edit the
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

## LZ4 1.10.0

SuperZip vendors the LZ4 library for decoding LZ4 native archive blocks.

- Upstream: <https://github.com/lz4/lz4>
- Release: `v1.10.0`
- Source archive SHA-256: `537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b`
- Library license: BSD-2-Clause, preserved at `third_party/lz4/LICENSE`.

The library files under `third_party/lz4/` are compiled in process. This
does not provide GPU acceleration or authorize use of unrelated LZ4
command-line source under a different license. The provenance archive and
checksum remain under `third_party/upstream/lz4/1.10.0/`.

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

SuperZip builds an app-local Zstandard/libzstd `v1.5.7` runtime from the pinned
upstream source archive, with multithreading and legacy decoding enabled.

- Upstream: <https://github.com/facebook/zstd>
- Release tag: `v1.5.7`
- Source archive SHA-256: `7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4`
- Win64 runtime package SHA-256: `acb4e8111511749dc7a3ebedca9b04190e37a17afeb73f55d4425dbf0b90fad9`
- Built DLL SHA-256: calculated at build time and embedded in the executable;
  a different runtime is rejected before loading.
- License: BSD, preserved at `third_party/zstd/LICENSE`

SuperZip uses libzstd for bounded CPU-codec work: single-file
`.zst`/`.zstd` streams and `.tar.zst`/`.tzst` stream filters over the native
TAR adapter, and CPU Zstandard block candidates in native SUZIP archives.
It does not provide GPU acceleration.

The production copy under `third_party/zstd/` contains only license files and
SuperZip runtime notes. CMake verifies the source archive, builds `libzstd.dll`
in the build directory with reproducible legacy-context and custom-allocation repairs
documented in `third_party/zstd/README.SUPERZIP.md`, and copies it beside each
executable. The original archives and license notices remain unchanged. The official
Win64 package remains pinned for independent CLI comparisons. SuperZip loads
its built DLL only from the executable directory,
validates runtime version `10507` (`1.5.7`), and never shells out to `zstd.exe`.
The original source archive, official Win64 package, and checksums are stored
under `third_party/upstream/zstd/v1.5.7/` for provenance.

## LZMA SDK 26.03

SuperZip vendors the minimal ANSI-C decoder subset from the official LZMA SDK
release `26.03` for extract-only 7z, LZMA-Alone, and lzip compatibility.

- Upstream: <https://www.7-zip.org/sdk.html>
- Release archive: <https://github.com/ip7z/7zip/releases/download/26.03/lzma2603.7z>
- SHA-256: `86c213f752520ab5325c310f50bef63ec344b56dd1c80b0246d06dc6cec953b2`
- License: public domain, as stated in `DOC/lzma-sdk.txt` inside the upstream
  archive

SuperZip uses this SDK for read-only `.7z`, `.lzma`, and `.lz` extraction and
the `.tar.lz` stream adapter. It does not ship or execute SDK sample tools or
call `7z.exe`, and does not use the SDK as a SUZIP codec or GPU path. The
patched production copy is under `third_party/lzma_sdk/`, while the unmodified
upstream archive and checksum are stored under
`third_party/upstream/lzma-sdk/26.03/`. The older 26.01 provenance is retained.

## Lhasa 0.6.0

SuperZip vendors Lhasa release `0.6.0` for extract-only LHA/LZH compatibility.

- Upstream: <https://github.com/fragglet/lhasa>
- Release tag: `v0.6.0`
- Release commit: `75ed83559f23e9538e0045c62f53f77ab03d03d6`
- Source archive SHA-256: `9840154367f73e9d9c3196f944a121ab4d398d84e921c8fe8fca8a931274aed7`
- License: ISC, preserved at `third_party/lhasa/COPYING.md`

SuperZip uses Lhasa only as an in-process LHA/LZH decoder and metadata reader.
It does not call Lhasa's extraction helper and does not shell out to `lha` or
other host tools. The SuperZip adapter validates every decoded entry path,
rejects symlinks, checks payload CRC/size before destination writes, and
publishes files through the standard verified temporary-file path.

The production copy under `third_party/lhasa/` carries narrow local hardening
patches documented in `third_party/lhasa/README.SUPERZIP.md`. The unmodified
upstream source archive and checksum are stored under
`third_party/upstream/lhasa/0.6.0/` for provenance.

## wimlib 1.14.5

SuperZip bundles the official wimlib release `1.14.5` Windows x64 runtime for
extract-only standalone WIM compatibility.

- Upstream: <https://wimlib.net/>
- Release package: <https://wimlib.net/downloads/wimlib-1.14.5-windows-x86_64-bin.zip>
- Complete upstream source: <https://wimlib.net/downloads/wimlib-1.14.5.tar.gz>
- Source archive SHA-256: `84221a3abd5b91228f15f8e6065c335a336237b5738197b75bf419eea561a194`
- Win64 runtime package SHA-256: `2f446d6fa3866582175f1a22a7be198eeee0aec7aba5b4e04ad25c99eae2d265`
- Extracted DLL SHA-256: `ba853ee1e3fc5f5798581f02e8e066ba07a0a2375f0bf444fe981431fd508495`
- Active header SHA-256: `76ff273fb0c89fd2fd084f0467b1146afb3228db9372d4193432f13c2871f67e`
- License: LGPL-2.1-or-later for the Windows library, selected from the
  dual-license terms preserved at `third_party/wimlib/COPYING.txt` and
  `third_party/wimlib/COPYING.LGPL.txt`; the alternative GPL-3.0-or-later
  terms are preserved at `third_party/wimlib/COPYING.GPLv3.txt`. The Windows
  build does not link libntfs-3g, which would prohibit the LGPL option.
  The bundled libdivsufsort-lite notice is preserved at
  `third_party/wimlib/COPYING.libdivsufsort-lite.txt`.

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

The unmodified complete source tarball is preserved beside the binary
provenance package. It contains the library sources, notices, configuration,
Makefiles, `README.WINDOWS.md` and `tools/windows-build.sh`. The upstream
Windows source build uses MinGW-w64 through MSYS2; it is not an MSVC build.
This does not change SuperZip's Windows-native MSVC/HIP product build and does
not authorize WSL or installation of another toolchain during ordinary work.

For distribution, use the source/rebuild route in LGPL-2.1 sections 6(a)/6(d),
not a claim that the shipped executable permits every interface-compatible
DLL to be substituted directly. Its runtime checksum pin deliberately rejects
modified DLL bytes. The release's exact SuperZip source must be available
alongside the binary release, including this complete wimlib source tarball
and application build scripts. A recipient rebuilding with a modified library
must update that build's wimlib package and DLL hash pins in `CMakeLists.txt`
and rebuild the application, preserving app-local loading and the library's
ABI. Do not change the canonical upstream archives or disable integrity
verification in distributed product builds. Final release validation must
prove that the offered source snapshot contains these materials and document
the rebuild check; adding this archive alone is not proof of that gate.

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

## Adapted Development Tools

`tools/cocoindex_agent_search.py` is adapted from the MIT-licensed
`scripts/cocoindex_agent_search.py` in
[strmt7/VulnerabilityScreener](https://github.com/strmt7/VulnerabilityScreener).
Its original copyright and full permission notice are preserved in
`third_party/notices/VulnerabilityScreener-MIT.txt`. It is development tooling,
not a shipped runtime dependency or a product scanner.

Benchmark-tool execution and result-publication decisions are recorded
separately in [Benchmark Permissions And Tool Selection](benchmark-permissions.md).
They do not grant permission to redistribute comparator binaries with SuperZip.
