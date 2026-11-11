# FlitzZip Implementation Plan

## Current Milestone

Prepare a Windows x64, HIP-enabled beta candidate with source-bound verification
and exact ZIP/MSI qualification. Publication requires the maintainer's green light.
See [beta readiness](docs/beta-readiness.md) and [release qualification](docs/release.md).

Use the [investigation skill](.agents/skills/flitzzip-test-investigation/SKILL.md)
and its independent oracles for coherent development batches. Apply the
[release criteria](docs/release.md#acceptance-criteria) and
[coverage acceptance policy](docs/code-coverage.md#release-acceptance) to the
final candidate. Preserve critical behavior tests and reviewed residual risks;
universal 100% line coverage is not a release requirement.

## Engineering Priorities

1. Preserve archive correctness, backward readability, safe extraction and explicit
   format capabilities. Fix production boundaries and retain independent regressions.
2. Keep Neutron star mode GPU-only and separate from numeric efforts. Optimize
   complete archive bytes across representative workloads; transfer changes to
   ordinary efforts only after demonstrating both smaller size and faster execution.
   Pursue structural gains in representation, entropy modelling and GPU work
   scheduling; small overhead reductions alone do not meet the compression goal.
3. Investigate GPU failures through code, runtime and retained execution evidence.
   Keep unexplained incidents visible; a successful rerun does not establish a cause.
4. Repair actionable security findings at source. Preserve raw findings, provenance
   and executable recurrence guards. Individually proven false positives use
   the authorized exact-source review contract without suppressing scanner output.
5. Use licensed real corpora and RAM-only transport. Separate size qualification,
   descriptive timings, controlled comparisons and broader effectiveness claims.
6. Keep development tools portable, verification proportional to changed consumers,
   and current documentation concise. Reuse evidence only when its identities match.
7. Qualify the exact portable ZIP and MSI payloads, checksums and installer lifecycle
   before requesting publication approval.
8. Improve the CPU lane across supported algorithms. Review each writer and reader
   separately, including allocation, checksums and streaming. Qualify comparative
   performance only when permitted. Preserve independent interoperability and avoid claiming
   that a native Zstandard improvement qualifies Deflate, Bzip2, LZMA or other codecs.

[Agent instructions](AGENTS.md) define the mandatory operating rules.
[Benchmark index](docs/benchmarks/README.md) identifies current evidence and limitations.
Completed checkpoint narratives remain in Git history and local review artifacts.

## Current Assessment

Baseline: source `dec2dca` and its
[qualified candidate](https://github.com/strmt7/FlitzZip/actions/runs/42993879195).
Revalidate changed inputs and live services before using this assessment for a
later revision. This is a source-backed baseline, not a claim of a flawless app.

Revision `089d944` contains the subsequent probe-generation repair. Its hosted
ASan, component-contract, lint and Scorecard workflows passed. ROCm qualification
[failed during configuration](https://github.com/strmt7/FlitzZip/actions/runs/43010070857)
with `HIP architecture resolution failed:` and no child diagnostic; the cause is
not established. Windows CI and C++ analysis were still running at inspection,
and DeepSource reported `Analysis quota is exhausted.` with a skipped check.
Do not infer current acceptance from the
qualified baseline. The same live inventory confirmed only `main`, no open PRs
and no open Dependabot alerts; those facts must be refreshed at final acceptance.

| Area | Verified state | Remaining problem or limit |
| --- | --- | --- |
| Product | Windows-native C++20 app, CLI, in-process adapters and HIP compute boundary | Exact methods differ by format; preserve the [capability matrix](docs/archive-format-support.md) |
| Native tests | Seven local suites passed with GPU admission and with devices unavailable; main registry had 1,220 passing cases | Counts do not prove universal hardware coverage or absence of defects |
| Hosted/package checks | Required Actions, HIP compilation/no-device execution, ZIP/MSI and installer lifecycle passed | Later changed inputs require applicable fresh qualification |
| GitHub maintenance | Only `main`; no open PRs or Dependabot alerts at assessment | These inventories are live, not permanent guarantees |
| Analysis acceptance | Exact governance baseline accepted; raw reports retained | DeepSource skipped the current revision because its analysis quota is exhausted; current analyzer results and complete inventory are unavailable |
| Neutron | GPU-only search remains separate from efforts 1–9; independent readback and real device-work controls exist | Broad size superiority and comparative speed are unproven; approved rANS is inactive in production |
| Numeric efforts | Nested CPU frame search and GPU entropy selection have scoped contracts | Complete-archive monotonicity is not established across all writers, partitions and resource policies |
| Build generation | Shared probe output writing preserves unchanged bytes/metadata; actual source/header/bridge dependencies are registered and have canonical regressions | Work-count controls prove eliminated recompilation and targeted rebuilds, not a measured speedup |
| Observation | Null-telemetry decode avoids optional timing events; device-resident CRC and checked scratch release are qualified | Non-null counters remain coupled to profiling events and clocks |
| Maintainability | Audit identifies large GUI, GPU, test and tooling files | Size is a review lead, not a defect or justification for mechanical splitting |
| Dependencies/tools | Pinned graphs have provenance and installed-consumer evidence; constrained parents remain explicit | Resolution is not runtime acceptance; universal upstream latestness is not established |
| Coverage/docs | CPU, HIP host, GUI, tooling and device scopes are separate; user/developer guides have canonical homes | No verified whole-repository percentage or universal hardware claim is available |

## Execution With The Active Goal

This canonical plan is already required by `AGENTS.md` and the development-context
reading map. Use it with the existing active goal and one local continuation;
do not create another goal, worker pool, maintenance service or recurring workflow.
Execution remains serial under the repository's single-agent rule.

The active goal prohibits benchmarks and performance-timing calculations. Continue
source review, implementation, ordinary correctness tests, independent complete-size
fixtures and deterministic work-count checks. Do not invoke benchmark controllers
or corpus campaigns, including size-only campaigns. Preserve safety deadlines,
cancellation and resource admission. Keep prohibited qualification deferred.

For each package, establish the contract, concrete problem, independent oracle and
affected consumers before editing. Batch one coherent mechanism with its regressions;
retain old-source or isolated mutation controls when practical. Preserve the first
failure and distinguish product, fixture, collector and infrastructure causes.

Use the canonical targeted verifier and inspect command roles before execution.
Reuse only matching source/tool/output evidence; do not hide a deferred check behind
a narrower pass. Publish reviewed batches on `main` through the official connector,
verify contributor/parent/tree, sample exact-SHA workflows once, and continue
independent work. Update durable package status when it changes and retain detailed
results in the single local continuation. A passing checkpoint does not complete
the full goal.

## Ordered Work Packages

The immediate priority is **B00**, the failed current HIP configuration boundary.
The subsequent ready sequence is **B02 → N01 → C01 → C02 → Q01**; B01 is implemented,
with its successful local evidence retained and overall hosted acceptance pending.
D01 interrupts
it for a demonstrated dependency vulnerability or incompatibility. T01 addresses
missing tool portability evidence without repeating valid installation checks. Resume R01 on
a provider state change; do not let that external blocker stall independent work.
N02 remains held by the measurement restriction. U01 follows affected product changes.

| ID | Work and current state | Dependency | Required completion evidence |
| --- | --- | --- | --- |
| B00 | HIP failure diagnostics repaired with independent controls; hosted cause remains unconfirmed | Current ROCm qualification and retained failing run | Actionable child-process diagnostics, independent rejection/recovery controls and successful affected hosted qualification; preserve unexplained historical failures |
| R01 | Current analysis acceptance: quota exhausted | Provider allowance resumes and a current run completes | Successful required analyzers, complete paginated inventory and exact-commit audit; retain the accepted governance baseline |
| B01 | Probe output invalidation: implemented with canonical regressions and local native/sanitizer qualification | Retain source-bound hosted acceptance | Old behavior detected; unchanged outputs preserve identities and avoid recompilation; changed inputs rebuild consumers |
| B02 | Shared optional host phase clock implemented; full counters-only observation remains in design | Coherent snapshot/aggregation and event-ownership contract | Real HIP counters without optional profiling; correct bytes, inheritance, failure cleanup and reuse; prohibited timing checks remain deferred |
| C01 | Complete-size effort progression: scoped review next | Existing nested-search inventory | Independent full-size/decoding regressions for every advertised guarantee; no padding or manufactured low-effort losses |
| C02 | CPU codec mechanisms: planned | Demonstrated work/ownership problem per codec | Independent read/write oracles and failure/recovery checks; supported capabilities unchanged; no unmeasured speed claim |
| N01 | Approved entropy integration: private bounded proofs exist | B02 and approved format contract | Integrated device-produced models/streams, independent readback/CRC, malformed-input and lifetime controls |
| N02 | Representative Neutron effectiveness: deferred | N01 and lifted benchmark restriction | Complete-size evidence on licensed independent workloads, including losses and all model/framing bytes, before production adoption |
| D01 | Dependency lifecycle: alerts clear at assessment | Official metadata, compatible graph and provenance | Reproducible pins plus fresh/update installed-consumer tests; explicit unsupported/constrained platforms |
| T01 | Development-tool portability: retain qualified consumers and close evidence gaps | Supported provider/host/checkout contracts | Fresh/update/recovery consumers, actual index/crawl receipts and owned-process cleanup; no user-specific repository configuration |
| Q01 | Critical gaps and complexity: ongoing | Actual risk and source-bound gap inventory | Meaningful success/rejection/recovery assertions and repaired root causes; explicit residual behavior/measurement gaps |
| U01 | User experience, upgrades and docs: maintain affected contracts | Changed product/settings/package behavior | Migration and failure/recovery tests, disposable-host installer qualification, affected GUI/figure review and accurate docs |

### B00 — HIP Configuration Failure

Scope: `cmake/ResolveHipArchitecture.cmake`, `tools/hip_architecture.ps1`, their
direct callers and existing configuration/architecture tests.

The resolver now retains its child result and both captured streams in failures.
Real subprocess controls cover nonzero stdout/stderr, malformed successful output,
missing commands and deadline expiry after valid partial output, followed by
canonical recovery. Both Windows PowerShell and PowerShell 7 passed these controls;
the original resolver fails the diagnostic-loss assertion. Target policy and the
existing deadline are unchanged. This repairs diagnostic loss, not the unconfirmed
cause of the earlier hosted configuration failure. Current hosted qualification
remains required.

1. Preserve the failing run, exact source, SDK/compiler identity and command.
   The observed failure precedes HIP compilation; it does not demonstrate a
   kernel defect, unsupported GPU or defective probe output.
2. Inspect the subprocess result, standard output and standard error together.
   The current fatal message reports only standard error and can lose the useful
   distinction between failed launch, deadline, nonzero exit and malformed output.
   Establish the actual cause before changing environment or admission policy.
3. Exercise canonical success and independent nonzero, malformed-output,
   unavailable-command and bounded-deadline controls at the resolver boundary.
   Keep diagnostics actionable, reject partial target lists and prove recovery.
   Do not repair this by weakening validation, retrying into a pass or blindly
   increasing a deadline.
4. Qualify the affected native configure/build consumer and current hosted ROCm
   job. Reuse unrelated successful evidence while source/tool identities match;
   retain incomplete Windows/analysis results until their actual outcomes arrive.

### B01 — Probe Generation Contract

Scope: `tests/zstd/RawBlockProbe.cmake`, `HuffmanTableProbe.cmake`,
`DictionaryMergeProbe.cmake` and the existing dependency CMake contract suite.

1. Share a small text-output boundary. Validate source/region identities before
   output reuse; preserve generated definitions and provenance. Do not cache
   unverified compiler products.
2. Cover first emission, unchanged output, corruption repair, changed bridge input,
   restoration and unrelated-producer independence. Use fixed metadata sentinels,
   not elapsed measurements, to detect unnecessary writes.
3. Reject altered source inputs without mutating admitted outputs. Keep native
   newline semantics and the declared CMake minimum compatible; avoid adding a
   dependency merely to compare generated text.
4. Check real unchanged incremental compile work and a real input-induced rebuild.
   Counts establish eliminated work, not a measured speedup.

### B02 — Observation Contract

Scope: telemetry/snapshots, shared dispatch, prefix/dictionary encoders,
`NeutronStageClock`, child attempts and structured-output consumers.

Host encode and owned-decode phase boundaries now share an operation-local clock.
Absent phase telemetry, including nested encode trials, requests no clock reads
or recorder callbacks. Tracked nested phases copy their enclosing boundary;
recorder failure retains the last successfully recorded boundary. Fixed clock-read
and tick-sequence fixtures exercise these contracts without reading a real clock.
This is not an exposed counters-only mode: independent prefix/dictionary event
owners and the snapshot/aggregation/output contract below still require integration.

Define an immutable operation mode and distinct not-requested, measured and
failed/incomplete timing states. Specify mixed-mode aggregation before changing
consumers; missing timing must not become zero. Inherit the mode into every child.

Counters-only mode must avoid optional event creation, elapsed queries and profiling
clocks throughout the operation. Preserve completion waits, mandatory deadlines,
cancellation, overflow checks and ownership. Count actual completed work and
transfers; do not invent internal-library kernel counts.

Test inheritance, aggregation, failure, poisoned event reuse and immediate recovery
independently. Audit CLI/output/figure/qualification consumers together. Preserve
existing measured-mode contracts and explicitly defer prohibited timing-producing
qualification rather than silently disabling it.

### C01 — Complete-Size Effort Contract

Map the existing nested CPU frame search and GPU entropy selection before extending
them. Distinguish per-block savings from full archive framing, dictionary choices,
partition changes and resource admission. Retain a lower effort's winning complete
representation wherever the same format/resource contracts permit it.

Use bounded empty, small, repetitive, mixed and already-compressed fixtures with
independent decoding. Count every descriptor, dictionary, model, name, checksum and
index byte. Repair demonstrated regressions through shared candidate accounting,
explicit admissibility and duplicate-trial elimination while preserving tie policy.
Do not manufacture low-effort losses or promise inverse-linear size on arbitrary data.
Keep Neutron outside numeric progression. Representative effectiveness campaigns and
speed qualification remain separate and deferred under the active restriction.

### C02 — CPU Codec Review Contract

Review supported readers and writers separately: ZIP/Deflate/Gzip, Bzip2,
Zstandard, native CPU blocks and read-only LZMA-family adapters. This does not
authorize adding writers to extract-only formats. Start with the format registry
and native codec inventory so every enabled reader/writer, including LZ4 and
method-specific decoders, receives an explicit scope rather than assuming this
summary list is exhaustive.

Inspect repeated allocation, copying, checksum passes, buffer ownership and stream
handoffs. Share bounded mechanisms only where a demonstrated problem warrants it.
Preserve errors, cancellation, interoperability and publication semantics. Retain
independent empty/partial/truncated input, allocation-failure, cleanup and reuse
controls, including an old-source or isolated failure oracle where practical.

Keep instruction-set admission portable across supported x64 hosts; avoid
machine-specific global flags or optimistic execution followed by crash recovery.
Comparative performance remains unqualified until measurement is permitted.

### Q01 And U01 — Review And Maintenance Contract

Review large GUI state, HIP host, format/stream test and build/smoke modules by
responsibility, ownership and consumers. Split only a demonstrated duplication or
responsibility problem, not a file merely because it is long. Batch meaningful
tests before recollecting coverage; review zero-hit and unmeasured-source inventories
with their actual source/tool/binary scope.

Follow the [persistence and upgrade contract](docs/development/persistence-and-upgrades.md)
for stable external keys, unknown fields, schema transitions and recovery. Preserve
append-only historical fixtures and direct upgrades from supported earlier versions;
do not infer compatibility with unknown future implementations. Preserve installer
scope, upgrade identity, portable/MSI parity, repair, rollback, uninstall and user-data
policy. Qualify lifecycle on disposable hosts rather than altering the maintainer's install.

Keep README/user guides product-oriented and developer rules canonical and linked.
Recheck affected GUI capability/settings gates and user journeys when behavior
changes. Review affected figures, error bars, units, legends and source/data identities
when charts change. Old measurements and golden renderer fixtures are not current
results; preserve the canonical mark and native product-text rendering system.

Critical changes beyond existing approvals require a concrete reviewed proposal
before execution: additional public format assignments, new application runtime
dependencies, platform expansion, destructive history changes or weakened gates.
Routine repairs and already-approved work proceed within their existing contracts.

### T01 — Portable Development Tools

Qualify Caveman instruction delivery, source-bound CocoIndex search, Crawl4AI
extraction and affected MCP/verification consumers through their canonical launchers.
Cover fresh and existing installations, absent/stale providers, moved checkouts,
supported configuration overrides and actionable access failures. Preserve valid
receipts rather than reinstalling unchanged tools at every checkpoint.

Use discovered paths and capabilities; do not track a maintainer-specific cache or
overwrite another task's registration. Treat site challenges as access limitations,
retain failed crawl receipts and use only permitted alternatives. Actual tool
defects require repair; a changed cache context is not evidence of a product defect.

Verify owned subprocess, pipe, cancellation and shutdown lifetimes. Leave no stale
workers, scheduled services or unbounded buffers, and do not interfere with unrelated
host work. Installation, an index marker or a resolver result alone is not proof of
actual tool use, runtime acceptance or complete analysis.

## Release Blockers

The following gates define what blocks a candidate. Determine their current
status from source-bound local evidence, the pushed revision and its hosted
checks. Completed checks may be reused only while relevant identities match.
Qualification and authorization to publish remain separate decisions.

The product and format migration uses FlitzZip and `.flzip` throughout active
interfaces, build targets, packages and tooling. Wire signatures and the MSI
upgrade identity remain stable. Legacy names are confined to compatibility
inputs and their tests, exact historical evidence, and verified build-cache
migration. The previous release and its tag were deleted at the maintainer's
request; this does not authorize publication of a replacement.

1. **Retain current source-analysis acceptance.** Qualify the exact pushed
   revision against open and reproduced dismissed findings. The authorized
   [individual false positives](docs/security-reviews/current-source-findings.md)
   and two exact governance observations remain bound to their reviewed identities.
   Repeat the affected detectors and exact-commit audit after further production
   changes; an earlier checkpoint does not approve later source bytes.
2. **Qualify current GPU execution.** Validate required-HIP behavior, bounded kernel
   progress, asynchronous ownership and failure cleanup on the candidate binaries.
   Preserve the historical incident evidence and its limitations in
   [beta readiness](docs/beta-readiness.md#outstanding-acceptance-limits).
   The maintainer has closed the unreproduced dictionary-corruption investigation
   as nonblocking after focused qualification; neither a driver cause nor a
   production repair has been established. Keep the distinct-input and start-schedule
   regressions active. Any new byte mismatch or hang reopens the affected release gate.
3. **Complete source and release qualification.** Review changed behavior and
   critical parser, extraction, codec, driver and kernel contracts; resolve
   identified defects and perform the required source and dependency audit.
   Qualify the final pushed SHA's
   workflows, audit, HIP-enabled ZIP/MSI, checksums and installer lifecycle before
   requesting release approval. Earlier successful artifacts cover their own
   source identities, not every later commit.

4. **Apply risk-based testing and coverage acceptance.** Require meaningful
   success, malformed-input, lifetime and recovery assertions for critical and
   changed behavior. Resolve demonstrated defects at their actual boundaries.
   Review the uncovered-line and unmeasured-source inventories under the
   [coverage policy](docs/code-coverage.md#release-acceptance), investigate
   regressions and retain residual risks. Missing critical behavior tests,
   failed required collection and unexplained regressions block release.
   CPU, GUI, tooling, HIP host and device measurements retain separate scopes.

## Approved Entropy Extension

Implement and qualify the [compact rANS design](docs/native-rans-extension.md)
for Neutron only. Version 12 and kind 14 are approved design assignments, not
currently emitted product representations. Preserve versions 1–11 and efforts
1–9, and include inner-stage kinds in writer and reader version admission.
Continue bounded HIP correctness and ownership qualification independently of
performance measurement. Derive candidate histograms from immutable transition
counts with exact segment resets, and reuse counts across precision choices.
Qualify normalization and model/stream construction independently before joining
the archive pipeline, then connect actual device-produced counts, models and
streams in the integrated proof. Bound source scanning without resetting
predecessors at dispatch boundaries, and reject partial statistics after
cancellation. Enable the production candidate only after independent
correctness and worthwhile complete-size evidence from representative workloads.
The shared-host restriction on local benchmarks remains in effect.

## Domain Work And Deferred Qualification

- **N02 — Neutron effectiveness:** the retained
  [public-corpus evidence](docs/neutron-public-corpus-benchmark.md) reports limited
  gains for its stated native source and workload. When measurement is permitted,
  follow with controlled paired CPU/GPU comparisons and independent workload classes.
  Preserve complete archive sizes and byte-exact readback; current evidence does
  not establish broad size superiority or a GPU speedup.
- **N01/N02 — Compression research:** evaluate the retained primary-source research before
  another broad crawl. Test GPU-suitable dictionary search, parsing, entropy and
  lossless model-data ideas, counting every model, dictionary and format byte.
  Keep Neutron GPU-only and separate from efforts 1–9. Adopt an idea only after
  independent correctness, malformed-input and representative size measurements.
  Current priorities are wider independent numerical contexts, batched GPU work,
  compact entropy representations and context modelling for text. Evaluate a small
  learned selector on held-out files only after collecting an exhaustive candidate
  oracle: count training/inference cost and every decoder/model byte. A classifier
  can order trials; it cannot justify dropping a competitive candidate in a
  minimum-size mode without a proven lower bound. Track native CPU, ZIP/Deflate,
  Gzip, Bzip2, Zstandard and LZMA-family results separately, including losses.
  Lossy model quantization is not a lossless archive technique.
- **C01 — Effort progression:** follow the complete-size contract above and
  [the effort specification](docs/compression-level-and-benchmark-suite.md).
  Extend its advertised scope only after the relevant format and resource cases pass.
- **D01 — Dependency lifecycle:** reconcile release and registry metadata against pinned
  sources, lock hashes and installed consumers. Upgrade action wrappers and the
  executables they install together when their contracts require it. Keep the
  Vulnetix action and CLI pins aligned with provenance verification. The signed
  installation and upload-help audit qualifies the offline interface; authenticated
  upload still requires a configured broker and authorized live scan. An action-only
  Dependabot change or passing offline audit does not qualify that live consumer.
  Preserve exact pins and distinguish unavailable live integration from passing tests.
  Qualify fresh installation and existing-installation updates with actual installed
  consumers; platform resolution alone is insufficient. Keep newer constrained
  parents visible, upgrade coherent graphs and keep one-off constraint exceptions
  exceptional. No floating requirements, untested prereleases or global host changes.
- **Q01 — Repository review:** continue risk-prioritized source and document review;
  an inventory is not proof of complete line-by-line review. Refactor demonstrated
  ownership, duplication, complexity or performance problems and retain direct
  consumers. Recheck frontend Neutron admission and interaction when their code
  changes. Preserve the existing AGPL-3.0 license, third-party notices and
  action-specific benchmark permissions.
- **U01 — Benchmark presentation:** retained visual checks qualify their own
  quantitative fixtures and diagram sources. Repeat affected checks when figures change,
  including uncertainty/error bars, units, scales and legends. Keep current
  measurements distinct from golden renderer fixtures; archive obsolete
  narratives locally without removing tests.
- **B01/B02 — Development efficiency:** optimize demonstrated build/workflow bottlenecks while
  retaining traced security analysis and release gates. Finish fresh-host and
  existing-installation tool qualification where evidence is missing. Keep
  canonical agent rules concise, crawler installation portable and finding
  prevention executable; do not add recurring maintenance for unchanged tools.

Reuse completed checks only when source, tools, configuration and outputs still
match. Resume from the retained local evidence and current GitHub audit rather
than repeating unchanged scans or benchmarks.
