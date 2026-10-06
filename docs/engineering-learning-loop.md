# Engineering Learning Loop

## Focused Iteration And Failure Handling

On 3 October 2026 the maintainer explicitly directed component-specific
verification and batching instead of repeated unrelated suites. A failed check
does not by itself establish a global defect: diagnose the failed mechanism,
repair it and rerun its affected contracts. The verification runner now stops
at the first failure and preserves its original error rather than automatically
launching the full profile. Controlled first-command, later-command and manual
failures exercise the actual runner without building or launching the product.
Explicit `-Full` remains available for demonstrated wider impact.

The next routing batch replaces blanket tooling/skill and path-count expansion
with component/dependency patterns. Unknown paths fail planning for review.
Inclusion and exclusion regressions cover receipt producers versus fixtures,
compiler helpers, agent tools, MCP containment, scanner contracts and explicit
broad coverage. Toolchain helpers already invoked by the native test driver are
not repeated separately. Preserve scanner failures and pending hosted checks;
a focused pass must not be described as complete product requalification.

SuperZip records concrete engineering mistakes as enforceable repository
invariants. The goal is not more process; it is to convert verified failures
into small checks that prevent the same regression from reaching GitHub Actions,
the Security tab, released artifacts, or the product UI again.

## Current Guardrails

- Portable host admission must remain linkable in every supported build
  configuration. A device-dispatch regression must not call an optional HIP
  implementation from a CPU-only test target. Keep one canonical host validator;
  exercise that validator on CPU-only hosts and the real dispatcher on HIP hosts.
  Preserve the device assertions rather than substituting CPU fallback. Validate
  the actual test executable link and its consumer in both configurations when
  moving this boundary. The [development review](final-development-review-2026-10-05.md)
  records the linker failure and its repair.

- Independent public-fixture approval cohorts must not change earlier source
  commitments. Authenticate the complete current and historical archive, exact
  member, match, reported location, source location and scanner identity. Bound
  decompression before parsing archive metadata; retain every original finding
  and reject redirects, ambiguous names, mixed findings and scanner errors.
  An approved public example does not authorize source-code suppression.

- Select C++ database reconstruction from its complete native input projection,
  source suffixes and query/build policy, including removals and both sides of
  renames. Keep each selected CodeQL database whole and its query suite intact.
  Scheduled, manual and initial-push scans remain unconditional. Missing history
  fails planning; failed planning or missing/malformed output requests full C++
  analysis. Other security jobs continue on every push. The offline contracts
  exercise the real Git ranges, receipt projection and workflow consumer, so
  document or unrelated tool edits do not habitually retrace an unchanged build.

- Binary benchmark transport must not inherit text encoding or treat compressed
  payload bytes as restored input coverage. The native protocol's
  `validated_bytes` proves complete readback; `output_bytes` counts compressed
  payload and must fit its complete framed archive. Admission controls cover
  both compression and expansion. The actual stdin consumer now runs under a
  preamble-capable ambient encoding and sets its owned writer to omit a
  preamble. Hyperfine forwards bounded worker diagnostics; failed and partial
  studies remain unqualified. Natural corpus runs validate the actual consumer
  in addition to mocked protocol contracts.

- Corpus metadata checks must use the same detected backend as their readback
  check. A hosted CPU-only runner must not request Neutron and then interpret
  its correct HIP refusal as an input-admission failure. The offline transport
  contract executes the actual corpus consumer with both capability states and
  rejects the original unconditional Neutron request. Independent native plan
  tests still require Neutron to refuse unavailable HIP; no fallback is added.

- Initial feature-branch pushes have no previous branch commit. Treating that
  event as a change to every inherited file caused an unrelated component-plan
  failure on a dependency PR. The production tool now computes its merge base
  with the default branch. Real Git fixtures require exactly the changed paths,
  reject inherited files and fail on unavailable ancestry. Initial publication
  of the default branch keeps complete tracked-file coverage.

- A composite Zstandard migration fixture exceeded its aggregate deadline
  although its individual operations remained bounded. The shared outer test
  budget now accommodates the complete matrix, with explicit extraction and
  rejection-child deadlines. Mutation controls reject missing child bounds;
  the actual ASan suite, instrumentation negative control and migration cases
  remain required. The [incident record](neutron-stability-investigation-2026-10-05.md)
  retains the original timeout and successful qualification separately.

- An upstream SSRF regression fixture triggered the URI secret detector on a
  decoy prefix. The maintainer individually approved the exact public test
  after original-source and detector review. The canonical redaction boundary
  now retains every sanitized result and emits separate adjudication counts,
  bound to the historical Git blob, current archive, complete member, exact
  match, location and scanner image. Real Git/archive/Bash controls reject
  changed identities, mixed findings and scanner errors. Useful adversarial
  tests and immutable archives remain intact; historical source approvals are
  not revived. See the [individual review](security-nltk-ssrf-fixture-review-2026-10-05.md).

- Source review admissions were mistaken for remediation. The source preflight
  now counts every exact historical match as unresolved, and the hosted audit
  never consults the admission ledgers. Its complete inventory blocks every
  open source or governance alert. Regression cases include formerly accepted
  source and Scorecard records, pagination, mixed producers and malformed
  admission outputs. Historical review text remains explicitly historical.
  The [redesign record](security-source-remediation-redesign.md) tracks the
  production boundary changes and required source closure evidence.

- The SDK follow-up exposed three undocumented inherited parser routines.
  The changed-function audit skipped vendor source and recognized only
  same-line C/C++ braces. It now admits edited vendor source, recognizes
  both brace styles, masks comments/literals, requires every contract field,
  rejects borrowed contracts from preceding functions and deletion-only
  contract removals, and checks long-body
  documentation. Negative controls cover these failures. The actual folder
  and header parser is split into bounded stages with documented ownership;
  production decoder cases retain malformed-header rejection and exercise
  file-map bitmap rollover with nine files/directories.

- The 7z header parser has a reviewed native component cohort covering every
  registered 7z parser and PPMd consumer. Its only product caller is the 7z
  adapter; the folder decoder is its in-library consumer. Shared SDK headers,
  decoder primitives or unmapped native inputs retain the full driver. Selector
  contracts check both boundaries. Normal local builds remain HIP-enabled;
  selecting a CPU parser cohort does not select unrelated GPU timing work.

- The 54 historical hosted dispositions retain every incident field under a
  fixed integrity digest. Their informational matching controls reject source,
  caller, scanner and location changes, altered decisions, missing records and
  expanded inventories. These controls preserve review provenance; they do not
  admit any finding. Raw results remain intact and every open report requires
  its actual remediation or governance control.

- The generic offline-contract name heuristic admitted the native ASan project
  and repeated completed qualification during a tooling-only continuation.
  `Test-SuperZipToolContractCommand` now identifies that command as native work.
  The dedicated `zstd-sanitizers` workflow retains all six instrumented targets
  and source-bound receipts. Selector tests require affected inputs to select
  and trigger that workflow, and reject unrelated tooling/docs in both local
  selection and hosted filters. This separates execution roles without deleting
  sanitizer coverage or treating an unchanged-source result as a fresh run.

