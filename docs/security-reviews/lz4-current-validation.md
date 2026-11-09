# LZ4 Source-Finding Validation

These individual decisions use the maintainer's authorization to fix defects and
admit only proven false positives. Import identity and numeric style classification
do not establish security acceptance. The source ledger binds each decision to
its complete source, location, producer and relevant evidence. Changed identities
remain blocking until reviewed; raw reports are retained.

The active library is the unchanged official LZ4 1.11.0 raw-block implementation,
verified against its pinned release archive. FlitzZip does not use its frame,
ring-buffer or deprecated unsafe decoding APIs. The production CPU reader calls
`LZ4_decompress_safe` with bounded segment extents and requires its returned length
to equal the descriptor. A successful shorter decode is rejected, including by
the checksum path. Independent literal and overlapping-match fixtures, output
canaries and guarded input/output pages exercise this contract. Candidate checks
do not replace the affected product tests or exact-commit hosted analyses.

The native CPU Deflate workspace now reuses worker-owned miniz state. Review of
that caller change confirms that `materialize_dictionary_cpu`, its exact-length
check, the dictionary extent parser and the GPU dictionary producer are unchanged.
The known LZ4 operations below retain their exact source and locations. Renewed
context identities cover the changed translation unit; they do not admit the new
Deflate helper or substitute for its own correctness and scanner qualification.

## DevSkim Reports

The pinned local detector reproduced exactly these six reports on the frozen
1.11.0 source. Every highlighted token and its source coordinates match the
preceding individually reviewed operation. The complete file and current callers
were reviewed again rather than inheriting the earlier file hash.

### Alert 846

`DS154189`, `third_party/lz4/lz4.c:359`, reports the C `memmove` fallback in
`LZ4_memmove`. Overlap is required by dictionary-copy operations. The primitive
choice does not establish a bounds defect: current callers validate the relevant
input, dictionary and output regions before copying. This decision covers this
macro diagnostic, not any other extent report or every possible input.

### Alert 847

`DS121708`, `third_party/lz4/lz4.c:351`, reports the standard C fallback of
`LZ4_memcpy`, beside the compiler-builtin branch. It does not choose a copy extent.
The bounded decoder and dictionary callers retain their checks; the standalone
candidate probe also rejects malformed cases without crossing guarded spans.
This decision admits only the primitive-choice report.

### Alert 848

`DS154189`, `third_party/lz4/lz4.c:286`, reports debug newline output. The format
is a constant literal and the code is enabled only when `LZ4_DEBUG >= 2`.

### Alert 849

`DS154189`, `third_party/lz4/lz4.c:285`, reports the variadic debug formatter.
All current `DEBUGLOG` invocations in this translation unit start with literal
format strings. Input-derived values are arguments, not formats. The production
default disables this debug branch. A changed call site requires renewed review.

### Alert 850

`DS154189`, `third_party/lz4/lz4.c:284`, reports debug source/line output. Its
format is the constant file/line diagnostic; `__FILE__` and `__LINE__` supply the
values. This has no attacker-selected format string and is debug-only.

### Alert 851

`DS161085`, `third_party/lz4/lz4.c:225`, reports the default C allocation macro.
Its current allocation calls use fixed stream or decoder-state `sizeof` extents
and the matching allocator/free family. Custom and disabled allocator modes keep
their separate contracts. This allocator-choice report does not demonstrate an
unbounded allocation or mismatched owner and does not admit other lifetime reports.

## Header Inclusion Contract

### Alert 10227

`cpp/missing-header-guard`, `third_party/lz4/lz4.h:1`, identifies a deliberately
re-enterable header. Public declarations and the optional static API have
separate guards. Defining `LZ4_STATIC_LINKING_ONLY` after an initial include must
make the later declarations available. The leading standard-header includes are
outside C linkage and use their own system guards.

A Windows C++20 compile/link control includes the header once, enables the
static API, then includes it twice more. It requires the optional function
declaration and verifies the linked version API. That control passes without
redefinitions. A whole-file guard would remove the required late-inclusion
behavior. This static review renews only this known diagnostic; current CPU and
HIP-host CodeQL analyses remain required after publication.

The current CPU and HIP-host analyses reported this instance after the upstream
import. Its preceding alert 853 identifies an older producer instance; the active
ledger now binds the new alert, exact current header and both reported categories.
A source-bound compile control accepts the original header and linked version API.
An isolated copy with a whole-file `#pragma once` fails to declare the required
late opt-in API. This negative control confirms that the suggested whole-file
guard would remove supported behavior; no product header was changed.

The preceding four `cpp/constant-comparison` decisions are retired from the
active ledger because their old source locations no longer identify the reviewed
expressions. They are not transferred to new locations. Current analyses must
establish whether the rewritten upstream decoder produces any new reports.
