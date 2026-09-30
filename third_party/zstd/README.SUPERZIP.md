# SuperZip Zstandard Runtime Notes

SuperZip uses the official `facebook/zstd` v1.5.7 release for native `.zst`
and `.tar.zst` compatibility.

- Upstream: <https://github.com/facebook/zstd>
- Release tag: `v1.5.7`
- Original source archive: `third_party/upstream/zstd/v1.5.7/zstd-v1.5.7.zip`
- Official Win64 runtime package: `third_party/upstream/zstd/v1.5.7/zstd-v1.5.7-win64.zip`
- Build output DLL: built from unmodified pinned upstream C sources in the CMake binary directory with multithreading enabled, then copied beside each executable. The repository CMake target preserves upstream MSVC definitions and legacy decoder level 5 without configuring the upstream CLI, tests, or GNU assembler project.
- Source SHA-256: `7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4`
- Win64 package SHA-256: `acb4e8111511749dc7a3ebedca9b04190e37a17afeb73f55d4425dbf0b90fad9`
- DLL identity: its build-specific SHA-256 is embedded in the executable through `cmake/WriteRuntimeIdentity.cmake`. Runtime loading rejects a DLL that differs from that build.
- License: BSD, preserved in `LICENSE`

Production policy:

- Keep original upstream archives untouched under `third_party/upstream`.
- Build upstream implementation source only in the binary directory. Do not
  track or modify extracted sources or runtime binaries in the source tree.
- Load `libzstd.dll` only from the executable directory; never from the current
  working directory.
- Validate the runtime version number at startup and fail closed unless it is
  `10507` (`1.5.7`).
- Keep Zstandard compression in-process through the DLL API; never shell out to
  `zstd.exe` or depend on a host-installed archive tool.
- Enable frame checksums for archives SuperZip creates.
- Cap decompression window size at the wrapper boundary to avoid untrusted
  streams forcing unbounded memory growth.