- Historical source matching binds normalized whole-file bytes and complete
  rule/region identity for the 21 Zstandard/control and six SDK locations. Its
  provenance controls reject changed bytes, coordinates, rules, multiple
  locations, redirects, malformed policy and broader source paths. Even an
  exact match counts as unresolved. Source ledger changes select the relevant
  contracts locally and in CI; no directory or scanner rule is removed.

- Publishing unscanned benchmark receipts caused thousands of public-checksum
  alerts and a failed Gitleaks gate. `tools/scanner_preflight.py` now freezes
  changed tracked and untracked inputs and runs the same pinned detectors before
  expensive verification. It fails on findings, missing/mismatched tools,
  invalid output, resource limits and source mutation. Reports remain private
  and complete; code findings are never filtered. This adapts bounded admission
  and explicit evidence patterns inspected in the maintainer's scanner and
  skill-review repositories, without importing their blanket test policies.

- Logger entries do not establish completed calls. The production HIP trace
  contained unpaired empty status-query entries matching AMD's direct-return
  logging pattern. `analyze_hip_trace.py` preserves strict parsing by default;
  an explicit narrow opt-in retains these records with unknown duration,
  status and unique-call count. It never fabricates timing or overlooks other
  missing returns. Parser contracts run once through the canonical component
  selector, independently of lint and native benchmark execution.

- Narrative benchmark reports do not change graph inputs. A report-only push
  exposed a planner requesting `benchmark-graph` despite its data-only event
  filter. Routing now selects graph contracts for `docs/benchmarks/data/` and
  existing producers/policies, while report prose retains hygiene and lint.
  Inclusion/exclusion regressions preserve nested corpus-pin acquisition and
  graph checks. Missing historical runs remain missing; this correction does
  not relabel them as passed or weaken data publication admission.

- Test doubles must preserve the production CLI's exit contract. The first
  hosted CPU component invocation rejected an honest HIP-OFF binary because
  its guard expected exit 0 from `gpu-info`; the actual CLI returns 1 when
  unavailable. CPU validation now requires exit 1 and both false state fields,
  with regressions rejecting unexpected success/error statuses. Required-HIP
  validation remains exit 0 with compiled and available state. Actual hosted
  integration remains separate from passing controlled command fixtures.

- Source registrations are not necessarily compiled registrations. A sparse
  device-admission test was registered only under `SUPERZIP_ENABLE_HIP`, although
  CPU-only CI selected its exact name from source. Its registration now exists
  in both builds, with configuration-specific assertions inside the body. The
  inventory rejects unknown registration conditions and preserves supported
  positive Windows guards. GPU assertions remain explicitly unqualified in CPU CI.

- Offline test fixtures must create and clean their own temporary directories.
  The hosted native-routing contracts initially passed locally but failed on a
  fresh runner because their output fixture assumed ignored `out/` already
  existed. The fixture now owns a unique directory under the system temporary
  root; production GitHub output handling remains unchanged.

- A fixture-created artifact is not proof that a fresh production build emits
  it. The first native-receipt integration passed local HIP checks but failed
  both hosted CPU-only builds because CMake generated runtime metadata only for
  HIP. One production writer now generates truthful CPU/HIP manifests, with
  direct configure-time regressions and hosted validation of actual build
  receipts. CPU builds retain app-local dependencies and declare HIP disabled;
  release packaging still requires HIP. Do not fabricate prerequisites or relax
  receipt checks to make a missing artifact pass.

- A complete measurement journal does not prove that final report export
  succeeded. Confirmation and pilot records now use one frozen-identity
  exporter, exercised with and without pilots through the production caller.
  A removed local variable survived in the former pilot-only branch and caused
  export to fail after 176 observations. Preserve the original failed exit and
  journal; label any recovered report, retain every observation, and independently
  compare its values and statistics rather than rerunning or silently relabeling
  that failure as a successful controller run.
- A complete SDK migration must align compiler, headers, libraries and
  dependency scans on one explicit root. Distribution and component versions
  are recorded separately; bundled components are never individually patched.
  `test_rocm_toolchain.ps1` verifies compiler-environment restoration and hosted
  policy, while `test_bootstrap_rocm_sdk.py` exercises corrupt archives, changed
  caches, timeouts and preservation. Compiler-only settings must not leak into
  application execution; a newer SDK's `LLVM_PATH` caused a reproducible runtime
  access violation on one driver configuration despite successful compilation.
- Statistical uncertainty must preserve fractional measurements and remain
  invariant when timing units change. Windows PowerShell overload selection can
  turn `Math.Max(0, doubleVariance)` into an integer calculation; use the typed
  floating-point boundary. Reporting regressions require nonzero fractional
  sample deviation, scale-invariant CV/RSE and variance-driven pilot counts.
  Corrections retain raw observations and disclose invalid earlier plans rather
  than trimming samples or appending post hoc confirmations.
- Development context must expose partial reads and stale synthesis instead of
  quietly treating either as current knowledge. The bounded reader caches only
  fully emitted exact windows in an explicitly retained context. Source mutation,
  a new context or forced reads restore the source; note expiry and changed or
  unavailable dependencies withhold the summary. Real-file regressions cover
  those cases, collision preservation and Windows case-insensitive secret-path
  admission. See [development context](development-context.md).
- A mandatory tool's installation does not prove its use. Startup delivers the
  current Caveman/CocoIndex instructions, while only successful fresh semantic
  searches publish a routing receipt. A source mutation during retrieval fails
  before output and leaves previous evidence historical. Instruction delivery
  and source freshness do not prove model compliance or complete review.
- Freeze a coherent source batch before full verification. In the October
  context-tool round, a late Windows path refinement required another full run;
  earlier passing checks could not establish acceptance of the final source.
  Use focused failure diagnosis while editing, then the actual-path classifier.
  Record failures, pending gates and exact evidence rather than narrowing the
  mandatory profile to save time. Summarize saved raw logs before emitting them
  to the model, and continue required reads through explicit unread ranges.

- Reduce scanning backlogs by root-cause batch, not alert-count targets. Separate
  code defects, quality observations and verified false positives; retain an
  alert-to-evidence record and confirm actual fixes on the pushed SHA. An upstream
  borrowed stack pointer is not a dangling pointer when all success/error paths
  join its users before destruction. Likewise, a provenance digest is not a
  credential just because a token-pattern rule matches it. Neither observation
  authorizes blanket dismissals, source filtering or rewriting correct code to
  satisfy a heuristic. Preserve unresolved paths as review work and continue
  independent product improvements instead of repeating unchanged scans.
- Decoder constructors must return a complete owner or release every partial
  allocation. A version transition must initialize its replacement before
  destroying the old owner and publish pointer/version identity together only
  after success. The Zstandard legacy regressions exercise both allocation
  failures, every shipped transition, dictionary errors, retry and destruction
  against the production dependency source, not a copied decoder.
