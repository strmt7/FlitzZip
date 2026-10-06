# Intermediate Beta Review — 3 October 2026

This packet proposes **SuperZip 0.8.0 as an internal beta review candidate**.
The configured version remains 0.8.0; no tag or release has been published.
Maintainer review is the next decision. The modernization goal will pause once
the packet and its local qualification are ready, as requested. Publication
still requires the held acceptance gates below and explicit approval.

## Candidate Scope

The codec scope is frozen at `e950e06035959b81938ca7bb64c45d64980b545f`.
The subsequent evidence/documentation batch is
`8cabac46fa306dbab0dbcb9591440f0b7c50c7ad`. The final packaging adjustment adds
the existing brand, benchmark and design assets to CMake installation so README
and shipped documentation image links resolve. It does not change codecs or
the canonical logo. The resulting CLI remains byte-identical to the benchmark
binary, SHA-256
`a2294790af46e0cf243c5012381c41760ba1bd52af517109004c60d9219a0b06`.
Build receipts distinguish the packaging input change from the earlier timed
study identities; the original records are not relabeled.

The candidate includes the complete pinned ROCm Core SDK 10.0.0 migration,
six-target HIP compilation, native version-eight Huffman support, bounded
CPU/GPU ownership and concurrency, archive read-back/path hardening, reduced
allocation/event overhead, scientific sampling and focused development tooling.
[Draft release notes](../../releases/0.8.0.md) and the
[implementation ledger](../../../IMPLEMENTATION_PLAN.md) separate historical evidence
from current qualification. Exact GPU execution has been tested on the available
Radeon RX 9070 XT; compiling six images does not qualify every target GPU.

## Current Evidence

| Area | Verified evidence and practical limit |
| --- | --- |
| Native block sizes | [Seven level-5 cases](../benchmarks/native-beta-blocks-2026-10-03.md), 42 pilots plus 112 confirmations. Every observation passed byte-exact RAM validation. Only 8 MiB met all declared timing-quality safeguards. Six inconclusive cells and the separate 29-pilot aborted setup remain visible. |
| Native efforts | [All nine exact sizes](../benchmarks/native-beta-effort-sizes-2026-10-03.md), 18 RAM observations. Timings remain unqualified; no single-run tradeoff or speed claim. |
| Writable formats | [132 archive/reader cases](../benchmarks/beta-format-sizes-2026-10-03.md) across 13 writable formats and six bounded synthetic fixtures, with available independent 7-Zip/Zstandard/libarchive writers/readers. Sizes only; no filesystem throughput. |
| Registry contracts | CLI matrix passed for all 36 registered formats, including extract-only fixtures, aliases, overwrite refusal and all nine accepted efforts for level-aware writers. Native SUZIP in this matrix is CPU-only; HIP proof is separate. |
| Plot and raw records | README leads with the admitted 8 MiB result. All observations and exact byte counts are retained; CI regenerates the chart. Encoded payload and full modeled archive bytes are explicitly distinguished. Historical plots remain linked and dated. |
| Native/package input change | The canonical HIP build passed after the install-rule adjustment, without changing the CLI bytes. All six CTest targets passed, including 582 main-registry native cases. Corpus integration, local lint, policy, MSI identity and native selection contracts passed. |
| Local artifacts | HIP-enabled x64 portable ZIP and per-machine MSI passed canonical staging and checksum checks. Independent 7-Zip integrity/extraction, packaged HIP dependency checks and source-hash agreement for all 11 documentation assets passed. MSI installation smoke is pending on an isolated elevated host. |

On the qualified synthetic 8 MiB case, observed CPU/GPU mean combined elapsed
time was 10.205179/5.078817 seconds. The GPU archive was **11,768,213 bytes
(0.2606%) larger**, and GPU extraction alone was slower. This does not establish
both smaller and faster output, general application superiority, a ROCm-caused
speedup, or a before/after improvement. The real household-power diagnostics
are separate evidence and expose workload-dependent ratio/speed weaknesses.

## Artifacts For Local Review

The canonical output names are:

- `out/SuperZip-0.8.0-win64-portable.zip` and its `.sha256` sidecar.
- `out/SuperZip-0.8.0-win64.msi` and its `.sha256` sidecar.

Artifacts and machine-local receipts are intentionally ignored, not committed.
The portable ZIP can be inspected without replacing an installed application.
The current installation was preserved; do not run MSI uninstall against it
as a substitute for isolated candidate testing. These are internal-review
artifacts, not approved distribution binaries. Final publication must rebuild
from the approved immutable source and use the release workflow's identity,
checksums, source offering and installer qualification.

## Held Acceptance And Follow-Up Work

The latest [finding review](../security/security-finding-review-2026-10-03.md) covers
`8cb3f4d`: all ten security jobs succeed, while the required post-push audit
fails on 195 unapproved alerts (183 CodeQL and 12 DevSkim). The two residual
Scorecard findings are separately approved. Passive benchmark reports no
longer have open DevSkim findings; actual code, tests and configuration remain
scanned. This does not clear the security gate or requalify the historical
packages described above. The current development CLI differs from the
original candidate after a benchmark-observation guard change; the historical
measurements and package hashes retain their original identities.

- Open unapproved code-scanning alerts remain an acceptance failure. They are
  not all independently proved vulnerabilities, but cannot be declared fixed,
  dismissed or suppressed merely to qualify this beta. The final hosted audit
  must retain its actual failure and counts.
- Final exact-SHA hosted qualification and accumulated-range acceptance are
  separate from an intermediate workflow snapshot. Any unfinished, missing or
  failed run remains pending/failed, never inferred green from a local pass.
- Per-machine MSI install/repair/uninstall requires an isolated elevated host.
  This session is not elevated and an existing installation is present.
- Fresh repeated effort tradeoff, additional 10 GiB profiles, representative
  licensed corpora and application comparator timings remain incomplete. The
  finished level-5 sweep and size diagnostics do not cover those missing cells.
- Broader hardware execution, GUI changes beyond the already reviewed native
  design, small-entry GPU batching and ratio/speed improvements on authentic
  dictionary-heavy data remain future work. No universal compatibility,
  zero-bug or enterprise-readiness claim is made.

The next decision is whether this bounded candidate is useful for maintainer
review and which held work to resume. Approval to review does not silently
waive release gates, authorize replacement of an older release, or convert the
unfinished modernization goal to complete.
