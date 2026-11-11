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

Assessment source: published `main` at
`c2fd539a204df65745befad59d6991b75f7c7fc8`, with a matching clean development
checkout. At the assessment boundary, component contracts, lint, Scorecard,
Zstandard sanitizers and fuzzing passed; Windows CI, ROCm qualification and security
were still running. Their pending state is not a failure or a pass. The preceding
source `3706eae43b1f447a11512c6141bd782caa43d843` passed all eight of its Actions
workflows, including benchmark-graph. Its
[ROCm run](https://github.com/strmt7/FlitzZip/actions/runs/43034932616)
qualifies the configuration-diagnostic repair for that source. The original
[configuration failure](https://github.com/strmt7/FlitzZip/actions/runs/43010070857)
still has no established cause; a later pass does not explain it.

The live inventory confirmed only `main`, no open pull requests and no open
Dependabot alerts. DeepSource's check on this exact source was skipped because
its analysis quota is exhausted. Successful Actions do not replace that analysis
or the exact-commit findings audit. Recheck volatile service state at useful
publication and final-acceptance boundaries, rather than repeatedly polling it.

The counters-only observation integration spans telemetry, HIP event owners,
dictionary callers, output consumers and their tests. Its current HIP-enabled
Release build, fixed-data host cases, required-HIP readback/cancellation/recovery
cases and affected offline reporting contracts passed locally. Independent
compatibility and format-matrix checks cover their recorded binary identities.
Full native-driver execution remains deferred because that driver invokes
benchmark consumers prohibited by the active goal. Hosted checks cover the
published observation batch; the exact-source audit and final candidate
qualification remain outstanding. The published sparse-reader component has
bounded caller-owned workspace, eleven registered native tests, independent
fixed-payload and malformed-input controls, and differential/fault-control
evidence. Its differential control checked 2,073 cases: 1,222 admitted frames
decoded byte-exactly and 851 nonimproving frames were rejected. These results
qualify the reader's recorded scope, not the connected GPU entropy pipeline.
Its archive admission and GPU integration remain closed; current hosted
acceptance remains pending.
The earlier source `dec2dca` has a
[qualified ZIP/MSI candidate](https://github.com/strmt7/FlitzZip/actions/runs/42993879195);
those artifacts do not qualify subsequent source changes.

| Area | Verified state | Remaining problem or limit |
| --- | --- | --- |
| Product | Windows-native C++20 app, CLI, in-process adapters and HIP compute boundary | Exact methods differ by format; preserve the [capability matrix](docs/archive-format-support.md) |
| Native tests | New observation host and required-HIP cases pass, including independent readback and failure recovery | The broad benchmark-coupled driver remains deferred; bounded cases do not establish full release acceptance |
| Hosted/package checks | Five Actions workflows passed and three were pending on the assessment source; all eight passed on its predecessor | Final exact-source workflows, audit and ZIP/MSI lifecycle remain required; earlier packages do not qualify changed linked bytes |
| GitHub maintenance | Only `main`; no open PRs or Dependabot alerts at assessment | These inventories are live, not permanent guarantees |
| Analysis acceptance | Exact governance baseline accepted; raw reports retained | DeepSource skipped the current revision because its analysis quota is exhausted; current analyzer results and complete inventory are unavailable |
| Neutron | GPU-only search remains separate from efforts 1–9; independent readback and real device-work controls exist | Broad size superiority and comparative speed are unproven; approved rANS is inactive in production |
| Numeric efforts | Nested CPU frame search and GPU entropy selection have scoped contracts | Complete-archive monotonicity is not established across all writers, partitions and resource policies |
| Build generation | Shared probe output writing preserves unchanged bytes/metadata; actual source/header/bridge dependencies are registered and have canonical regressions | Work-count controls prove eliminated recompilation and targeted rebuilds, not a measured speedup |
| Observation | Published mode inheritance, optional event ownership, paired recording and structured status have local qualification and successful predecessor hosted checks | The broad local driver and final exact-source audit remain outstanding; measured remains the default and no speedup is claimed |
| Maintainability | Audit identifies large GUI, GPU, test and tooling files | Size is a review lead, not a defect or justification for mechanical splitting |
| Dependencies/tools | Pinned graphs have provenance and installed-consumer evidence; constrained parents remain explicit | Resolution is not runtime acceptance; universal upstream latestness is not established |
| Coverage/docs | CPU, HIP host, GUI, tooling and device scopes are separate; user/developer guides have canonical homes | Refresh affected gaps and claims, not a fictional whole-repository percentage; stale checkpoint prose must not drive execution |

This is an engineering assessment of inspected contracts and retained matching
evidence, not an exhaustive audit of every code line, dependency release or
document. Unreviewed behavior remains unreviewed. No new production defect is
established merely by listing an integration gap or an optimization opportunity.
The repository is not yet release-qualified.

### Problems, Inefficiencies And Uncertainty

These are distinct engineering categories, not a list of confirmed vulnerabilities:

- **Repaired observation contract:** the preceding count helper accepted measured
  launches without a duration, allowing a later sample to mask missing coverage.
  Measured recording now pairs launches with a duration/unavailable sample and
  rejects count-only misuse before mutation. The regression rejects the old
  behavior. No observed archive corruption is attributed to this accounting gap.
- **Profiling separation:** B02's counters-only mode avoids optional events and
  queries while retaining completion, deadlines and cancellation. Measured
  remains the default; the new operation contract adds no public mode switch.
  Fixed provider/work-count controls establish eliminated optional work, not a
  measured application speedup.
- **Reporting consistency:** the suite writer now uses the common GPU projection.
  Transfer-diagnostic admission rejects absent/incomplete timing status and
  missing actual-work evidence. Fixed-lane tests exercise the real consumer tail;
  actual timed diagnostic and suite execution remain deferred.
- **Expensive or conflicting verification entry points:** a broad unit driver
  also invokes benchmark consumers. Under the current restriction it cannot run
  unchanged. Select legitimate correctness consumers now; review separation of
  orchestration without removing benchmark coverage or declaring the full driver
  passed. Record every remaining required check.
- **Evidence drift:** older plans, continuation notes, build receipts and checkout
  snapshots can disagree. Use the actual `main` checkout, matching source/tool/
  output identities and one current local continuation. Do not repeatedly rescan
  unchanged inputs or treat an old successful build as current.
- **Overbroad evidence invalidation:** the dependency sanitizer receipt includes
  the whole native input projection. A late unrelated native-test edit therefore
  required another sanitizer qualification. Batch test edits before broad gates
  now. A future narrower projection needs demonstrated transitive completeness,
  mutation controls and a reviewed proposal before any mandatory gate changes;
  it must not be introduced as an exception to source freshness.
- **Compression opportunities:** integrated compact entropy coding, complete-size
  effort progression, duplicate candidate work, allocation and stream ownership
  merit investigation. Their presence on the queue is not proof of a defect or
  a promised compression gain. N01, C01 and C02 require concrete oracles first.
- **External analysis gap:** DeepSource quota prevents current complete analysis.
  Keep R01 visible, retain raw inventories and continue independent development.
  Do not contact support, pay, move accounts or reduce analyzer coverage.
- **Hardware and effectiveness limits:** the historical freeze has no confirmed
  cause; the unreproduced corruption incident has the maintainer's documented
  nonblocking disposition. Fresh mismatches or hangs reopen the affected gate.
  Broad Neutron size superiority and GPU speedup remain unproven.

### Priority And Decision Boundaries

| Priority | Work class | Decision |
| --- | --- | --- |
| Immediate | Any newly reproduced corruption, unsafe publication, hang, lifetime defect or incompatible dependency | Stop the affected path, preserve the first reproducer, repair its shared boundary and qualify recovery before continuing that path |
| Next ready | Correctness/benchmark orchestration separation and N01 GPU integration | Remove avoidable verification coupling without losing consumers; integrate bounded stages with independent oracles |
| Following | C01 complete-size progression, C02 CPU work reduction, Q01 critical gaps | Select demonstrated accounting, ownership or duplicated-work problems; do not undertake speculative blanket rewrites |
| External hold | R01 current DeepSource analysis | Resume on actual provider availability; no support request, payment, account move or analyzer reduction |
| Measurement hold | N02 representative size and CPU/GPU comparisons | No local corpus campaign or performance timing under the active goal; no new superiority claim or entropy-writer activation |
| Final candidate | Exact-source audit, applicable coverage review, package and installer lifecycle | Complete every applicable gate before requesting release approval; inactive research is not itself a defect in existing formats |

Dependency freshness is a separate inventory, not a conclusion from an empty
Dependabot list. Review runtime libraries, build tools, Actions and agent-provider
graphs independently. Preserve compatible pins until their replacements pass
actual consumers. Newly discovered fixes interrupt the queue according to their
impact; speculative opportunities do not continually reopen accepted work.

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

### Harness Execution And Resumption

The existing active goal supplies the current restrictions; this plan supplies
the queue and completion contracts. Neither is a second agent or service. Use
this serial loop in the current harness:

1. Read current user steering, this queue and the single local continuation.
   Reconcile the Git root, published head, working changes and owned jobs.
   Recover an existing job's result before relaunching it or changing its inputs.
2. Select the first ready package whose dependencies are met. State its invariant,
   direct consumers and independent success/rejection/recovery oracle. Reuse
   matching accepted evidence. A held external gate does not hold unrelated work.
3. Implement one coherent mechanism with its tests. Keep source immutable while
   its build or collector runs. Preserve the first failure, diagnose it, then
   qualify the correction under a new evidence identity.
4. Inspect `tools\verification_plan.ps1 -IncludeUntracked -Checkpoint intermediate`
   before invoking selected checks. Explicitly identify benchmark-producing
   commands and keep them deferred under the current goal; do not substitute
   an unrelated pass. Use exact selected native cases for development feedback.
5. Review the intentional diff, renew only substantively reviewed source-bound
   evidence where required, and publish qualified changes through the official
   connector with an expected parent. Never publish unfinished working changes
   incidentally with a documentation checkpoint.
6. Sample exact-SHA hosted work opportunistically once. Save pending/failed
   results and continue ready work. Update the package's durable status and the
   local continuation with source identity, evidence, remaining checks and the
   next action; put no changing counts or transcripts in skills.
7. At the final candidate boundary, apply all release criteria across the full
   accumulated change range. A package can be complete while overall release
   acceptance remains pending. Ask for publication approval only after the
   candidate qualifies; do not create a release or tag during development.

Package status means: **implemented** is source present; **qualified** adds the
required matching acceptance evidence; **held** names a specific unmet dependency.
Do not collapse these into a single misleading done marker.

For each active batch, the existing local continuation records: package/substep,
invariant and affected paths; source/tool/output identities; independent oracle;
observed failures and their disposition; terminal or still-owned job state;
completed, pending and deferred commands; published SHA and next concrete action.
Keep raw logs in ignored evidence directories. This plan owns durable scope and
decisions; skills own reusable instructions. Do not duplicate volatile evidence
into skills or install another harness, scheduler or background agent.

## Ordered Work Packages

The next bounded tooling slice is **T01 correctness orchestration separation**,
followed by **N01**, connecting the qualified sparse-reader component to the
approved GPU pipeline. **B00 and B01 are qualified
for their published scope; B02 is published with successful hosted checks.**
Its local broad-driver deferral and final audit remain visible. The ready sequence
is **T01 correctness separation → N01 → C01 → C02 → Q01**. If the tooling
separation cannot yet prove complete consumer retention, keep the broad driver
deferred and continue independent N01 work; do not weaken verification to unblock it.
D01 interrupts it for a demonstrated dependency vulnerability or incompatibility. T01 addresses
missing tool portability evidence without repeating valid installation checks. Resume R01 on
a provider state change; do not let that external blocker stall independent work.
N02 remains held by the measurement restriction. U01 follows affected product changes.

| ID | Work and current state | Dependency | Required completion evidence |
| --- | --- | --- | --- |
| B00 | Qualified diagnostic-loss repair; original failure cause unconfirmed | Preserve accepted evidence unless inputs change | Independent rejection/recovery controls and successful current ROCm qualification; retain historical uncertainty |
| R01 | Current analysis acceptance: quota exhausted | Provider allowance resumes and a current run completes | Successful required analyzers, complete paginated inventory and exact-commit audit; retain the accepted governance baseline |
| B01 | Qualified probe invalidation repair with local and hosted evidence | Revalidate only affected changed inputs | Old behavior detected; unchanged outputs preserve identities and avoid recompilation; changed inputs rebuild consumers |
| B02 | Published with bounded local host/HIP, reporting and successful hosted workflows; local broad driver deferred | Preserve source-bound results and final audit requirements | Real HIP counters without optional profiling; correct bytes, inheritance, failure cleanup and reuse; prohibited timing checks remain deferred |
| C01 | Complete-size effort progression: scoped review next | Existing nested-search inventory | Independent full-size/decoding regressions for every advertised guarantee; no padding or manufactured low-effort losses |
| C02 | CPU codec mechanisms: planned | Demonstrated work/ownership problem per codec | Independent read/write oracles and failure/recovery checks; supported capabilities unchanged; no unmeasured speed claim |
| N01 | In progress: canonical sparse parser/model workspace/CPU reader and independent controls; archive/GPU admission closed | B02 and approved format contract | Integrated device-produced models/streams, independent readback/CRC, malformed-input and lifetime controls |
| N02 | Representative Neutron effectiveness: deferred | N01 and lifted benchmark restriction | Complete-size evidence on licensed independent workloads, including losses and all model/framing bytes, before production adoption |
| D01 | Dependency lifecycle: alerts clear at assessment | Official metadata, compatible graph and provenance | Reproducible pins plus fresh/update installed-consumer tests; explicit unsupported/constrained platforms |
| T01 | Development tools: separate correctness orchestration first; retain qualified portable consumers | Complete existing command/consumer inventory and supported provider/host/checkout contracts | No benchmark reachable through the explicit correctness-only path; all existing consumers retained in ordinary/final selection; fresh/update/recovery and owned-process cleanup evidence |
| Q01 | Critical gaps and complexity: ongoing | Actual risk and source-bound gap inventory | Meaningful success/rejection/recovery assertions and repaired root causes; explicit residual behavior/measurement gaps |
| U01 | User experience, upgrades and docs: maintain affected contracts | Changed product/settings/package behavior | Migration and failure/recovery tests, disposable-host installer qualification, affected GUI/figure review and accurate docs |

### B00 — HIP Configuration Diagnostics

Scope: `cmake/ResolveHipArchitecture.cmake`, `tools/hip_architecture.ps1`, their
direct callers and existing configuration/architecture tests.

The resolver now retains its child result and both captured streams in failures.
Real subprocess controls cover nonzero stdout/stderr, malformed successful output,
missing commands and deadline expiry after valid partial output, followed by
canonical recovery. Both Windows PowerShell and PowerShell 7 passed these controls;
the original resolver fails the diagnostic-loss assertion. Target policy and the
existing deadline are unchanged. This repairs diagnostic loss, not the unconfirmed
cause of the earlier hosted configuration failure. Current published ROCm
qualification has passed; do not rerun this investigation without changed inputs
or a new failure.

1. Preserve the failing run, exact source, SDK/compiler identity and command.
   The observed failure precedes HIP compilation; it does not demonstrate a
   kernel defect, unsupported GPU or defective probe output.
2. Keep subprocess result, standard output and standard error together. The
   preceding fatal message lost that distinction; independent controls detect
   the loss. Establish a new failure's cause before changing admission policy.
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
The integration propagates an immutable observation mode through
shared, prefix, sparse and dictionary event owners, child attempts, snapshots,
aggregation and output consumers. This remains an internal operation contract;
no new public CLI switch is claimed. A successful build is not runtime acceptance.

Bounded host and real HIP tests now cover mode admission, literal aggregation,
completion/query failure, independent CPU/upstream readback, optional attempts
and Neutron cancellation followed by recovery. The paired-recording regression
fails on the preceding source and passes after repair. Transfer admission also
has a reproduced old-consumer failure and fixed-provider rejection/projection
controls. Preserve this evidence; do not describe the deferred broad driver or
timed consumers as passed.

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

Complete this batch in the following order:

1. Restrict the count-only recording primitive to counters-only operations.
   Measured completion must supply its duration or an explicit unavailable sample
   with the same launch count. Reject mismatched use before changing counters.
   Completion failure counts no completed launch; query failure preserves actual
   completed work and marks timing unavailable exactly once.
2. Exercise the host state table with independent literal expectations: no
   samples, observed zero, not requested, invalid and mixed/incomplete. Check
   aggregation associativity and child-mode admission. A missing requested
   duration must not become a valid measured zero.
3. Prove the counters-only event factory and query callbacks are never invoked.
   Use throwing providers and isolated behavioral mutations, not wall-clock
   comparisons. Keep stream completion, checked cleanup and cancellation active.
4. Run the new bounded RAM-only HIP cases: cached and deferred dictionary readback,
   cancellation followed by immediate Neutron recovery, and optional/required
   native GPU attempts. Require real device admission and byte equality against
   independent CPU/upstream decoding. A no-device return cannot qualify these cases.
5. Qualify changed structured consumers. Fresh measured records must reject
   missing/unknown/partial timing status; retained historical identity records
   remain readable without inventing timing fields. Keep those parser roles
   separate and retain their failure controls.
6. Run the remaining selected offline, interoperability, format-matrix and
   bounded correctness consumers on the matching build. Publish only after
   their applicable contracts pass. Record broad benchmark-coupled driver and
   measurement deferrals explicitly; refresh hosted acceptance for the new SHA.

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

1. Enumerate each enabled writer's effort mapping, partition policy, candidate
   ordering, budget and tie rule. Label guarantees by format and resource policy;
   do not extend a block-local result to a whole archive.
2. Build a small fixed regression matrix covering empty/tiny boundaries, repeated
   content, mixed blocks and already-compressed bytes. Compare complete serialized
   representations and independently restore every result. These are deterministic
   correctness fixtures, not a size campaign or timing exercise.
3. For each actual regression, trace the lost lower-effort candidate, framing
   change or accounting error. Repair shared admission/accounting rather than
   padding a result, weakening an effort or adding workload-specific exceptions.
4. Test deduplication with explicit candidate/work identities: identical work can
   be reused only when source, parameters, dictionary and decoding contract match.
   Preserve cancellation and memory bounds. Publish each proven scope precisely.

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

Work through a reader/writer inventory, not a blanket codec rewrite. For each
selected mechanism, record the current buffer owner, number of copies/allocations,
checksum pass and error/publication boundary. Choose a demonstrated avoidable
operation or ownership defect, then use deterministic work counters and independent
format readers to validate the change. Cover streaming split boundaries, empty
and truncated input, output capacity, cancellation and reuse after failure. Review
instruction-set dispatch separately from compression policy. Stop broadening a
batch when its consumers no longer share the same mechanism; no timing result or
cross-codec improvement follows from a work-count reduction alone.

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

First trace the canonical `unit-tests` descriptor in
`tools/flitzzip_verification.psm1` to its actual native driver and every child
command. Inventory correctness cases, interoperability, fuzz controls, benchmark
campaigns and measurement consumers before changing orchestration. Reuse the
existing runner and verifier rather than introducing a parallel test framework.

Add an explicit correctness-only execution contract only if the inventory proves
which commands it must retain and which benchmark producers it must decline.
Unknown or mixed-role commands must fail classification rather than run silently.
Use synthetic command recording to prove no benchmark can start, required
correctness consumers remain present, failed children fail the invocation, and
cancellation leaves no owned child alive. Default and final qualification must
retain every existing required consumer; the restricted path never implies that
held measurement gates passed. Qualify the real permitted correctness invocation
after the offline controls, using current source-bound binaries and no timing.

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

### N01 — Integrated Correctness Before Adoption

Execute these implementation batches in order. Keep public archive admission and
writer activation separate from private component qualification.

| Batch | Production boundary | Independent acceptance |
| --- | --- | --- |
| N01-A | Private HIP ABI/export/layout, component binding and complete device-entry admission; bounded GPU decoder consuming the canonical admitted payload/model views | Independent framed bytes, actual required-HIP dispatch and byte/CRC equality; absent or incompatible module rejection; malformed states, capacities, cancellation, failure cleanup and immediate recovery |
| N01-B | GPU transition counts, segment corrections and context projection bound to immutable stage bytes | Direct independent counts across all approved contexts/shifts and dispatch tails; disappearing contexts, zero predecessors, overflow/underflow and omitted/corrupted correction controls; reject partial statistics |
| N01-C | Normalization, full-width model construction and stream encoding consuming actual preceding GPU outputs | Exact positive model mass, singleton 65,536, deterministic ties, canonical model order, state/stream bounds and portable decoding of emitted streams; compare raw counts so normalization cannot hide an upstream error |
| N01-D | Archive/version/composition readers, resource admission, shared materialization and CRC; direct malformed-payload fuzz entry before public admission | Versions 1–11 preserved; kind 14/version 12 checked at every advertised and recovered inner stage; exact workspace/multiworker accounting, independent readback, interoperability and affected parser/fuzz qualification |
| N01-E | Neutron-only candidate portfolio, complete-frame lower bounds, winner ownership and payload-aware minimum-version propagation | Losing/tied trials preserve the previous winner and version; all model/framing/workspace bytes counted; efforts 1–9 unchanged; emission remains inactive until N02 qualifies adoption |

The reader component is already published; do not rebuild it as a second product
parser or treat private prototype successes as integrated device-model evidence.
Each batch qualifies its real connected consumers before the next stage relies
on them. A failed stage cannot publish statistics, advertise an admitted kernel
or silently retry CPU compression. Implementation of a writer is distinct from
permission to activate it.

1. Reuse the canonical `GpuRansBlockView` component and its registered native
   tests; do not introduce a second payload parser or portable decoder. Its
   explicit scratch requirement must enter caller resource accounting. Retained
   independent option-matrix and final-state-omission controls cover their source
   identities; no correctness fixture establishes a compression advantage.
2. Bind the existing private proofs to their actual source inputs. Trace device
   transition counts through normalization, model construction, stream emission,
   inner-stage admission and decode; do not substitute host-generated statistics
   for the claimed GPU-produced stage.
3. Integrate behind the approved inactive candidate boundary. Count complete
   model/stream/framing bytes and preserve versions 1–11, numeric efforts and
   ordinary required-HIP behavior. Do not assign another public kind/version.
4. Use an independent reference decoder and CRC/byte equality on bounded fixed
   fixtures. Cover empty and single-symbol models, segment/dispatch boundaries,
   skewed and mixed symbols, normalization limits and invalid model/stream data.
   Reject partial counts after cancellation and validate sizes before allocation.
5. Exercise GPU completion, checked release, failed-stage reuse and cancellation
   recovery without optional profiling. Add the direct malformed-payload fuzz
   target before opening public kind/version admission; run affected format and
   interoperability consumers.
6. Keep production emission disabled until N02 supplies worthwhile representative
   complete-size evidence. Passing synthetic correctness fixtures cannot justify
   adoption, a superiority claim or removal of a competitive existing candidate.

### D01, R01 And T01 — Maintenance Execution

These packages run at relevant boundaries rather than restarting on every turn:

1. **Dependencies:** capture current official release/registry metadata once for
   the graph being considered. Separate application runtimes, build tools, Actions
   wrappers and development providers. Identify constraints and consumer contracts
   before changing any pin; newer metadata alone is not upgrade acceptance.
2. **Upgrade qualification:** update a coherent pinned graph with provenance and
   licenses. Exercise fresh install and existing-install update through actual
   supported consumers, including rollback/recovery where applicable. Preserve
   unaffected installed environments and report unsupported platforms explicitly.
   Keep action wrappers and their installed executables aligned, including the
   Vulnetix action/CLI. Signed installation and upload-help evidence qualifies
   the offline interface; live upload still needs its authorized scanner/broker.
3. **Analysis:** on a genuine quota/provider change, obtain current complete
   paginated DeepSource occurrences using the existing inventory tool, reconcile
   counts and bind the run/configuration/source. Repair real findings in coherent
   root-cause batches; retain raw results and exact individual reviews. Until then,
   R01 stays held and other packages continue. An empty alert list is insufficient.
4. **Tool portability:** use canonical launchers for actual index and crawl use,
   installation/update/recovery and moved-checkout controls. Restore a stale index
   only when navigation requires it. Treat a site refusal separately from a
   crawler defect; do not reinstall a qualified provider to evade an access limit.
5. **Harness ownership:** inspect broad test-driver command roles. If correctness
   and performance orchestration need separation, retain both sets of consumers,
   source-bound receipts and explicit final selection. Test that a correctness-only
   invocation cannot start a benchmark. Do not create a second verifier framework.
6. **Cleanup:** prove cancellation, pipe draining/closure, bounded output and owned
   child termination through success and failure controls. Remove only task-owned
   temporary work; never stop unrelated processes or modify shared registrations.

### Q01 And U01 — Candidate Review Sequence

1. Inventory critical changed behavior and its tests: archive publication/parsing,
   CPU/HIP ownership, scheduling/cancellation, settings migration, GUI admission
   and package/runtime loading. Distinguish a reviewed contract from a filename
   inventory or an incidental coverage hit.
2. Add coherent success/rejection/recovery test batches using independent oracles.
   Investigate actual failures before broad refactoring. Collect the applicable
   coverage scopes after the batch stabilizes, review residual high-risk gaps and
   repair root causes with sibling-caller guards. Preserve raw missed lines.
3. Review user/developer documents against implemented behavior. Remove conflicting
   active instructions at their canonical source; retain required provenance and
   regression fixtures. Do not turn skills into dated status ledgers or delete
   evidence merely to produce a cleaner report.
4. Recheck settings schema/unknown-field preservation and direct supported upgrades
   when their producers change. Qualify portable/MSI payload parity and lifecycle
   on disposable hosts. Reuse only matching evidence, never an earlier package's
   pass for changed linked bytes.
5. Inspect affected GUI journeys and every changed graph against its actual data,
   units, scales, legends and uncertainty. Keep renderer fixtures separate from
   measurements; do not regenerate benchmark results while campaigns are prohibited.
6. Freeze the final source offering, run selected final workflows and exact-commit
   audit, qualify HIP ZIP/MSI/checksums/notices and installer lifecycle, then present
   the concrete candidate and residual limitations for the maintainer's green light.

N02 is a prerequisite for enabling the new entropy representation or making new
effectiveness claims, not a reason to describe unfinished research as a defect in
an otherwise qualified existing representation. Final acceptance still requires
every applicable existing release gate; a prohibited or missing required check
remains deferred, not passed. Optional research must not reopen unrelated accepted
contracts indefinitely.

## N02 — Measurement-Held Research

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

Reuse completed checks only when source, tools, configuration and outputs still
match. Resume from the retained local evidence and current GitHub audit rather
than repeating unchanged scans or benchmarks.
