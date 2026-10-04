# SDK Fixed-Width Copy Review - 4 October 2026

Current status: historical review, superseded as an acceptance mechanism by
the [source remediation redesign](security-source-remediation-redesign.md).
All six matching source reports now remain blocking. The analysis below
records the earlier assessment and its tested revision; it cannot establish
current safety, source closure or final acceptance.

Status: maintainer approved these exact six dispositions on 4 October 2026.
Normalized complete-source SHA-256:
`196748d22995e4764288f514d8b2af56371c9e13db052d28fe066cd640288792`.
The current publication preflight retains six `DS121708` findings in
`third_party/lzma_sdk/C/CpuArch.h`. These copies are the existing production
repair for the independently reproduced alignment/aliasing fault. The folder
parser rewrite does not introduce or change their executable bodies. Removal
of four disabled architecture-test directives changes their file identity and
line locations, so previous observations cannot silently admit this revision.

| Function | Complete finding region | Exact extent and ownership |
| --- | --- | --- |
| `Z7_GetNative16` | 487:2-487:33 | Reads exactly `sizeof(UInt16)` from the caller's documented two-byte span into a live local object of that same type. |
| `Z7_GetNative32` | 496:2-496:33 | Reads exactly `sizeof(UInt32)` from the caller's documented four-byte span into a live local object of that same type. |
| `Z7_GetNative64` | 505:2-505:33 | Reads exactly `sizeof(UInt64)` from the caller's documented eight-byte span into a live local object of that same type. |
| `Z7_SetNative16` | 513:2-513:33 | Reads the live two-byte value object and writes exactly two bytes to the caller's documented writable span. |
| `Z7_SetNative32` | 520:2-520:33 | Reads the live four-byte value object and writes exactly four bytes to the caller's documented writable span. |
| `Z7_SetNative64` | 527:2-527:33 | Reads the live eight-byte value object and writes exactly eight bytes to the caller's documented writable span. |

Each copy has a fixed compile-time extent, with one side sized by its actual
object type. Copying object bytes avoids the alignment and aliasing assumptions
of the previous typed dereference implementation. No allocation, pointer
retention or variable-length copy occurs in these six operations. Caller input
extent validation remains an independent requirement; this review does not
dispose any caller-boundary, pointer, lifetime or other rule finding.

The shared byte-access oracle checks wire values, round trips and neighboring
canaries for offsets 0-15. The unchanged checks passed in the complete HIP
native test driver after the SDK rewrite. All 17 direct 7z tests also pass,
including the valid empty Copy folder and fourteen malformed descriptor/binding
controls, fragmented PPMd input, CRC, truncation, names and overwrite behavior.
The source review does not claim universal caller safety or final acceptance.

Admission must use this complete file's normalized SHA-256 and
each exact rule/region. Detectors, raw SARIF, rules and scanned paths must remain
unchanged. Any source or location change invalidates the admission. The other
findings remain blocking; no broader suppression is proposed.