- Downstream dependency fixes must survive fresh archive extraction and reject
  source drift. The Zstandard patch pins complete original/derived file hashes,
  explicitly preserves LF bytes on Windows and verifies them before atomic
  replacement. CMake fixtures cover repeat configuration and preservation of
  interrupted input/output files. Upstream provenance and licenses stay intact.
- Header-guard remediation must preserve intentional re-inclusion contracts.
  Zstandard separates public and static-only APIs; xxHash additionally supports
  public-then-inline inclusion. Do not blanket-wrap these headers or remove
  assertion-only helpers because a Release build erases their callers. The
  isolated C/C++ regression proves the original private-header redefinition,
  repaired repeated inclusion, and unchanged late opt-in before accepting the
  two private guards. Quality recommendations alone are not vulnerability proof.
- Whole-language lint expansion must retain newly added changed files instead
  of replacing the change set with tracked files alone. CMake routing covers
  owned fixtures and nested lists throughout the repository; production-selector
  regressions cover changed, all and configuration-triggered modes without
  duplicating the routing policy.
- MCP command trees need aggregate committed-memory containment, not merely
  a per-process output limit or sampled RAM allowance. Regression tests query
  the actual child job and prove that two individually admissible allocations
  cannot exceed their shared ceiling. Suspended launch prevents descendants
  escaping before assignment, and injected assignment/resume failures prove
  fail-closed cleanup. Pure admission tests match the PowerShell policy;
  nonfinite deadlines cannot silently disable time limits.
- Archive numeric fields must use bounded, typed conversion and exact wire
  widths, not varargs formatting. CPIO keeps eight uppercase ASCII hex digits
  at every uint32 boundary; TAR reuses its octal writer with six checksum digits,
  NUL and space. Independent field and checksum oracles accompany roundtrip and
  interoperability checks; changing an API name alone is not security evidence.

- Native worker admission now caps both active windows and the floor share of
  workers per window. Ceiling division could exceed the requested aggregate;
  a one-worker minimum alone could also exceed it when queue depth was larger
  than the budget. The CPU/GPU production and RAM-validation paths share this
  policy. Decode counts actual complete-block windows through execution's
  grouping helper; total-byte division can undercount uneven blocks.
  `test_worker_budget.cpp` covers the aggregate geometry without exhausting
  host resources, exercises the real archive pipeline, and checks balanced
  nonempty ranges and exception-time joins. These are concurrency invariants,
  not complete codec-workspace admission or measured speedup claims.
- Device-memory admission must key outstanding plans and capacity by the HIP
  device selected when the plan was acquired. One adapter's reservations must
  not consume another adapter's budget or inherit its capacity. Owners retain
  that identity through moves and cleanup without changing thread selection.
  Synthetic unequal-device, pressure, concurrency and overflow tests cover the
  shared production ledger; they are not multi-GPU hardware validation. Driver
  memory snapshots remain volatile and, on Windows, may omit other processes.
- Telemetry composition must preserve every counter and worker-stage array,
  including pinned allocation/output traffic, through successful optional-GPU
  attempts and archive read-back verification. Nested classification intervals
  remain inside their enclosing stage, not additional wall or device time.
  Concurrent, overflow, CPU/GPU dispatch, archive-pipeline and JSON regressions
  cover these boundaries without adding HIP synchronization for measurement.
- Windows fixture cleanup must run after test-owned file streams leave scope.
  The worker-cap regression originally kept its read-back stream open through
  `remove_all`; verification caught the sharing violation. Its scoped stream
  and CPU/required-HIP roundtrips now exercise the corrected ownership order,
  without retry delays or relaxed cleanup assertions.
- Scanner dependency remediation must reach the real CI path. CI installs with
  normal dependency resolution, wheel-only artifacts and `--require-hashes`, runs
  `pip check`, exercises actual JWT consumer APIs and scanner controls, then
  retains full scan coverage and vulnerability gates. Prefer compatible official
  releases and retire temporary packaging revisions after equivalent validation;
  Semgrep 1.179.0 replaces the previous metadata revision. A passing isolated
  override alone is not production verification or remote alert closure.
- Scanner requirements gates must parse the permitted installation syntax,
  require one approved index and reject duplicate/unknown options, instead of
  only searching for forbidden substrings. A negative fixture assertion caused
  DevSkim `DS205001` even though it configured no index. The stronger admission
  gate rejects added install locations, direct wheel URLs, malformed hashes and
  changed binary policy. The same pinned scanner reproduced the old match and
  found none in the replacement test; hosted closure remains a separate check.
- Sanitizer build reuse belongs within one invocation and one exact compiler,
  flag and include context. Recompile shared objects for each run and sanitizer;
  never reuse a previously accepted binary or omit source from analysis. The
  ClusterFuzzLite command-contract tests retain all 14 target links, both include
  contexts, sanitizer/alignment flags, paths containing spaces and fail-fast
  compilation. Real sanitizer execution is still required; compiler fixtures
  alone are not compiled-code or fuzzing evidence.
- Standalone MCP integration scripts must not claim the SDK's `mcp` package
  namespace. Test discovery imports the uniquely named `superzip_mcp` module;
  Semgrep runtime tests import the real installed SDK from the repository root.
  The stdio client launch path remains `mcp/superzip_mcp.py`.
- CocoIndex inventories the live checkout, not every cached Git path. It omits
  an absent source only when Git confirms its unstaged deletion, retains
  unexpected-missing-file and link rejection, and removes the deleted mirror
  entry on refresh. A real temporary-checkout regression covers this boundary.

- Expected-failure native tests run in an isolated process so their checked
  exit status cannot leak into GitHub Actions' PowerShell wrapper. The local
  unit-test command propagates native exit codes like CI instead of relying on
  PowerShell's different `-File` success behavior.
- Every third-party GitHub Action and reusable workflow is pinned to a full
  40-character commit SHA. `tools\verify_change_hygiene.ps1` checks changed
  workflow files and `tools\security_scan.ps1` checks the complete workflow
  tree so a mutable tag cannot silently re-enter the supply chain.
- Workflow `run` blocks must not interpolate `${{ github.* }}` directly.
  `tools\verify_change_hygiene.ps1` and `tools\security_scan.ps1` require
  environment-variable indirection so Semgrep-style script-injection findings
  are caught before push.
- GitHub Actions must not use `Install-PackageProvider -Name NuGet` as a CI
  bootstrap. The lint workflow installs PSScriptAnalyzer from a pinned,
  hash-verified package from the official PowerShell/PSScriptAnalyzer release
  instead of depending on mutable runner PackageManagement state. The Gallery
  download endpoint returned a WAF denial on a hosted runner; the official
  release serves the identical pinned package. CI and local bootstrap keep
  the same version and hash, and must never disable the analyzer or weaken
  verification to bypass a download failure.
- The README license badge is static for the current AGPL-3.0 license. It must
  not depend on the shields GitHub license endpoint, which can fail from
  upstream token-pool exhaustion even when the repository license is valid.
