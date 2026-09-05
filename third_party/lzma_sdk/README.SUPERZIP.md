# LZMA SDK 26.03

SuperZip vendors the minimal ANSI-C decoder subset from LZMA SDK 26.03.

- Upstream: <https://www.7-zip.org/sdk.html>
- Archive: `lzma2603.7z` from the official `ip7z/7zip` release `26.03`.
- SHA-256: `86c213f752520ab5325c310f50bef63ec344b56dd1c80b0246d06dc6cec953b2`
- License: public domain, as stated in `DOC/lzma-sdk.txt` in the upstream archive.
  `LICENSE` is an exact text copy of that upstream file for build-time license
  notice generation on hosts that cannot extract `.7z` archives.

Only the C decoder files needed by the `7zDec` path are copied into
`third_party/lzma_sdk/C/`. SuperZip does not ship or call the SDK's prebuilt
executables. The original upstream archive and checksum are stored under
`third_party/upstream/lzma-sdk/26.03/` for provenance. The older 26.01
provenance archive remains unchanged.

SuperZip uses this SDK for read-only `.7z`, `.lzma`, and `.lz` compatibility,
including the `.tar.lz` stream adapter. Native `.suzip` GPU compression remains
the AMD HIP product path.

## Local Adaptations

The previous 26.02 FilesInfo backport is now supplied by upstream 26.03:

- singleton FilesInfo properties are rejected instead of replacing owning
  pointers;
- each FilesInfo property is parsed through its declared bounded slice;
- zero-sized Name properties and trailing external-property bytes are rejected;
- archives without a Name property receive the SDK's defined empty name rather
  than dereferencing a null offset table.

The 26.03 update also includes upstream PPMd state allocation through the
bounded allocator and LZMA probability-offset arithmetic changes. The existing
SuperZip byte-copy/fill helpers remain in the production subset. Unused
upstream `Z7_ANALYZE_MODE` warning suppressions are not imported. Some unchanged
files retain historical comment removal; the upstream archive contains the
unmodified complete source and public-domain notices.
