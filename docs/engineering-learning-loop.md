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
  the output directory.
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

Release identity and replacement policy share one implementation in
`tools/release_workflow_policy.ps1`, used by changed-file hygiene and repository
security scanning. Offline mutation contracts check commit binding and guard
removal. Select these contracts for orchestration changes; rebuilding unchanged
artifacts cannot validate an Actions reference. Keep artifact qualification for
actual packaging producers.