- The README and application logo must derive from
  `resources\brand\superzip-logo.svg`. `tools\verify_brand_assets.ps1` checks
  that the canonical mark exists, generates the Win32 geometry from it, verifies
  the ICO, rejects SVG viewBox clipping, and forbids AI edits to the mark path
  geometry, stroke style, layer count, and source-of-truth metadata. Agents may
  move or resize the mark for layout and may edit surrounding tagline copy.
- Workflow waiting must use `tools\wait_relevant_workflows.ps1` and the plan's
  `workflowWaitPolicy`. The waiter resolves local refs to full commit SHAs,
  performs a GitHub CLI preflight, queries the Actions API by `head_sha`, and
  fails on API errors instead of polling misleading "missing workflow" states.
  Agents must not wait on completed Greenbone/OpenVAS integration runs,
  unauthenticated GitHub CLI calls, wrong commit refs, unreliable commit-filter
  wrapper results, or unrelated long-running workflows.
- Remote checkpoint timing follows remaining work, not file type. Intermediate
  workflow/verifier/MCP/skill edits and full local escalation can be observed
  once without blocking development. The planner/verifier checkpoint never
  changes local command coverage or final audit requirements. The waiter records
  exact-SHA pending evidence, rejects API errors/wrong commits, and cannot turn
  a nonblocking sample into final acceptance. Selector/checkpoint/MCP regressions
  enforce this separation. Final scope must cover all accumulated changes.
- Local compiler admission uses current RAM instead of a fixed four-job cap;
  explicit overrides cannot bypass RAM admission. The verifier runs correctness
  children at below-normal priority, restoring its own priority after exceptions.
  It does not change unrelated processes or timed benchmark priority. Shared
  resource regressions test low-memory refusal and priority restoration without
  exhausting the host. Memory planning is not a hard operating-system RAM cap.
  CMake/MSBuild scheduling shares the same admitted count and restores its local
  environment on failure. Hosted YAML must not force a stale job count past this
  admission check; dedicated CI still uses the shared production build helper.
- Local fuzzing previously had no Docker RAM or swap cap. It now admits the
  smaller budget from Windows and Docker-host free RAM, verifies cgroup v1/v2
  enforcement before compilation, and leaves CPUs unrestricted. Offline
  regressions cover low-memory refusal, malformed/unavailable counters,
  native failure propagation, unsupported limits, and both cgroup layouts.
  The real sanitizer smoke must also pass under those enforced limits.
  Volatile admission is not a reservation against concurrent unrelated work.
- A native bridge launched from PowerShell 7 can pass incompatible module paths
  to Windows PowerShell 5.1. The traced CodeQL pilot and actual MCP launcher both
  reproduced missing `Get-FileHash`. MCP now normalizes only the child environment,
  respecting `WinPSModulePath` or allowing Windows PowerShell's defaults, without
  changing the host. Mixed-case environment tests and a real Windows child test
  prove built-in command discovery and below-normal priority.
- CI timing reviews separate queue delay, setup, traced compilation, analysis,
  and timed fuzzing using exact run/job IDs. The October review found short
  queues and already resource-adaptive CodeQL execution, not an idle timeout.
  The offline Greenbone audit now cancels superseded checks; live scans and
  releases retain non-cancelling transaction semantics. Preserve those different
  ownership boundaries rather than applying one concurrency rule everywhere.
  [The timing review](workflow-performance-review.md) records measured costs
  and the limits of the optimization claim.
- CodeQL C++ must use a real manual Windows build database. Build-free C/C++
  analysis produced parser-artifact Security tab alerts for Win32/GDI+, HIP,
  and vendored C code; `tools\verify_change_hygiene.ps1` and
  `tools\security_scan.ps1` reject restoring `build-mode: none`.
- Changed-file lint must validate the GitHub push base before using it. Amended
  or force-with-lease pushes can reference a replaced sibling commit; the lint
  workflow falls back to `HEAD~1` only after proving the event base is not
  available in the full-history checkout.
- Build/package commands must not run in parallel with GUI smoke tests. The
  build script checks for a running build-output `SuperZip.exe` and fails with a
  direct close-the-app message instead of surfacing a linker file-lock error.
- Release replacement is exceptional. The release workflow requires
  `replace_existing=true` plus `replacement_acknowledgement` set exactly to
  `replace <same-version>`, and both `tools\verify_change_hygiene.ps1` and
  `tools\security_scan.ps1` reject release workflow/action edits that remove
  that safeguard.
- MSI release replacement must use major-upgrade identity, not version-only
  ProductCode reuse. Keep a stable UpgradeCode, keep same-version
  `MajorUpgrade` enabled, and seed ProductCode with the release/build identity so
  a republished same-version MSI replaces an installed package cleanly instead
  of showing Windows Installer's already-installed conflict.
- Release instructions must use placeholder SemVer values in examples instead
  of concrete proposed release versions. The changed-file hygiene and full
  security scans reject hardcoded `release_version=` and replacement examples
  in release/agent instructions.
- Release notes must not repeat the GitHub release title as a Markdown H1.
  `tools\verify_change_hygiene.ps1` and `tools\security_scan.ps1` reject the
  generated-notes pattern that caused the release title to render twice.
- Greenbone/OpenVAS manual target input is a broker authorization request, not
  the effective scan target. `tools\verify_change_hygiene.ps1` and
  `tools\security_scan.ps1` reject workflow edits that let workflow-dispatch
  target text bypass the OIDC broker's returned `greenbone_target`.
- Archive-format GUI labels must keep exactly one visible extension per create
  selector row and must be backed by the core extension registry. Focused C++
  tests reject grouped visible alias labels while still allowing engineering
  docs to describe grouped backend support.
- Process-local environment restoration must distinguish absent and empty
  values across Windows PowerShell and PowerShell 7. Compiler scopes, build
  scheduling and GUI-smoke cleanup use `tools/process_environment.ps1`;
  `tools/test_process_environment.ps1` covers exact restoration after success,
  exceptions and nested scopes. A passing check under one shell does not
  establish the other shell's behavior.
- Installer process waits are bounded. `tools\security_scan.ps1` rejects
  release workflow edits that restore unbounded `Start-Process -Wait` calls for
  MSI install/repair/uninstall smoke tests, and the MSI smoke timeout must remain
  300 seconds by default. Whole-SDK provisioning uses the shared contained
  extraction path and finite download/inventory bounds.
- Standalone fuzz targets must link every transitive source dependency, and the
  local Docker driver must turn native failures into a nonzero PowerShell exit.
  `tools\security_scan.ps1` enforces both rules so sanitizer linker failures
  cannot be reported as a successful verification lane.
- Local fuzz smoke runs execute `.clusterfuzzlite/local_smoke.sh` from the
  read-only source mount. Passing script text through Windows native command
  arguments stripped XAR XML attribute quotes and weakened the valid seed.
  The script parses its generated XML before fuzzing; the security policy
  checks LF transport and parser validation, and selector tests cover routing.
