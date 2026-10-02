# SuperZip Zstandard Runtime Notes

SuperZip uses the official `facebook/zstd` v1.5.7 release for native `.zst`
and `.tar.zst` compatibility.

- Upstream: <https://github.com/facebook/zstd>
- Release tag: `v1.5.7`
- Original source archive: `third_party/upstream/zstd/v1.5.7/zstd-v1.5.7.zip`
- Official Win64 runtime package: `third_party/upstream/zstd/v1.5.7/zstd-v1.5.7-win64.zip`
- Build output DLL: built from pinned upstream C sources with the documented SuperZip legacy-context hardening patch in the CMake binary directory, then copied beside each executable. The target preserves upstream MSVC definitions, multithreading and legacy decoder level 5 without configuring the upstream CLI, tests, or GNU assembler project.
- Source SHA-256: `7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4`
- Win64 package SHA-256: `acb4e8111511749dc7a3ebedca9b04190e37a17afeb73f55d4425dbf0b90fad9`
- DLL identity: its build-specific SHA-256 is embedded in the executable through `cmake/WriteRuntimeIdentity.cmake`. Runtime loading rejects a DLL that differs from that build.
- License: BSD, preserved in `LICENSE`

Production policy:

- Keep original upstream archives untouched under `third_party/upstream`.
- Build upstream implementation source only in the binary directory. Do not
  track or modify extracted sources or runtime binaries in the source tree.
- Apply downstream changes through `cmake/PatchZstdLegacy.cmake`, never by
  editing the provenance archive or relying on an untracked build-tree fix.
  The patch requires exact original or patched file hashes and checks the
  complete output before atomic publication. Unknown source and interrupted
  patch files are rejected; repeated configuration does not rewrite valid files.
- Load `libzstd.dll` only from the executable directory; never from the current
  working directory.
- Validate the runtime version number at startup and fail closed unless it is
  `10507` (`1.5.7`).
- Keep Zstandard compression in-process through the DLL API; never shell out to
  `zstd.exe` or depend on a host-installed archive tool.
- Enable frame checksums for archives SuperZip creates.
- Cap decompression window size at the wrapper boundary to avoid untrusted
  streams forcing unbounded memory growth.

## Legacy Context Hardening

The downstream patch makes the v0.5 buffered constructor release a partial
owner when its inner allocation fails. Context transitions between the shipped
v0.5-v0.7 decoders initialize a candidate before releasing the previous owner
and propagate dictionary initialization errors. Failure cannot publish an owner
whose version disagrees with the caller's retained version metadata.

`superzip_zstd_legacy_tests` compiles these same production sources with a serial
allocation interposer. It checks both constructor failures, first initialization,
all six cross-version transitions, same-version reuse, malformed dictionary
errors, retry, leaks and invalid releases. The separate CMake regression verifies
fresh archive extraction, idempotence, source-drift rejection, incomplete-patch
preservation and immutable archive provenance. Neither target is a product
decoder fork or a compression-performance claim.

Upstream copyright and BSD license text remain unchanged. This is a downstream
patch, not a claim that the upstream release already contains these fixes.

## Custom Allocation Failure

The same reproducible recipe guards `ZSTD_customCalloc` before it clears custom
storage. A callback returning NULL now propagates allocation failure rather than
passing NULL to `memset`. This matches the failure handling in
[upstream commit 3f8f9b3f](https://github.com/facebook/zstd/commit/3f8f9b3f89244638f10bca664c120fd28cb14efe),
checked on 2026-10-02; it does not replace the pinned release with development
source. The original header and complete derived output have verified hashes.

The allocation fixture invokes the actual production helper. It covers injected
failure, zero-size failure, an existing live owner, retry, nonzero-to-zero custom
initialization, the unchanged standard allocator, and matched release. CMake
fixtures additionally cover the helper's drift, idempotence and interrupted-write
boundaries. SuperZip currently uses the standard allocator; attacker-triggerable
reachability through a custom allocator is not established by these tests.

## Private Header Inclusion

The extracted `dictBuilder/cover.h` and `compress/hist.h` now receive ordinary
include guards through the same identity-checked recipe. Declarations, macros,
copyrights and license notices are unchanged. This is a compile-time quality
repair; it is not evidence of an attacker-triggerable runtime vulnerability or
a compression speedup.

`superzip_zstd_header_contracts` extracts an isolated, verified source fixture.
It compiles the same contract as C and C++, first proving the original COVER
redefinition on repeated inclusion, then accepting the repaired private headers.
It also verifies public-then-static Zstandard, dictionary and FSE inclusion,
public-then-inline xxHash inclusion, and repeated allocation-header inclusion.
These deliberately staged headers remain unchanged. Patch fixtures cover all
five transformed files for idempotence, drift and interrupted-write preservation.
