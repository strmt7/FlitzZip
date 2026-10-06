# Archive Engineering Review

## Scope And Evidence

Reviewed on 2026-10-01 against the revisions below. This is a focused review of
selected source, product documentation, and licenses, not an exhaustive audit
of these projects or a performance comparison. No implementation, artwork,
screenshots, benchmark numbers, or test corpus was imported.

The priority order is SuperZip's engineering judgment. An upstream design is
evidence that a technique is practical, not proof that it benefits this app.
SuperZip remains a Windows-native C++20 application with AMD HIP acceleration
for its versioned SUZIP format and in-process compatibility adapters.

| Project | Reviewed revision | Evidence and applicable practice |
| --- | --- | --- |
| 7-Zip | [0766b733](https://github.com/ip7z/7zip/tree/0766b733fe3e06dd2a7f9a3cfbf2108ac73abd17) | [LZMA2 configuration](https://github.com/ip7z/7zip/blob/0766b733fe3e06dd2a7f9a3cfbf2108ac73abd17/C/Lzma2Enc.c): normalizes codec threads, block threads, and total threads together; reduces useful block concurrency when input size is known. Adopt the resource-accounting principle, not its codec or thread constants. |
| PeaZip | [6eaca9a0](https://github.com/peazip/PeaZip/tree/6eaca9a02724b3dced6ffd372c2d0156134c89b8) | [Product README](https://github.com/peazip/PeaZip/blob/6eaca9a02724b3dced6ffd372c2d0156134c89b8/README.md) and selected declarations/comments in [the main form](https://github.com/peazip/PeaZip/blob/6eaca9a02724b3dced6ffd372c2d0156134c89b8/peazip-sources/dev/peach.pas): user-task presentation, scriptable jobs, and Unicode-aware process boundaries. Its external-tool orchestration is not suitable for SuperZip's in-process product contract. The large form was not reviewed line by line and is not a structure to copy. |
| NanaZip | [a6fc1284](https://github.com/M2Team/NanaZip/tree/a6fc12843f1936f9d094b2569176c7d1640a03c4) | [Product README](https://github.com/M2Team/NanaZip/blob/a6fc12843f1936f9d094b2569176c7d1640a03c4/ReadMe.md), [mitigations](https://github.com/M2Team/NanaZip/blob/a6fc12843f1936f9d094b2569176c7d1640a03c4/K7Base/K7BaseMitigations.cpp), and [policies](https://github.com/M2Team/NanaZip/blob/a6fc12843f1936f9d094b2569176c7d1640a03c4/Documents/Policies.md): explicit deployment limits, Windows download-origin propagation, and policy-aware process hardening. Evaluate compatibility before adopting a mitigation. |
| libarchive | [20931756](https://github.com/libarchive/libarchive/tree/209317564787ec3826177f273f3125220d41f0af) | [Streaming design](https://github.com/libarchive/libarchive/blob/209317564787ec3826177f273f3125220d41f0af/README.md) and [absolute-path regression](https://github.com/libarchive/libarchive/blob/209317564787ec3826177f273f3125220d41f0af/libarchive/test/test_write_disk_secure_noabsolutepaths.c): common stream APIs, distinct format/filter responsibilities, and executable extraction-policy tests. Its documented global-state/threading limitations must not be copied into SuperZip. |
| Zstandard | [01b7154f](https://github.com/facebook/zstd/tree/01b7154f1172432f8abe9b3bb9909e14a1176b7d) | [Worker/context pools](https://github.com/facebook/zstd/blob/01b7154f1172432f8abe9b3bb9909e14a1176b7d/lib/compress/zstdmt_compress.c), [test categories](https://github.com/facebook/zstd/blob/01b7154f1172432f8abe9b3bb9909e14a1176b7d/TESTING.md), and [benchmark disclosure](https://github.com/facebook/zstd/blob/01b7154f1172432f8abe9b3bb9909e14a1176b7d/README.md): bounded reuse, separate concurrency testing, decoder-version checks, and explicit hardware/compiler/corpus context. Historical CI descriptions are not proof that today's workflow runs those checks. |
| XZ Utils | [3b1efb04](https://github.com/tukaani-project/xz/tree/3b1efb04d17c3a9ef7f473d73af13f1531428ffe) | [Multithreaded encoder](https://github.com/tukaani-project/xz/blob/3b1efb04d17c3a9ef7f473d73af13f1531428ffe/src/liblzma/common/stream_encoder_mt.c): explicit worker state, cancellation checkpoints, lazy worker creation, and overflow-checked memory estimates including input buffers, codec workspaces, and output queues. Apply these principles to existing codecs; this does not approve a new XZ encoder dependency. |

## Licensing Boundaries

Reading and linking these sources does not import their code or license terms
into SuperZip. No trademark endorsement or blanket redistribution permission
is implied. Any future import must be checked against the exact file and
revision, approved as a dependency when applicable, and given provenance and
required notices before it enters production.

- [7-Zip's license](https://github.com/ip7z/7zip/blob/0766b733fe3e06dd2a7f9a3cfbf2108ac73abd17/DOC/License.txt)
  is mixed: LGPL, public-domain files, BSD portions, and an unRAR restriction.
  The reviewed LZMA2 source declares public domain; that does not license every
  other 7-Zip file identically.
- [PeaZip](https://github.com/peazip/PeaZip/blob/6eaca9a02724b3dced6ffd372c2d0156134c89b8/LICENSE)
  publishes LGPLv3 terms. Bundled external components need their own review.
- [NanaZip](https://github.com/M2Team/NanaZip/blob/a6fc12843f1936f9d094b2569176c7d1640a03c4/License.md)
  distinguishes MIT project code, inherited 7-Zip/third-party code, and
  CC BY-ND file-association icons. Do not copy its artwork.
- [libarchive](https://github.com/libarchive/libarchive/blob/209317564787ec3826177f273f3125220d41f0af/COPYING)
  has a BSD-style baseline with per-file exceptions and varied build-script
  licenses. Check the linked original and each affected file before reuse.
- [Zstandard](https://github.com/facebook/zstd/blob/01b7154f1172432f8abe9b3bb9909e14a1176b7d/LICENSE)
  identifies BSD licensing, with a GPLv2 alternative documented by the project.
  Consult the original texts rather than a summary of their conditions.
- [XZ Utils](https://github.com/tukaani-project/xz/blob/3b1efb04d17c3a9ef7f473d73af13f1531428ffe/COPYING)
  uses 0BSD for liblzma with different terms for certain scripts, portability
  components, and build files. It is not uniformly licensed as one component.

This source review does not authorize running commercial comparators,
redistributing their binaries, or republishing others' figures. Benchmark
admission remains governed by [the permissions record](../../benchmark-permissions.md).

## Prioritized SuperZip Plan

### 1. Finish Root-Cause Security Batches

Keep scanner inventory and exact analysis identity authoritative. Group related
records by rule and production component to reuse evidence, but retain every
alert ID and examine distinct allocations, callers, configurations, and failure
paths. A safe family member does not prove the rest safe.

The first completed repair covers real SDK byte-access alignment UB and missing
local sanitizer instrumentation. Shared endian/canary oracles and policy guards
now protect that category; the hosted undefined lane also enables alignment.
It does not close the unrelated scanner backlog. Fixed-size-copy API-name
reports need evidence-based review, not unsafe casts, scanner exclusions, or
platform-specific substitutions solely to make an alert disappear.

Separate confirmed defects, non-actionable claims, unresolved claims, and
maintainability reports. For commented-code reports, distinguish obsolete code
from format examples, contracts, diagrams, and supported conditional branches;
retain useful behavior and upstream provenance. Add the narrowest regression
guard for each confirmed root cause and verify closure on the pushed SHA.

### 2. Account For The Whole Pipeline

The current [host budget](../../../src/core/host_memory_budget.cpp) estimates three
buffers per native window, not total process memory. Include active codec
contexts, candidate payloads, descriptors, retained pools, and GPU staging in a
single lifetime-aware admission design. Use bounded estimates or allocator
accounting; current context size is not a guaranteed maximum.

At the review baseline, the [worker share](../../../src/core/worker_budget.hpp)
used a ceiling per active window, which could exceed the requested aggregate;
admitting more windows than workers amplified the one-worker minimum.
Ceiling-sized CPU ranges could also launch empty callbacks for uneven counts.
The follow-up production repair caps active windows, uses floor shares and
balanced nonempty ranges, and shares exact decode grouping with admission.
See [the current worker contract](../../compression-level-and-benchmark-suite.md#aggregate-codec-workers).
Complete codec-workspace/pool accounting remains separate unfinished work.

Acceptance: synthetic RAM/worker matrices, disjoint complete range coverage,
no empty ranges, requested aggregate limits, allocation/failure cleanup, and
the same admission logic in product and RAM benchmarks. Preserve encoded bytes.
Do not replace a volatile OS snapshot with a claim of reserved physical RAM.

### 3. Reduce Repeated CPU Work Without Losing Ratio

Native [CPU encoding](../../../src/core/archive_blocks.cpp) keeps all lower-effort
codec winners to prevent growth, reuses a context within a range, and caps
per-chunk concurrent Zstandard contexts. Context lifetime still ends at each
range, and exhaustive effort trials can dominate compression time.

First measure stage costs and allocation counts. Then evaluate bounded reuse
across windows and candidate-search improvements against held-out input types.
Retain complete lower-effort winners and independent decoding; do not drop
search candidates from unverified heuristics or increase archive size to force
distinct level results. Any retained pool must participate in memory admission.

Acceptance: byte-exact roundtrips, no effort-to-effort encoded-size growth in
the regression set, small/tail/incompressible cases, and matched repeated RAM
comparisons. Codec contexts must never be shared concurrently without ownership.

### 4. Optimize The HIP Pipeline From Stage Evidence

Use existing GPU telemetry to separate submission, transfer, classification,
entropy coding, CRC, decode, and publication costs. Extend measurements only
where needed; event validity and process telemetry are already guarded.
Prioritize the largest demonstrated cost, not a guessed kernel bottleneck.

Evaluate bounded persistent staging, device work batching, and transfer/compute
overlap only with explicit stream ordering, event completion, buffer ownership,
and host/device reservation. Preserve the existing small-file batching gains.
Changing dictionary or entropy representation requires a versioned wire design
and old-reader compatibility decisions, not an invisible format change.

Acceptance: real HIP execution, all supported effort/block-size combinations,
corrupt input rejection, held-out workload families, unchanged sizes for
format-preserving changes, and repeatable end-to-end improvements. Do not infer
CPU saturation or disk limitation from GPU utilization alone.

### 5. Strengthen Product Parity And Windows Hardening

Retain the shared stream/publication helpers and registry-driven format matrix;
do not introduce a second codec path in the GUI, CLI, or benchmarks. Review
cancellation, failed-job cleanup, capability labels, and settings capture by
product workflow. Keep unsupported archive methods explicit.

Evaluate download-origin metadata and compiler/process mitigations separately.
A global child-process ban conflicts with opt-in Defender and shell workflows;
dynamic-code restrictions may conflict with the HIP/driver stack. Test the exact
HIP-enabled release and supported deployment forms before enabling a policy.
Do not copy NanaZip's packaging model or disable required functionality.

Acceptance: CLI/GUI job parity, partial-output preservation, policy failure
diagnostics, all-page UI smoke for UI changes, and portable/MSI runtime checks.

### 6. Publish Comparisons And User Documentation Last

Follow [the comparison methodology](../../comparative-benchmark-methodology.md) and
[research record](../../benchmark-research.md), preserving separate RAM and filesystem
lanes. Reuse comparator results only under the existing version, input, binary,
settings, host, and methodology identity checks. Do not reuse an upstream chart
as SuperZip data or count codec libraries as independent desktop products.

After the implementation and security gates pass, collect the final repeated
suite, inspect every generated graph and error bar, and update end-user material
with measured benefits, capability limits, and actual system/version context.
Release publication remains a separate maintainer approval step.

## Execution Discipline

Each numbered item is a bounded workstream, not permission for one unreviewable
rewrite. Reuse existing tests and the change classifier; run expensive timing
only when behavior or performance evidence changes. Investigate a non-win's
stage costs and resource geometry before accepting or rejecting its mechanism.
None of the planned work above is a completed speedup or universal guarantee.