- A direct base-builder invocation does not apply ClusterFuzzLite's sanitizer
  flags. The local smoke script must explicitly instrument both C and C++ with
  ASan, UBSan, and libFuzzer coverage, fail on sanitizer reports, and check every
  target's linked runtimes. A libFuzzer pass, `SANITIZER` environment value, or
  linked UBSan symbol alone does not prove product-source instrumentation.
  Earlier local smoke passes without those flags are libFuzzer-only evidence;
  hosted address/undefined jobs are separate evidence. See
  [LLVM's libFuzzer usage](https://llvm.org/docs/LibFuzzer.html#fuzzer-usage).
- The pinned base image's hosted undefined-check list omits alignment. The
  shared fuzz build explicitly adds nonrecovering alignment instrumentation
  for C and C++ in that lane; the policy scan guards this addition. A green
  sanitizer job proves only the enabled checks, not every sanitizer category.
- Hardware support for unaligned loads does not make typed integer pointer
  casts on byte buffers valid C/C++. The LZMA SDK's production endian accessors
  use fixed-size `memcpy` with existing endian conversions, not sanitizer-only
  substitutions or disabled alignment checks. Byte-oracle/canary tests cover
  every offset 0-15, and the previously failing valid 7z seed remains in fuzz
  smoke. The policy scan rejects reintroduced SDK casts or missing Windows/Linux
  oracle hooks. Preserve these downstream adaptations when updating the SDK.
- XAR payload `length` is the stored heap extent and `size` is decoded bytes.
  The parser and its old fixture builder shared a reversed mapping, so their
  mutual agreement was not interoperability evidence. The compatibility smoke
  now extracts independent libarchive-created stored/zlib XARs; C++ tests reject
  reversed extents, and the valid fuzz seed asserts both standard fields.
- Shared SDK CRC tables must initialize through one guard before either native
  or 7z use. Independent bitwise-oracle, seeded-fragmentation, and concurrent
  first-use regressions protect the IEEE polynomial and finalized seed
  convention. Separate per-caller initialization guards do not protect shared
  mutable SDK state from concurrent writes.
- An entropy savings threshold must not suppress a smaller GPU dictionary
  candidate. Bounded source-structure screening and complete payload comparison
  remain authoritative. The all-nine-effort regression reproduces a prior
  effort-6 ratio inversion and checks CPU/HIP reconstruction after its repair.
- Bounded GPU output admission must resolve decoder CPU work from the actual
  decode queue, not the encoder's worker share. Native and RAM paths share
  the worker calculation; CRC-oracle and ownership regressions preserve host
  output validation, failure cleanup, and pool lifetime. Pool setup and final
  release stay inside extraction timing. Reporting tests distinguish fresh
  pinned allocation from all reused pinned output, and decoder queue/worker
  fields remain separate from encoder fields. RAM reconstruction CRC checks
  must not be described as full byte comparisons. GUI smoke coverage uses the
  current run's returned capture manifest, not stale images accumulated in
  the output directory. Every screenshot-producing helper must return its
  record or append it to the caller's current collection; writing the image
  alone does not establish review coverage.
- Host telemetry recorders compile independently of HIP. A hosted CPU-only
  build caught new pinned-output recorders calling an attempt-merge helper
  hidden under the HIP guard; the recorders now use the same direct atomic
  update pattern as other host counters. The backend-independent counter test
  runs in both configurations. A passing HIP build is not CPU-only compile
  evidence; changes crossing that boundary require both configuration checks.
- Embedded-manifest tests inspect every configured executable, not an optional
  GUI that was disabled at configure time. CMake supplies the component flag
  and test-target dependencies; the same test still fails for a missing enabled
  executable. Both GUI-enabled and headless builds must retain the CLI/test
  Windows compatibility identity checks.
- Profile normal adapter phases before changing codec policy. Zstandard's
  optional setup/stream/publication intervals keep runtime admission, codec
  plus file I/O, and durable commit distinct from whole-command time. Worker
  admission tests cover unknown sizes, small inputs, low-core hosts, all nine
  efforts, and size-counter extremes. Independent read-back and exact archive
  sizes accompany timing: threaded speed gains can change job-boundary ratio.
  Test-name filtering is the native harness's positional argument, not an
  environment variable; supplementary configurations use the pinned CTest.
- `tools\github_post_push_audit.ps1 -IncludeHistory` fetches open, fixed, and
  dismissed incidents only on demand. Its optional new-file JSON report retains
  every incident ID, rule, state, location, and latest analysis identity without
  raw reviewer comments. Pagination rejects duplicate IDs, malformed records,
  partial API failures, and non-open records in open-only queries. Offline tests
  prove that closed history cannot pass an unapproved current finding, report
  collisions preserve existing bytes, and blocked audits still save evidence.
  Relative report paths use the PowerShell location rather than the unrelated
  .NET process directory; a changed-location regression covers both shells.
  Remote resolution requires an exact credential-free GitHub HTTPS/SSH host,
  rejects failed Git queries, and never echoes rejected remote text. Mocked Git
  and API tests prove that lookalike hosts cannot select a different repository.
  Both the audit and workflow waiter use `tools/github_repository.ps1`; route
  regressions reject copied parsers, missing imports, and caller-directory
  dependence. A repair in only one caller does not close this boundary.
- CMake script checkouts require LF through `.gitattributes`, not only an editor
  preference. A local LF file passed lint but checked out as CRLF on the hosted
  Windows runner before the `*.cmake` attribute was added. Validate attribute
  changes with a fresh Git export under the runner's checkout configuration.

- `tools/semgrep_coverage.py` compares actual same-run scanner admission with
  Git's complete tracked inventory and retains parser/matcher diagnostic
  counts and locations. Offline regressions cover deep/vendor/HIP/test/skill
  paths, malformed evidence, omitted source, and metadata-only publication.
  A `find` listing or successful scan exit is not coverage evidence. CPU-only
  CodeQL does not extract HIP kernels, and scanner upgrades do not change a
  public checksum into a credential. Keep source pins and negative tests;
  record each false positive only after checking its producer, consumer, and
  exact analysis identity. GitHub disposition comments are limited to 280
  characters; preserve full evidence separately and stop on API failure.

- `tools/devskim_provenance.py` derives the download ceiling and exact package
  identity from one measured, pinned provenance record. The verified NuGet
  package was 58,204,542 bytes, not within the previously guessed 50 MB ceiling.
  Offline regressions reject metadata drift, truncation, growth, and digest
  corruption. A CLI-version probe does not validate the SDK installation path;
  the hosted job must also install and scan through the verified local feed.
  The next run exposed an upstream empty-snippet SARIF serialization failure.
  Before changing scanner pins, exercise an actual finding through scanning,
  publication, and complete SARIF schema validation, not only the version probe.
  Keep source excerpts disabled and finding metadata intact. The report adapter
  rejects producer drift and unexpected excerpt content; its offline regressions
  run locally and in hosted CI. A valid raw result count is not an accepted upload.

The [incident-history review](code-scanning-history-review.md) maps all retrieved
scanner families to existing guardrails and records unfinished work. It is an
evidence index, not a new allowlist or proof that every historical alert was a
vulnerability. Normative agent rules remain in the operating guide.

- `core/host_memory_budget.cpp` owns native production and RAM-benchmark buffer
  admission. `test_host_memory_budget.cpp` checks boundary refusal, unknown
  capacity, overflow, reserve parity, and a synthetic host-size matrix. Never
  clamp insufficient capacity upward to one window or give benchmarks a
  smaller buffer estimate than production. Keep estimated buffers distinct
  from codec workspace and a volatile snapshot distinct from a reservation.
  The extraction's first push exposed an orphaned overflow helper through
  CodeQL `cpp/unused-static-function`; the build and contract gate had passed.
  Apply the operating guide's private-helper reference check after moving
  callers, and confirm automatic closure in the exact follow-up analysis.

## Adding A New Lesson

Security repairs must finish the learning loop rather than stopping at a lower
alert count. Review the same boundary in sibling versions and direct callers,
preserve a practical old-source negative control, and bind verification to the
changed bytes. Keep a failed control distinct from a test that only establishes
preserved behavior. Instructions and static guards reduce recurrence; they do
not establish the absence of vulnerabilities.

At the maintainer's October 4 final checkpoint, first verify all addressed
findings against the exact pushed SHA, then review the prevention mechanisms
and their actual local/hosted routing. New repairs restart affected verification;
a successful pre-push policy review cannot stand in for this final review.

### Security Tool And Skill Review, October 4

The local VulnerabilityScreener evidence-label skill distinguishes executed,
source-inspected, proposed and externally reported evidence. The AI skills
catalog's improved property-testing and verification-loop guidance adds useful
oracle checks, preserved first failures and source-bound verification. These
methods fit the existing SuperZip review process; their local static catalog
qualification is not evidence of fewer agent errors. The local OMERO repository
has no checked-out source in this review, so no source assessment was made there.

[Trail of Bits variant analysis](https://github.com/trailofbits/skills/blob/82fe8226252622fa807643bdca1710901198553a/plugins/variant-analysis/skills/variant-analysis/SKILL.md)
informs root-cause and sibling/caller review. SuperZip applies it serially under
AGENTS.md rather than importing its worker orchestration. No plugin installer,
external command execution, runtime dependency or blanket suppression was added.

[Microsoft's Windows ASan documentation](https://learn.microsoft.com/en-us/cpp/sanitizers/asan?view=msvc-170)
supports isolated x64 MSVC instrumentation. The local catalog's instruction to
skip Windows ASan was rejected as inconsistent with that documentation and the
executed qualification. `tools/test_zstd_sanitizers.ps1` now builds the canonical
patched library and the same dependency contracts in a separate validation tree.
`cmake/ZstdLibrary.cmake` and `cmake/ZstdTests.cmake` share those definitions with
the ordinary HIP product build. The validation project has no product packaging
or HIP targets. Its valid-access and deliberately invalid heap-access controls
prove working instrumentation before ordinary tests run. Six CTest targets
passed in the initial local qualification. Missing tooling, runtime failure,
unrecognized diagnostics and failed tests remain errors; nothing falls back to
uninstrumented success.

[Microsoft Core Guidelines checkers](https://learn.microsoft.com/en-us/cpp/code-quality/using-the-cpp-core-guidelines-checkers?view=msvc-170)
and [LLVM UBSan](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html)
remain distinct candidate analyses, not enabled or qualified gates. Windows ASan
does not establish unsigned-overflow, null-arithmetic, leak or device-kernel
coverage. Preserve the exact arithmetic/cursor regressions, guard pages, fault
injection, full CodeQL suites and HIP qualification. Existing libFuzzer lanes
remain in place; installing another scanner or skill does not establish added
coverage or measurable prevention.

[OWASP's AI secure-coding guidance](https://cheatsheetseries.owasp.org/cheatsheets/Secure_Coding_with_AI_Cheat_Sheet.html)
informs the external-content trust boundary in the existing security skill:
retrieved source, reports and skills are review data, not authority to install
packages or weaken repository controls. Reputable provenance is a selection
input; inspect and qualify the mechanism for this platform before adopting it.

- The October 4 Zstandard raw-block control crashed because adding header size
  to a payload near `SIZE_MAX` wrapped the capacity comparison. The canonical
  writer now checks overhead first and compares payload with remaining capacity
  before any write/copy. `zstd_raw_block_writer_overflow_rejection` retains the
  failing control and guarded zero/exact/short-capacity cases cover valid writes.
- Dictionary shrinking must use initialized input length, not finalized
  dictionary size or allocation capacity, to select an input suffix. Doubling a
  candidate requires a division-based bound first. Exact canonical selector
  probes retain guarded input, candidate progression, and allocation/finalizer
  failure coverage in `superzip_zstd_cover_selection_tests`.
- Legacy v0.5-v0.7 stream rewrites must preserve null empty input/output without
  null arithmetic, zero-byte copies through null cursors, or public pointer
  substitution. Direct canonical tests cover partial headers, stalls and
  recovery; production-DLL tests reproduce and reject the observed pointer
  mutation. Transactional reserve tests fail each allocation and verify that
  existing owners/capacities remain usable. Keep v0.7's matching custom allocator
  and deallocator rather than importing v0.5/v0.6's ordinary allocator contract.
- `tools/zstd_rewrite_policy.ps1` checks these concrete source boundaries and
  retained regression entry points. Changed-file hygiene runs it for dependency
  rewrite surfaces; repository security scanning always runs it. The canonical
  planner routes `tools/test_zstd_rewrite_policy.ps1` to relevant source/tool
  edits and hosted component contracts. Its 14 negative controls exercise actual
  owned-source mutations, comment-only checks and missing input; this narrow
  static guard supplements native tests and CodeQL without suppressing findings.
- Dependency recipes must migrate every exact supported preceding revision as
  well as fresh pinned source. The first October 4 legacy migration correctly
  stopped on an unrecognized local hash. `superzip_zstd_legacy_patch` now tests
  both previous stream revisions, fresh application, idempotence, unknown-byte
  rejection and interrupted writes. Do not bypass identity checks to recover.
- Legacy sequence helpers must prove output, literal and dictionary extents
  before forming endpoints or copying. The October 4 canonical controls accepted
  a `SIZE_MAX` literal length and wrote output in all three shipped versions;
  v0.7 also stored an encoded block error as a history endpoint. Explicit history
  lengths and error-first publication repair these boundaries. Independent LZ
  recurrence tests cover 38,964 valid sequences plus malformed extents, and the
  existing fault objects compile the complete canonical sources. Extending a
  supersession relationship too broadly then skipped an old stream migration;
  the exact historical fixture rejected it. Bind each supersession edge to its
  actual later revision and keep all prior migration controls. The source-policy
  tests now retain 25 rejection controls, including commented-out dispatch
  mutations. Require an executable dispatch line rather than accepting a call
  name inside a comment. These controls do not replace native or SAST
  verification or establish malformed-archive reachability for injected lengths.
- Legacy one-shot dictionary setup must propagate its error before any frame
  output. The October 4 public-DLL controls accepted malformed dictionaries
  and wrote output in all three shipped versions. The v0.5/v0.6 loaders also
  read a four-byte marker from a one-byte guarded dictionary and crashed.
  Complete entry-point/loader replacements retain raw-content semantics,
  reject invalid entropy setup before output, and allow same-context recovery.
  Six production-DLL tests retain malformed/short dictionary controls across
  v0.5-v0.7; exact preceding-source migration joins the existing fixture matrix.
  The source-policy guard now has 31 rejection controls, including missing and
  commented-out error propagation and dictionary dispatch. Native/ASan/SAST
  results remain distinct from scanner dispositions and publication acceptance.
- Import only required private declarations into canonical source probes.
  Broad internal codec imports introduced ten unused static constants in a
  fresh CodeQL extraction. Narrowing the dictionary-merger probe alone did not
  close them: the raw-writer test also imported the complete compression header.
  Both probes now import only their exact canonical algorithm and required
  declarations, retaining separate region and declaration digests. Re-run
  source-bound analysis to verify closure; a successful compile cannot establish
  it. Keep the complete production translation units in scanner coverage.
- Shared CMake modules must preserve their caller-visible output names and
  validate required inputs before creating targets. During the October 4
  library/test extraction, renaming a local variable also renamed the exported
  library directory and broke the product configure. The export was restored;
  `ZstdTests.cmake` now rejects an absent or invalid directory explicitly. Both
  the HIP product build and isolated sanitizer consumer must pass after changes
  to these shared definitions.

Visible preferences must be traced through settings capture and the actual job
before being described as features. `tools/gui_smoke.ps1` exercises Apply and
legacy migration, including true/false obsolete no-op values. Never activate an
old saved deletion preference when removing unsupported state. The October 2
cleanup removes four controls without introducing source deletion.

Compile fault-injected dependency sources directly in a separate object target,
with allocator interception confined to those objects. Do not include `.c`
files from wrapper sources or leak interception into the fixture's real allocator
or product DLL. Legacy allocation-failure tests must still fail on the original
defect. For untouched telemetry, test exact zero classification rather than
using a tolerance that could admit recorded work; retain separate checks for
non-finite or overflowing timings.

HIP event timing now has bounded accumulation and a persistent unavailable
state. Encode and owned-decode worker intervals use the same overflow contract;
never restore unchecked timing `fetch_add` or sum overlapping worker intervals
as wall-clock phase fractions. `test_gpu_telemetry.cpp` covers invalid numbers,
total overflow, concurrent updates and production CPU/GPU decode recording;
`tools/test_benchmark_reporting.ps1` rejects missing, extra or non-finite stage
evidence and checks JSON roundtrips and lowercase native `nan` handling on
Windows PowerShell.
Successful HIP API calls must not be taken as proof that returned durations
are valid.

Device suballocation must preserve the transaction alignment of the original
allocations, not merely a metadata type's alignment. Checked extents and fewer
allocator calls do not prove faster execution. The October 2 packed-workspace
experiment was not accepted after its aligned correction still regressed Mixed
verification; retain failed hypotheses and inspect the affected phases before
repeating an experiment or claiming a gain.

The repository's `mcp/` directory is not installed as the Python `mcp` package.
An installed SDK can shadow that name. Launch the documented MCP entry point;
for a direct internal helper invocation, load the exact repository file rather
than assuming `import mcp.superzip_mcp` resolves locally.

When a failure exposes a repeatable class of mistake, add the narrowest useful
guard in this order:

1. Fix the root cause in product code, workflow code, or documentation.
2. Add a local invariant to `tools\verify_change_hygiene.ps1` when the issue can
   be detected from changed files.
3. Add a repo-wide invariant to `tools\security_scan.ps1` when stale or
   unrelated files can reintroduce the issue.
4. Add or update selector coverage in `tools\test_verification_selector.ps1`
   only when routing behavior changes.
5. Document the lesson here in one short bullet with the concrete tool that
   enforces it.

Do not add broad exclusions, scanner suppressions, mandatory heavyweight gates,
or product-direction changes as a lesson. A learning guard is valid only when it
prevents a known failure class without hiding findings or slowing unrelated
development paths.

Historical evidence stays bound to its recorded source revision. A current
source edit must not invalidate an unchanged historical checksum or cause its
scanner policy to be broadened. `tools/test_gitleaks_policy.py` verifies the
exact original Git blobs and rejects neighboring authentication fields, changed
values and paths outside passive benchmark records.

Legacy decoder state must own every byte that survives a call: history,
dictionary content and padded literals. Keep the active output prefix local,
retain only the decoded window's initialized suffix, preserve destination
allocator identity when cloning, and propagate clone or capture failures before
later output. `test_zstd_legacy_history.cpp` checks source expiry, independent
byte oracles, complete acquisition rollback and prepared-frame error propagation;
`test_zstd_legacy_buffers.cpp` checks ring geometry, window changes, allocation
matching and actual memory accounting. The rewrite policy rejects removal of
these production boundaries and their executable regressions.

Dependency rewrites must publish the complete current implementation directly
from immutable provenance. Retire superseded production generators once their
ownership and extent boundaries are replaced; keep exact historical identities
for cache migration, and reject unknown bytes before publication. Historical
migration tests reconstruct passive inputs from a pinned published Git tree in
private storage, then apply the current generator. They require a full Git
checkout and never compile the retired implementations. Product builds use the
pinned upstream archive and do not require Git history. Keep source-language
comment parsing in policy guards separate from generated-code arguments; a
CMake glob must not erase later checks, and comments cannot satisfy a guard.
Stage intentional native source deletions before capturing build inputs. The
provenance collector deliberately rejects missing files still present in the
Git index; this prevents an incomplete checkout from producing an accepted
build receipt. Review the staged deletions and preserve that rejection contract.

Headers should declare shared immutable library values when callers require one
identity; define their storage once in the owning production translation unit.
Validate cross-language linkage, address identity and unchanged values with
independent callers and an original-storage negative control. Tool source
repairs require exact original and complete output identities, atomic publication,
repeated-inclusion controls and runtime probe checks. Include repaired compiler
probe bytes in native build receipts. Public artifact digests belong in standard
checksum manifests with a closed inventory; retain the complete hexadecimal
values, validate every record and bind the manifest to native inputs.

When a portable evidence schema changes, exercise its real checked-in records
and their publication consumers alongside synthetic mutation fixtures. Historical
readers may admit an exact older schema in a separate cohort; they must not
invent missing evidence or qualify current builds. Keep current artifact
validation strict and preserve byte-for-byte regeneration of reviewed outputs.

Shared graph document structure belongs in the passive SVG template; renderers
instantiate independent documents and insert measurements through XML APIs.
The standard namespace is an identifier, not a network request; preserve the
[W3C SVG namespace](https://www.w3.org/TR/SVG2/struct.html#Namespace).
Keep templates in publication/scanner scope, reject active markup and oversized
inputs, and check the real published charts for byte-exact regeneration.
Route graph tests and template loading through the same bounded UTF-8 XML
boundary. Reject declarations before parsing, enforce depth/node limits during
construction, and cap reads before allocating file contents. Cover encoded DTDs,
external and parameter entities, processing instructions, malformed encodings,
exact structural limits and real published documents. Keep the DTD callback as
an independent guard; do not rely on a byte-pattern check that alternate XML
encodings can bypass.

Minimize owned dependencies around their actual enabled capability contracts.
Preserve upstream provenance and supported public interfaces; require compiler
token equivalence for working configurations and retain a failing original
control for each additional source repair. Cover language, staged inclusion and
optional-API combinations. Discover compiler include paths from its actual
build instead of guessing host installations, and use the selected compiler's
standard option. Dependency specialization does not establish a performance
improvement or repair an algorithm that the product does not enable.

When a dependency produces MSVC `/GL` objects, propagate explicit Release
`/LTCG` to its link consumers. This preserves the required optimization while
avoiding the linker's automatic restart. Confirm the effective link options and
absence of restart diagnostics; report timing improvements only from measured
comparisons under comparable conditions.

Keep precompiled headers private, limited to stable standard headers and compiled
independently for targets with different flags or definitions. Preserve explicit
source includes and an existing full build without PCH to expose missing includes.
Qualify mixed C/C++ consumers, target-local macros and the real scanner extraction
mechanism before adoption. Bind the strategy to build receipts. Measure clean
rebuilds in reversed pairs with host samples; compiler throughput evidence does
not establish codec speed or hosted-analysis speed. The retained evidence and
scope are in [build throughput validation](build-throughput-validation.md).

Release identity and replacement policy share one implementation in
`tools/release_workflow_policy.ps1`, used by changed-file hygiene and repository
security scanning. Offline mutation contracts check commit binding and guard
removal. Select these contracts for orchestration changes; rebuilding unchanged
artifacts cannot validate an Actions reference. Keep artifact qualification for
actual packaging producers.

Verify new GitHub syntax with the actual latest stable scanners before changing
production workflows. actionlint 1.7.12 and the current Semgrep action-pin rule
do not recognize `$/`. The release pipeline now declares its stages directly
in workflow source, preserving commands and pinned tools without a redundant
local action or scanner exception.

Treat platform-specific dependency failures at their production boundary.
Preserve the upstream security property with the host's native equivalent;
never replace an unavailable protection flag with zero. For an identified
downstream source build, retain original provenance and notices, minimize and
test the source delta, use normal dependency resolution, and verify the installed
repair bytes on cache reuse. Exercise the actual affected API with its documented
configuration type, plus refusal-before-write and ownership cleanup controls.
Admit untrusted names before normalization and I/O backend selection: a custom
opener is a file-handle boundary, and some runtimes select device or console I/O
before invoking it. Exercise the composed destination-validation and write path
on fresh platform installations as well as cache reuse; a hidden local process
does not reproduce every hosted runtime context.
The [portable crawler repair](crawl4ai-portable-download-repair-2026-10-05.md)
records this incident; the reusable source and consumer gates select automatically.

When introducing a composed encoding, retain the preceding representation's
independent oracle. Reconstruct and compare its exact bytes instead of deleting
an equality check when the outer frame changes. Require strict complete-frame
cost improvement, independent reference decoding, real required-backend
readback, closed stage kinds, corruption/downgrade controls and cancellation
at the added stage. Protocol limits belong in the native format contract;
individual measurements and failed assertions belong in dated validation records.

When adding a serialized kind or readable version, update every closed dispatch,
independent version oracle and sanitizer seed before qualification. Keep the
exhaustive byte-kind matrix independent from production dispatch and require
its latest covered version to match the reader boundary. Test both writer
refusal and reader refusal of a downgraded new-kind index. Preserve previous
codec-byte oracles when a new framed representation wins.

Keep backend selection separate from operation failure. Expected absence and
known unsupported block kinds can select an explicitly permitted CPU route
before work begins. An error after HIP selection must remain an error; a broad
exception handler must not turn failed device work into successful CPU output.
Exercise CPU-only and HIP builds, optional and required policies, independent
readback and telemetry for the same dispatcher. Storage/allocator compatibility
choices must preserve the selected backend and retain their documented bounds.

For metadata returned inside an owned API buffer, admit the reported location
against that buffer before forming a view. Convert fixed object representations
through exact-size byte arrays and typed value conversion; retain signature
and version checks. A trusted provider does not make an unchecked byte extent
an ownership contract.

A dependency version check cannot establish that its security repair is still
installed. For an admitted source build, derive a bounded runtime source
inventory and content identity from the original artifact and documented
packaging changes. Verify that identity before loading the dependency on cache
reuse, retain normal dependency resolution, and reject added, missing or changed
source without retrying through an unverified installation. Share archive
metadata admission with the build reader; check path aliases, redirects, file
types and budgets before reads or extraction writes. Exercise real admitted
source plus mutation controls and an installed-cache consumer. Verification
changes can reuse an unchanged, hash-admitted build artifact; they must not
silently regenerate a wheel or weaken its source/recipe/output binding.

Source admission must also bind the standard import path. A version and source
digest cannot qualify execution through an unchecked bytecode cache. For a
source-admitted runtime, use a fresh owned cache prefix with bytecode writing
disabled, retain cache files, reject executable additions outside that package's
declared representation, and test actual interpreter import behavior. Keep
runtime flags scoped to the owned child; do not monkeypatch the import system
or rebuild an unchanged package merely to change its verification policy.

Verify pinned binary tools before executing even their version probes. When a
measurement lease follows provisioning, compare the locked executable bytes
with the provisioner's exact evidence before launch. Test the actual contained
Python caller as well as direct shell invocation: inherited module search paths
can differ between PowerShell editions. Load required inbox modules from the
running interpreter's installation, preserve hash checks, and reject linked
installation ancestors before creating directories. Keep missing, changed and
redirected-artifact controls executable without network access.

Construct offline redirect tests from the reviewed origin using the standard
URL parser, changing protocol, authority and query independently. Assert the
intended mutation explicitly and require rejection before any network request.
This isolates the property under test without retaining an unnecessary combined
protocol-and-origin example or removing downgrade coverage. Such tests remain
inside source analysis; every raw report and unmatched finding stays retained.

Keep codec canonicality separate from payload containment. Validate the finite
codec catalog, exact raw/fill extents and compressed-child constraints before
checking the containing byte span or dispatching work. Exhaust all serialized
kind values and test extent boundaries with an independent wire fixture; a
shorter Boolean expression alone does not establish equivalent admission.

An immutable Action pin must resolve from its documented upstream tag. Review
the actual pinned action and CLI together before changing integration: inputs,
authentication and report formats must agree. Retrieve complete scanner reports
explicitly, retain original results, reject malformed numeric scores, and test
both findings and empty results. Validate private broker credentials before
passing them to a third-party action, mask them before outputs, and keep their
values out of failure diagnostics. Offline contracts do not prove a live scan
or authenticated upload; those require the authorized broker and target.
