# SuperZip Lhasa Integration Notes

SuperZip vendors Lhasa `0.5.0` for native extract-only LHA/LZH compatibility.

- Upstream: https://github.com/fragglet/lhasa
- Release tag: `v0.5.0`
- Release commit: `450172de282c8f8730696f4370a57cf49bfabf22`
- Source archive SHA-256: `1ae8d82d37fc12ec2c52c520b6528ec61268e243f33eca4446b440e182c66d91`
- License: ISC, preserved at `third_party/lhasa/COPYING.md`

The unmodified release archive is stored under
`third_party/upstream/lhasa/0.5.0/` for provenance. Production integration
changes belong only in `third_party/lhasa/`.

Local hardening changes in the production copy:

- Scanner-clean byte copy/set helpers in `lib/lhasa_compat.h`.
- Checked allocation and string-length helpers in `lib/lhasa_compat.h`.
- Include-only decoder templates renamed from `.c` to private template
  headers so static analysis treats them as templates included by the concrete
  decoder modules.
- Dead debug-output removal in `lib/tree_decode_template.h` and
  `lib/lh1_decoder.c`.
- Unsigned MS-DOS timestamp bit extraction in `lib/lha_file_header.c` to keep
  malformed high-bit timestamp fields defined under UBSan.
- Preservation of decoded `..` path components so SuperZip's strict path
  validator rejects malicious archive paths instead of accepting rewritten
  names.
- Removal of Lhasa's unused filesystem extraction helpers and platform write
  abstraction from the production copy. SuperZip retains only header parsing,
  decompression, and CRC validation.

SuperZip does not call Lhasa's file extraction helper. Product extraction uses
Lhasa only for header/decode/CRC validation, then publishes approved files
through SuperZip's own path-safety and verified temporary-file pipeline.
