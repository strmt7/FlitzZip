# FlitzZip Implementation Plan

## Current Milestone

Qualify a Windows 11 x64, HIP-enabled beta candidate, present its exact artifacts
and limitations for maintainer review, then wait for permission to publish.
Do not create a release or tag during development. The
[release criteria](docs/release.md#acceptance-criteria) and
[coverage acceptance policy](docs/code-coverage.md#release-acceptance) govern
acceptance; this plan does not replace or relax them.

Read [agent instructions](AGENTS.md) before execution. Keep one current ignored
continuation note with source identities, evidence paths and the next action.
Completed chronological reports belong in local artifacts and Git history.

## Current Assessment

**The repository is not yet release-qualified.** Component repairs and successful
published checks do not establish acceptance of accumulated unpublished changes
or of a final distribution.

The latest read-only GitHub assessment found one protected branch, `main`, no
open pull requests, and eight successful Actions workflows for published commit
[`dc1a5f0582b95bb389d658d9caef6f973e8b94fe`](https://github.com/strmt7/FlitzZip/commit/dc1a5f0582b95bb389d658d9caef6f973e8b94fe).
That snapshot does not qualify later edits, establish current analyzer or
vulnerability acceptance, or replace the final exact-commit audit. Refresh volatile
service state at publication and final acceptance, not through repeated polling.

| Area | Established evidence | Remaining acceptance limit |
| --- | --- | --- |
| Archive operations | Scoped independent readback, malformed-input, publication, Unicode, cancellation and recovery regressions | Review the accumulated patch and qualify current format consumers |
| Cancellation and completion | Adapter-specific original-failure, recovery and independent-format controls, plus current Release correctness-registry acceptance | Complete the registry-based boundary inventory and final package qualification |
| Test execution identity | Executable-bound HIP receipts, full/component driver acceptance and real-file PowerShell controls reject stale, missing, mismatched and unqualified executables | Preserve source/tool/output binding; historical results retain their original narrower artifact scope |
| HIP | Real-device correctness, cancellation, allocation/copy/dispatch cleanup and recovery controls for recorded inputs | Qualify affected current paths; compilation for six targets is not execution on every GPU |
| Coverage | Scoped CPU, HIP host, GUI, tooling and shared-header evidence | Review changed zero-hit behavior and unmeasured sources using matching identities; no whole-repository coverage claim |
| Distribution | Earlier ZIP/MSI parity and portable readback passed | Current production edits make those artifacts historical; rebuild once source settles |
| Installer | Authoring, version, location and recovery contracts have executable controls | Actual current install, repair, skipped-version upgrade, rollback and uninstall need a disposable Windows host |
| User experience | Earlier frontend Neutron admission/readback and reviewed captures passed | Final candidate journeys and affected pages require matching evidence |
| Analysis | Required gates and accepted governance observations remain defined | Security work is separately owned and deferred; retained publication admission is unresolved |
| Neutron | GPU-only behavior separate from efforts 1–9; private compact entropy components have independent controls | Public kind-14/version-12 emission remains inactive; integration and representative adoption evidence incomplete |
| Efficiency | Scoped ownership, bounded streaming and deterministic work-count improvements | No new comparative speed or broad compression superiority established |

The [compatibility contracts](docs/compatibility-stream-contracts.md) describe
qualified preparation, bounded payload, original-observer and publication
mechanisms across the repaired adapters. RPM now samples metadata, staging and
decoding while retaining decoder finish checks and CPIO publication. TAR/CPIO
emit the missing final entry notification. Current direct tests cover all five
RPM payload routes and the coupled TAR filters, CPIO, DEB, error and encoding
consumers. Historical runs remain bound to their own source and assertion scope.

The preceding native qualification passed 1,367 correctness cases and eight
selected CTest suites, explicitly deferring ten measurement cases and measured
corpus integration. Executable admission passed 54 provenance/receipt regressions
and focused live component/CTest checks. The subsequent transfer-stream repair
passed its complete 67-case affected cohort, including independent frames,
physical-byte counters, cancellation, cleanup and retry. Earlier broader results
retain their own source identities; later changes require their actual consumers.
These are scoped engineering results, not final candidate acceptance.

Retain distinct historical uncertainties: the original system freeze has no
confirmed cause; the unreproduced concurrent dictionary-corruption investigation
was closed as nonblocking by the maintainer with regressions retained. A new byte
mismatch or hang reopens the affected gate. Later success and a suspected driver
cause are not a diagnosis. An earlier PowerShell 5 analyzer crash also remains
unexplained despite passing isolated and PowerShell 7 controls.

## Release Blockers

Distance to release is measured by these unmet evidence milestones, not an
invented percentage, calendar estimate or universal absence-of-bugs claim.

1. **Review and freeze accumulated changes.** Complete Q01/U01, resolve demonstrated
   defects and classify remaining critical behavior gaps. Select commands for the
   complete intended range, including untracked files. Local evidence must match
   its actual source, tools and outputs.
2. **Complete current product qualification.** Run applicable correctness registry,
   CTest suites, independent interoperability and format matrix, real-HIP consumers,
   affected GUI journeys and required coverage review. Earlier full runs and current
   component runs qualify their own scopes only. Benchmark- or scanner-coupled
   commands remain deferred under current restrictions.
3. **Qualify exact distribution and lifecycle.** Build portable ZIP and MSI from one
   source, compare every declared payload and verify checksums, runtime admission,
   notices and corresponding source. Exercise installation, repair, supported
   direct/skipped-version upgrades, rollback, downgrade rejection and uninstall on
   a disposable Windows host. Do not install fixtures on the developer machine.
4. **Complete final admission and hosted acceptance.** Publish verified development
   increments through the authenticated official GitHub connector on `main` and
   verify contributor attribution and applicable hosted checks. Development
   publication is separate from release qualification. The candidate still needs
   separately owned security acceptance and the required exact-SHA post-push audit.
   Older green workflows do not satisfy this milestone.
5. **Present a qualified candidate for approval.** Supply artifact identities, source
   offering, supported scope, test/coverage limitations and residual risks. Request
   release green light only after all applicable criteria pass. Candidate
   qualification is not release-publication permission.

Security scans, finding remediation, disposition renewal, Dependabot and fuzzing
remain excluded from this agent's work. The last retained publication failure
was `Affected review has stale source or scanner context: 10227`; it has not been
rescanned in this round. Its separate owner must resolve it through existing
policy for final candidate acceptance. Deferred work must never be reported as a
passing scan, approval or release gate.

Benchmarks, corpus/size campaigns and performance-timing calculations remain held.
Ordinary fixed correctness fixtures and product diagnostics may continue. A
required but prohibited check stays deferred, never passed. Preserve default
measurement consumers and required final gates. The two accepted CodeReview and
CII Best Practices observations remain a pass under the existing
[governance baseline](.github/scanner-governance-baseline.json).

## Execution Order

Publish coherent verified engineering increments during these stages; do not
withhold all development publication until release qualification is complete.
Use the maintainer-authorized scope, retain explicit deferrals and preserve the
standing safeguards. Final candidate acceptance covers the resulting exact SHA.

| Stage | Work | Exit evidence |
| --- | --- | --- |
| 1 | Finish coherent repairs and inspect siblings | Original failure, independent regression sensitivity, successful direct consumers and reviewed source diff |
| 2 | Q01/U01: accumulated behavior, coverage and documentation review | Critical inventory reconciled; defects repaired; remaining gaps and affected journeys explicit |
| 3 | Freeze inputs and prepare current engineering packages | Source/tool/output identities, exact ZIP/MSI parity and portable runtime/readback |
| 4 | Disposable-host lifecycle and separately owned admission | Actual lifecycle logs and required owner acceptance; no prohibited check relabelled |
| 5 | Publish admitted source and qualify final SHA | Natural contributor identity, required successful hosted checks and final audit |
| 6 | Assemble final candidate and ask for green light | Every applicable criterion satisfied; concrete artifacts and limitations ready for inspection |

Order dependencies by actual consumers. A source change invalidates affected
binary evidence; documentation-only edits do not automatically invalidate unchanged
codec execution. Final artifacts still need current docs and corresponding source.
Do not rebuild, recollect coverage or rerun unchanged suites merely because a
new development turn began.

Use the existing [verification planner](docs/targeted-verification.md), inspect
command roles and run admitted consumers through canonical bounded executors.
Offline contracts must not silently rebuild native targets. Default broad
commands can include measurements or scanners: inspect them before execution.
Retain explicit omissions instead of creating a parallel passing framework.

## Next Coherent Round

The next round follows the remaining critical path rather than restarting every
previous investigation. These are evidence milestones, not a release date or an
estimate of implementation completeness.

| Priority | Batch and dependency | Concrete completion condition |
| --- | --- | --- |
| 1 | Reconcile Q01/C02 against the format registry and accumulated patch | One inventory connects each affected operation's preparation, payload, finish, notification and publication boundaries to current regressions or an explicit gap; demonstrated defects receive grouped root-cause repairs |
| 2 | Qualify critical behavior and coverage using that inventory | Reuse the matching native/CTest results; complete remaining independent format consumers, real HIP and affected GUI journeys; investigate changed critical zero-hit behavior |
| 3 | Freeze one candidate and qualify its distribution | ZIP/MSI payload parity, checksums, runtime admission, notices and corresponding source match; a disposable Windows host passes install, repair, supported direct/skipped-version upgrade, rollback, downgrade rejection and uninstall |
| 4 | Complete separately owned final admission and hosted acceptance | Development increments are published on main with natural attribution; final candidate admission, required exact-SHA hosted checks and audit pass |
| 5 | Present the candidate for maintainer inspection | Artifact/source identities, supported scope, remaining limitations and every acceptance result are reconciled; ask for green light without creating a release or tag |

N01's private extension work can proceed only after its prerequisites are met;
public kind-14/version-12 emission stays inactive. N02's representative size and
speed adoption evidence remains prohibited in the present round. Do not substitute
fixed correctness fixtures for that study, count it as complete, or make an
unmeasured compression claim. Active release behavior must qualify independently
of unfinished research.

## Q01 And U01 — Candidate Review And User Experience

1. Map the complete patch to production contracts: detection/formats, publication
   and overwrite, CPU/HIP ownership, cancellation/scheduling, settings, GUI state,
   installer and runtime loading. Separate confirmed defects, untested behavior,
   retained incidents and optimization hypotheses.
2. Batch meaningful boundary tests before coverage recollection. Use independent
   bytes/readers, original-source failures or isolated mutations where needed.
   Assert success, rejection, partial progress, first-error preservation, cleanup
   and immediate reuse. Inspect sibling adapters, not just the observed format.
3. Reconcile zero-hit lines and unmeasured sources by actual component. Keep CPU,
   HIP host, GUI, tooling and device scopes separate. Repair missing critical tests
   and invalid collection; review lower-risk gaps with concrete evidence. Never
   fabricate hits, remove defensive code or suppress paths for a percentage.
4. Review large modules by ownership and responsibility. Share common mechanisms
   and remove proven orphans; preserve format-specific contracts and write batching.
   Do not split files mechanically to satisfy a metric.
5. Preserve external settings keys, unknown fields and append-only historical
   fixtures under the [persistence contract](docs/development/persistence-and-upgrades.md).
   Test migration, failed writes, recovery and supported skipped-version paths.
   Do not promise compatibility with unknown future implementations.
6. Check capability/settings gates, queues, create/extract/verify, cancellation and
   persistence. Announce necessary GUI testing and review every capture; never
   build or package while that GUI owns outputs.
7. Keep README and user guides task-oriented, with developer material linked from
   the [developer guide](docs/development/README.md). Review affected charts' units,
   bounds/error bars, legends, data and source identities. Old plots and golden
   renderer fixtures are not new measurements. Preserve canonical branding.

## C01 — Complete-Size Effort Contracts

Inventory each advertised writer's effort mapping, partition policy, resource
budget, candidate order and tie rule. Distinguish block savings from complete
framing, dictionaries, indexes and composition. Retain a lower effort's winning
representation wherever identical format/resource constraints permit it. Do not
pad output, degrade lower efforts or promise inverse-linear size on arbitrary data.
Keep Neutron outside numeric progression.

Existing native fixed-fixture regressions independently count complete framing,
restore bytes and detect discarded lower-effort winners. They cover recorded
block/worker policies, not all compatibility writers. Extend the deterministic
matrix for an uncovered advertised contract or actual regression: empty/tiny,
repeated, mixed and already-compressed data. Trace lost candidates, framing and
accounting separately. Reuse work only with matching source, parameters, dictionary
and decode contract; preserve bounds, cancellation and deterministic ties.
Representative size and speed effectiveness remain N02 work.

## C02 — CPU Codec And Streaming Efficiency

Use the registry and codec inventory to review every enabled reader and writer,
including method-specific decoders. Do not add writers to extract-only formats.
Record ownership, copies, allocations, retained capacity, checksum passes and
publication/finish requirements for each selected mechanism.

Native candidate ownership, shared Gzip/Bzip2/Zstandard/XZ/Lzip windows,
TAR/CPIO/ZIP boundaries and stored/native-library cancellation have scoped repairs.
Review remaining mechanisms against concrete hypotheses; an extra copy or large
file alone does not establish a defect or end-to-end loss. Preserve batching and
pinned source ownership. The LZMA dictionary-to-output path needs particular care
before changing batching/termination. LZ4 already has a direct-buffer lane.

RPM preparation now has original-source reproducers across all five supported
payload routes: independent byte-validity controls pass while metadata, staging
and decoded-window cancellation fail before repair. Separate regressions also
reproduce missing final entry notifications in TAR/CPIO and their RPM consumer.
The repair preserves buffer sizes, decoder finish checks and private cleanup;
its complete affected cohort passes in Debug and Release. The first partial-window
observation required correction from cached directory sizes to fresh active-file
handle sizes. Retain those failed observations and their independent Windows
control rather than treating them as production failures. Scope-specific coverage
review and final candidate qualification remain separate requirements.

Before package generation, reconcile preparation, bounded payload work, final
validation and completion notifications against the format registry in one
remaining-consumer inventory. Batch any demonstrated sibling defects and their
tests; do not restart already qualified mechanisms without new evidence.

Validate with independent readers and deterministic work/ownership controls:
split boundaries, empty/truncated data, output capacity, allocation failure,
cancellation, finish validation and reuse. Review instruction-set admission
independently from codec policy. No machine-specific global flags or crash-based
dispatch. A work-count reduction is not a measured speedup.

## Approved Entropy Extension

The [compact rANS design](docs/native-rans-extension.md) assigns version 12 and
kind 14 to Neutron-only development. **Public archive admission and emission
remain inactive.** Preserve versions 1–11 and efforts 1–9. Private qualification
is not integrated archive or representative adoption qualification.

### N01 — Integrated Correctness Before Adoption

| Batch | Boundary and next work | Required acceptance |
| --- | --- | --- |
| N01-A | Preserve qualified private HIP ABI/module admission and bounded segment decoder | Independent frames, real device dispatch, byte/CRC equality, missing/incompatible module rejection and lifetime/cancellation recovery |
| N01-B | Preserve transition statistics, context projection and immutable counts across precision trials | Exact counts/resets, dispatch tails, disappearing contexts, overflow/underflow and corrupted correction rejection |
| N01-C | Qualify connected GPU model normalization/construction/encoding and bounded host ownership | Exact model mass including singleton 65,536, deterministic ties/order, stream bounds, independent decode and failed-stage reuse |
| N01-D | Integrate archive/version/composition readers, resource admission and materialization after current component acceptance | Versions 1–11 unchanged; inner kind/version checks; complete workspace accounting, readback and required parser/fuzz qualification |
| N01-E | Integrate Neutron portfolio, lower bounds and winner ownership after reader qualification | Count all framing/model bytes; losing/tied trials preserve winner/version; numeric efforts unchanged; emission held pending N02 |

Use canonical admitted payload/model views, not a second product parser. Preserve
allocation/copy/submit/completion/release fault controls, child-attempt terminal
state, bounded dispatch and rejection of partial statistics after cancellation.
Do not reset predecessors at dispatch boundaries. Propagate minimum versions
through recovered inner stages. N01-D's security/fuzz consumers remain with their
separate owner; private success cannot waive them.

### N02 — Representative Effectiveness And Research

Resume only when measurement is permitted and candidate correctness qualifies.
Retain the [benchmark index](docs/benchmarks/README.md) and
[public-corpus study](docs/neutron-public-corpus-benchmark.md) with actual source,
workload, losses and limits. Neither establishes universal size superiority or
comparative GPU speedup.

Evaluate content-derived static byte-context partitions first, then bounded
integer prediction and explicit match/residual modelling. Use retained
[candidate research](docs/neutron-star-mode-research.md#deterministic-model-candidates)
before another broad crawl. Consider GPU-suitable dictionary search, parsing,
batched entropy work and lossless model-data representations. New runtime
dependencies or additional public format assignments require reviewed approval.

Use licensed independent workload classes, held-out files and RAM-only transport.
Count all archive, model, dictionary and framing bytes; independently restore
every result. Compare equal efforts and actual HIP execution. Separate complete
size, descriptive timing and controlled CPU/GPU comparisons; include losses.
A learned classifier may order trials but cannot discard a competitive candidate
in a minimum-size mode without a proven bound. Count training/inference and
decoder/model costs. Lossy quantization is not a lossless archive technique.
Transfer Neutron work to numeric efforts only after proving both smaller complete
output and faster execution under relevant supported policies.

## B00, B01, B02 And B03 — Build And Observation Contracts

| ID | Preserved qualification | Reopen only for |
| --- | --- | --- |
| B00 | Resolver retains child status and both streams; nonzero, malformed, unavailable and deadline controls | Changed resolver/toolchain consumers or new failure; original hosted failure cause remains unconfirmed |
| B01 | Generated probes preserve unchanged bytes/metadata and rebuild changed dependencies | Changed generation inputs or unnecessary work; no unverified compiled cache or unmeasured speed claim |
| B02 | Counters-only mode propagates through host/HIP ownership and output consumers without optional event/query clocks | Changed consumers, timing-state errors, completion/cleanup failures or new counter mismatch |
| B03 | Executable-bound receipts, full/component admission, CMake declaration contracts, stale/mismatched rejection and current HIP recovery | Changed build/receipt/execution consumers, changed executable declarations or a new artifact mismatch |

Preserve not-requested, measured and incomplete timing states. Failed completion
counts no completed launch; failed elapsed query retains actual work and
unavailable timing. Fixed clocks and throwing optional event/query providers
qualify counters-only behavior without performance calculations. Keep deadlines,
completion waits, checked cleanup and real device admission. Default measured
consumers remain registered and deferred when prohibited.

## D01, R01 And T01 — Dependencies, Analysis And Portable Tools

- **D01:** review runtimes, build tools, Actions and agent providers as separate
  graphs at useful dependency boundaries. Capture official metadata, constraints,
  provenance and licenses once per graph. Upgrade compatible pins after fresh/update
  installed-consumer and recovery tests. Empty Dependabot results do not establish
  freshness. Vulnerability work stays with the separate security owner this round.
- **R01:** preserve source/analyzer acceptance and the exact governance baseline.
  The last retained DeepSource state was quota exhaustion; current eligibility
  was not rechecked here. On an actual provider/owner update, use the canonical
  paginated inventory, reconcile counts and bind exact source/run/configuration.
  No support request, payment, account move or analyzer reduction is authorized.
- **T01:** use canonical launchers for mandatory Caveman, actual CocoIndex queries
  and admitted Crawl4AI extraction. Preserve failures, receipts and checkout/provider
  binding. Refresh indexes when navigation needs them; distinguish site refusals
  from provider defects. Do not replace another task's registration or repeatedly
  reinstall a qualified provider.
- **Harness ownership:** preserve correctness roles, complete CTest role admission
  and default measurement registration. Retain controls proving correctness-only
  execution cannot start benchmarks. Bound owned children, pipes, output and cleanup;
  leave unrelated host work untouched.

Native test freshness is now part of the existing receipt and execution mechanism:
CMake declares executable targets, version-two receipts bind default-built outputs,
and component/full drivers request exact artifact admission before execution.
Real-file tests cover stale/missing artifacts, target/path/configuration mismatch,
excluded outputs, historical-receipt migration and both Windows PowerShell hosts.
The CMake correctness contract verifies nested declarations, target/directory
exclusions and actual tiny-build outputs without launching fixtures. Preserve
tests-disabled build roles, incremental reuse after migration, default measurement
registration and current full/component consumers. The live product build passed
with matching broad-run output digests; fixture success alone cannot
qualify future changes. The CLI uses one configuration option, rejects explicit
mismatches and derives the current receipt configuration only when execution
requests omit it. Earlier functional results remain scoped evidence of what ran
with their original artifact binding.

## Batch Decisions And Resumption

Every coherent pass ends with one decision: retain a demonstrated repair or
improvement with matching evidence; reject a hypothesis with evidence; or name
its unmet prerequisite. Reopen only when source, consumer or external evidence
changes. Research resolves named decisions; avoid speculative rewrite loops and
repeated unchanged checks.

N01/N02 and remaining C01/C02 work retain their scope. Inactive research does not
make an existing representation defective, and release approval does not complete
the broader optimization goal. A demonstrated defect interrupts the affected
path; speculative opportunities do not reopen unrelated accepted contracts
indefinitely. Reconcile this plan with actual candidate evidence and every
applicable release criterion before final review.
