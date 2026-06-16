# SuperZip Zstandard Runtime Notes

SuperZip uses the official `facebook/zstd` v1.5.7 release for native `.zst`
and `.tar.zst` compatibility.

- Upstream: <https://github.com/facebook/zstd>
- Release tag: `v1.5.7`
- Original source archive: `third_party/upstream/zstd/v1.5.7/zstd-v1.5.7.zip`
- Official Win64 runtime package: `third_party/upstream/zstd/v1.5.7/zstd-v1.5.7-win64.zip`
- Bundled runtime package: `third_party/upstream/zstd/v1.5.7/zstd-v1.5.7-win64.zip`
- Build output DLL: extracted from the runtime package into the CMake binary directory and copied beside each SuperZip executable.
- Source SHA-256: `7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4`
- Win64 package SHA-256: `acb4e8111511749dc7a3ebedca9b04190e37a17afeb73f55d4425dbf0b90fad9`
- Extracted DLL SHA-256: `8f07e1112ed283e5cd2798833e9a3c32d8961381bc36da04af57a1b0ca9bd40b`
- License: BSD, preserved in `LICENSE`

Production policy:

- Keep original upstream archives untouched under `third_party/upstream`.
- Do not compile, track, or modify third-party Zstandard implementation source
  or extracted runtime binaries in the SuperZip source tree.
- Load `libzstd.dll` only from the executable directory; never from the current
  working directory.
- Validate the runtime version number at startup and fail closed unless it is
  `10507` (`1.5.7`).
- Keep Zstandard compression in-process through the DLL API; never shell out to
  `zstd.exe` or depend on a host-installed archive tool.
- Enable frame checksums for archives SuperZip creates.
- Cap decompression window size at the wrapper boundary to avoid untrusted
  streams forcing unbounded memory growth.
