# LZMA SDK 26.01

SuperZip vendors the minimal ANSI-C 7z decoder subset from LZMA SDK 26.01.

- Upstream: https://www.7-zip.org/sdk.html
- Archive: `lzma2601.7z`
- SHA-256: `b860f17f9df3c0524dd2ef2c639ab5e43ad0006b77b8f7bb6d191bf528536885`
- License: public domain, as stated in `DOC/lzma-sdk.txt` in the upstream archive.

Only the C decoder files needed by the `7zDec` path are copied into
`third_party/lzma_sdk/C/`. SuperZip does not ship or call the SDK's prebuilt
executables. The original upstream archive and checksum are stored under
`third_party/upstream/lzma-sdk/26.01/` for provenance.

SuperZip uses this SDK only for read-only `.7z` compatibility. Native `.suzip`
GPU compression remains the AMD HIP product path.
