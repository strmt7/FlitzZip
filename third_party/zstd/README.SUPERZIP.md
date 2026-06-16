# SuperZip Zstandard Vendor Notes

SuperZip vendors a source subset of the official `facebook/zstd` v1.5.7
release for in-process `.zst` and `.tar.zst` compatibility.

- Upstream: <https://github.com/facebook/zstd>
- Release tag: `v1.5.7`
- Source archive: `third_party/upstream/zstd/v1.5.7/zstd-v1.5.7.zip`
- SHA-256: `7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4`
- License: BSD, preserved in `LICENSE`

The production copy keeps the public headers plus `lib/common`, `lib/compress`,
and `lib/decompress` C sources required by libzstd. SuperZip builds this path
as a static, in-process, single-threaded compatibility backend. It does not
shell out to `zstd.exe` and does not rely on host-installed archive tools.

SuperZip enables frame checksums for archives it creates and caps decompression
window size at the wrapper boundary to avoid untrusted streams forcing
unbounded memory growth.
