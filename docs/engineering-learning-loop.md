# Engineering Learning Loop

SuperZip records concrete engineering mistakes as enforceable repository
invariants. The goal is not more process; it is to convert verified failures
into small checks that prevent the same regression from reaching GitHub Actions,
the Security tab, released artifacts, or the product UI again.

## Current Guardrails

- Scanner dependency remediation must reach the real CI path. The reviewed
  Semgrep packaging revision has a distinct local version, pinned upstream and
  derived hashes, unchanged code/notice bytes, and offline provenance tests.
  CI installs with normal dependency resolution and `--require-hashes`, runs
  `pip check`, exercises actual JWT consumer APIs and scanner controls, then
  retains the full scan and vulnerability gates. A passing isolated override
  alone is not production verification or remote alert closure.
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
- Installer process waits are bounded. `tools\security_scan.ps1` rejects
  release workflow edits that restore unbounded `Start-Process -Wait` calls for
  HIP SDK setup or MSI install/repair/uninstall smoke tests, and the MSI smoke timeout
  must remain 300 seconds by default.
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

HIP event timing now has bounded accumulation and a persistent unavailable
state. `test_gpu_telemetry.cpp` covers invalid numbers, total overflow, and
concurrent updates; `tools/test_benchmark_reporting.ps1` rejects non-finite
evidence and checks lowercase native `nan` handling on Windows PowerShell.
Successful HIP API calls must not be taken as proof that returned durations
are valid.

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
