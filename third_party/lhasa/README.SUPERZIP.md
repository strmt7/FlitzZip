# SuperZip Lhasa Integration Notes

SuperZip vendors Lhasa `0.6.0` for native extract-only LHA/LZH compatibility.

- Upstream: <https://github.com/fragglet/lhasa>
- Release tag: `v0.6.0`
- Release commit: `75ed83559f23e9538e0045c62f53f77ab03d03d6`
- Source archive SHA-256: `9840154367f73e9d9c3196f944a121ab4d398d84e921c8fe8fca8a931274aed7`
- License: ISC, preserved at `third_party/lhasa/COPYING.md`

The unmodified release archive is stored under
`third_party/upstream/lhasa/0.6.0/` for provenance. Production integration
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
- The PM2 code-count guard previously backported from
  `765658909da866238055b8309034d2e3fe7f81b6` is included in upstream 0.6.0.
- Unlike upstream 0.6.0, unnamed regular-file entries remain rejected.
  SuperZip does not manufacture destination names for malformed metadata.
- LK7 offset codes are restricted to the 16-bit history domain and shifted as
  unsigned values, preventing attacker-controlled signed-overflow behavior.
- `lha_reader_check` finishes and validates an already-open decoder so the
  extraction publication pass verifies the exact streamed payload rather than
  attempting to reopen a decoder at end-of-input.
- Removal of Lhasa's unused filesystem extraction helpers and platform write
  abstraction from the production copy. SuperZip retains only header parsing,
  decompression, and CRC validation.

SuperZip does not call Lhasa's file extraction helper. Product extraction uses
Lhasa only for header/decode/CRC validation, then publishes approved files
through SuperZip's own path-safety and verified temporary-file pipeline.
