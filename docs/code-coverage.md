# Code coverage

The README Codecov badge links to measured native CPU line coverage. Coverage
helps locate untested behavior; it does not establish correctness, security,
performance, or a release qualification by itself.

## Coverage Follow-up

Improve coverage through coherent groups of missing behavior with independent
correctness, failure and recovery assertions. Preserve the existing collectors,
tests, raw reports and CI uploads. Prioritize potential data loss, unsafe input,
ownership, cancellation, GPU execution and changed behavior before low-impact
diagnostic paths.

Successful local GUI/HIP-host measurement writes `summary.json` and the raw report
under `out/coverage-hip-host-gui`. Before a new collection, the collector moves
the prior reports into `previous-measurement-*`; if that collection fails, no
current summary exists at the output root. Archived reports may guide test design
after checking their source, binary, collector and scenario identities, but they
do not qualify the current checkout. The hosted
CPU report, GUI/HIP-host report, tooling and device instructions have distinct
scopes; none establishes whole-repository coverage.

Device measurement must first pass the helper-inclusive qualification control
[in the device guide](development/hip-device-coverage.md), including the pinned compiler's registration repair. Unmodified
compiler output omits helper counters; the repaired measurement mechanism has
passed independent kernel/helper controls. Product measurements must identify
the selected real GPU workloads and their remaining behavior and measurement
gaps. A measured host percentage does not establish device execution.

## Release Acceptance

Release qualification uses the [release acceptance criteria](release.md#acceptance-criteria).
Universal 100% line coverage is not a release requirement. Coverage is an
execution measurement; release confidence also requires effective assertions,
independent interoperability, failure recovery and the supported user journeys.
Apply these requirements to every release candidate:

1. All applicable correctness and regression tests pass. Known data corruption,
   unsafe publication, unbounded execution, resource lifetime failures and
   untested critical advertised behavior block release, regardless of percentage.
2. Every changed production behavior has direct meaningful assertions for its
   important success, rejection and recovery contracts. Review uncovered changed
   lines and sibling consumers. A passing surrounding function or an incidental
   integration hit is insufficient. Use a reduced reproducer or an isolated
   behavioral mutation when it is needed to establish regression sensitivity.
3. Preserve the measured denominator, zero-hit lines and unmeasured-source
   inventory. Require valid source/tool/binary identities. Missing or failed
   collection cannot qualify a scope that the change requires. Compare only
   equivalent scopes and configurations; investigate coverage regressions before
   accepting a candidate.
4. Review residual gaps by component and behavior, with depth proportional to
   their impact. Retain a concise candidate review identifying the source/report,
   relevant existing tests, remaining risk and the next useful test or hardware
   qualification. Untested high-risk behavior needs a test or production repair.
   Established shared-boundary tests, compiler mapping artifacts, constant
   evaluation and hardware-specific paths may justify residual gaps when the
   evidence and limitations are explicit. A category name alone is not evidence
   that a particular missed line is harmless.
5. Keep CPU, HIP host, GUI, tooling and device measurements distinct. Release
   scope follows documented supported capabilities and actual qualification.
   Unmeasured hardware and missing measurement scopes remain visible; they do
   not become claims of universal execution or whole-repository coverage.

The [Codecov configuration](../.github/codecov.yml) compares the uploaded native
CPU project report with its base without permitting a percentage drop. Its 90%
patch target is advisory and helps prioritize review; direct behavior tests and
the requirements above decide whether a gap is acceptable. The required Windows
CI collector and upload still fail on execution, collection or upload errors.
Never add ignored source paths, fabricate hits, remove defensive code or write
low-value tests to satisfy a counter. Complete the relevant acceptance work,
then stop redundant scans and recollection while their identities remain valid.

[Google's coverage guidance](https://testing.googleblog.com/2020/08/code-coverage-best-practices.html)
supports risk-based gap review and warns against treating a percentage as test
quality. Its [release-testing guidance](https://testing.googleblog.com/2021/06/how-much-testing-is-enough.html)
adds integration tests and critical user journeys. [LLVM](https://clang.llvm.org/docs/SourceBasedCodeCoverage.html)
documents distinct line, region, branch and instantiation metrics, including
constant-folded branches. These principles guide the policy; they do not
establish product correctness or waive an identified defect.

## Measurement

The existing Windows 2022 CI build uses unoptimized `Debug` binaries and matching
PDB symbols, avoiding inlining and removed-code distortions in line mapping.
The Windows 2025 build keeps `Release`; HIP qualification
remains separate. After the selected correctness checks, OpenCppCoverage runs the
complete native test registry and CLI contracts, including child processes,
against the same build. CLI coverage includes help/version entry points and the
existing planning, invalid-input, binary-stdin and exact round-trip contracts,
plus the complete registry-driven archive format matrix. The Debug CI lane runs
that matrix inside collection; the Release lane retains its standalone check.
The matrix also checks option rejection without output changes, literal filenames
after `--`, and CLI SHA-256 against an independently calculated digest.
These correctness workloads retain their bounds; their timing is not performance
evidence. A failed scenario stops collection and retains its diagnostics.
There is no extra compilation or coverage runner. Collector changes select this
hosted consumer; offline collector tests do not silently rebuild the product.
The routing contract imports the collector's actual frozen host-scenario inputs
and requires every path to select Debug measurement and match the owned push/PR
filters. Changes to its MCP process owner or scenario helpers therefore reach
the real measurement/upload consumer. The Release lane and ordinary local
planning retain their independent build requirements.

The collector selects first-party `src` files linked into the native test and
CLI executables. It publishes their measured executable lines, including zero-hit
lines. GUI paths not linked into these consumers, Python/PowerShell tooling,
and GPU device instructions are not measured by this report. Do not describe this
number as whole-repository, branch, or GPU coverage.

On a HIP-capable development host, build `Debug` with the ordinary HIP-enabled
build command, then run `py -3 -B -m tools.native_coverage --hip-host`.
This explicit mode requires a current HIP build receipt and a real admitted
device before executing the same native registry and CLI contracts. It writes
separate evidence to `out/coverage-hip-host`; the default CPU-only CI report and
upload remain unchanged. This measures CPU instructions in HIP host orchestration,
including driver calls and resource management. OpenCppCoverage cannot measure
GPU device instructions; those still require the [device qualification](development/hip-device-coverage.md).
Collection never changes the build configuration or silently substitutes a CPU
execution path. Switching configurations still runs the selected build and
publishes a new qualification receipt. Matching per-configuration rebuild
evidence permits incremental compilation when source, toolchain, configuration
and outputs allow it; changed or unproven outputs still require a clean build.
Each configuration owns its runtime manifest alongside its binaries, so a Debug
collection cannot replace Release dependency identities. Packaging reads the
manifest for its selected configuration. Historical shared-manifest receipts
remain evidence only and cannot qualify a current build.
Use a separate checkout when simultaneous independent builds are necessary.

On an interactive Windows desktop, add `--gui` to run the existing all-page
GUI smoke under the same collector, for example
`py -3 -B -m tools.native_coverage --hip-host --gui`. Announce this run before
starting: it opens owned test windows and uses isolated smoke settings. Finish
the build first and do not rebuild or package until the windows have closed.
The smoke asserts interaction results, settings persistence, publication failures
and Neutron admission; inspect every generated capture as well. The added mode
uses the canonical bounded process runner, including its Windows PowerShell
environment handling. It requires measured GUI entry points and records the
GUI executable, PDB, scenario and imported UI helper identities. The collector passes its scenario deadline explicitly to the smoke driver. The
orphan-window watchdog includes that deadline and the driver's cleanup grace;
it cannot expire earlier than the still-active owning scenario. Native timer
parsing respects Windows' supported timer bound and rejects malformed or
overflowing input. A reached watchdog deadline is logged before orderly close.
A smoke failure or a changed helper invalidates collection. Before a new attempt, the collector
moves previous reports into an ignored local `previous-measurement-*` directory.
A failed retry must not leave an older qualifying summary at the current path.
The native/CLI collection keeps its 900-second outer deadline. Selecting GUI
adds that stage's 900-second budget, giving the combined transaction a bounded
1,800-second outer deadline. Individual scenario deadlines remain unchanged;
timeouts and incomplete runs still fail qualification.
The full smoke also opens real Windows multi-file and folder dialogs without
selection-result overrides. It checks spaced and Unicode paths, cancellation
with a populated filename field, unchanged Queue rendering, and a GUI-created
ZIP whose exact member paths and bytes are checked by the independent .NET ZIP
reader. Input controls must become visible and enabled, and acknowledge focus,
before receiving selection text; shell initialization must not overwrite it.
HWND ownership and native control classes are verified before input;
localized captions and personal file-list contents are not used as test data.
Evidence goes to `out/coverage-hip-host-gui` (or `out/coverage-gui` without HIP),
with `gui_host_coverage=true`. These local measurements do not replace the CI
badge or establish device-instruction coverage. The default collector does not
run the desktop GUI smoke. When the GUI is built,
native controller regressions create hidden windows on their own UI threads;
they use isolated temporary settings, never activate a window or move the pointer,
and join all workers before restoring that environment. The product and these
tests share the same `flitzzip_gui` static library and native resources. The tests
exercise real message dispatch, controlled thread admission refusal, subsequent
reuse, cancellation and responsive close. They supplement the visible all-page
smoke and screenshot review; they do not replace it.

Settings regressions observe rendered controls and persisted bytes independently:
drafts must not persist before Apply, failed file replacement must preserve the
last applied preferences, and a subsequent save and restart must recover normally.
Runtime logging must continue using the applied policy while a draft is edited,
abandoned or refused by persistence. Retention drafts must preserve existing rows;
successful settings publication applies the new pruning policy under the same
lock used by worker log events. These checks pair actual native commands with
independent persisted-byte, event-count and timestamped-row observations.
Extraction regressions wait for rendered Queue metadata before admission, preserve
existing files when confirmation is cancelled, keep overwrite approval local to
one operation, and check literal output bytes from batches with identical archive
names. Shared native fixtures retain ownership of windows, threads and off-screen
bitmaps; their message failures include the calling line and Windows error.

CLI presentation tests compare complete literal records, independently distinct
counters, optional phase presence and caller-owned stream failures. Shared output
projection must preserve command help, field names, ordering and unit conversions.
Component telemetry fixtures are not performance measurements; actual small CPU
RAM and ZIP readback controls qualify their production consumers separately.

Ordinary interaction regressions also check generated license headings, modal
input isolation, scroll endpoints, fractional wheel input, drag cancellation and
deferred button release. Queue tests compare literal rendered filenames against
an independent row model, keep checkbox inclusion separate from row focus, and
verify that removal and scrolling preserve the original files. Compression-form
tests check displayed formats, output extensions, numeric effort boundaries and
disabled controls; preferred-HIP policy must not offer Neutron star mode.

History regressions generate real Settings and ZIP results, check filter
conjunctions and preserve archive bytes across selection, scrolling and clearing.
The shared fixture owns the row model, control input and independent native text
observations. Text comparisons exclude control borders while retaining complete
glyphs, with wrong-value controls under keyboard focus. Drag tests check actual
Windows capture ownership, both scroll endpoints, cancellation and column bounds.
Required failure cases use real OS refusal at the affected boundary; an existing
output file alone does not establish a failure when replacement is supported.

Job-presentation regressions hold real work at thread admission and observe the
actual completion notification before checking summaries or destination requests.
Preferences take effect at job admission; unapplied drafts and later jobs must not
reuse a previous result. Active-job navigation remains blocked by product policy.
An instance-local FIFO fence acknowledges a separate posted marker and forwards
product messages unchanged; it never forces a completion handler or repaint.
Its context outlives the window and is removed during native destruction.
Destination assertions preserve the complete input path and account independently
for persistent-log escaping. Failed and cancelled jobs must not request opening
a destination, and successful requests are consumed once.

An owned HWND can exist before its initialization finishes. The hidden-window
fixture therefore uses its original creation deadline for the initial native
acknowledgement, then keeps the shorter normal-input responsiveness deadline.
Controlled native-window tests retain the premature-check rejection, bounded
startup refusal and successful recovery. This separates test readiness from
product responsiveness; it does not relax the latter or resolve unrelated hangs.

`tools/native_coverage.py` verifies the native build receipt, all selected
executables, their symbols and scenario drivers before and after collection.
It retains process diagnostics locally within its 8 MiB output budget, separately
from the MCP protocol's shorter response cap. Truncation metadata remains explicit.
`tools/native_coverage_report.py` rejects empty, foreign,
ambiguous, malformed, or source-inconsistent reports and normalizes paths without
changing measured hits. The workflow retains raw XML, normalized XML, diagnostics,
and source/tool identities for seven days. Inspect the `native-cpu-coverage`
artifact when investigating a coverage change or upload failure.

Codecov receives only `out/coverage/native.xml`, using GitHub OIDC and a pinned
action/CLI. Upload failures fail the check. Fork pull requests retain the report
as an artifact and omit the authenticated upload. No repository upload token is
stored in source or workflow configuration.

Codecov applies its documented [report adjustments](https://docs.codecov.com/docs/fixing-reports),
including removing C++ brace-only lines. Its displayed totals can therefore
differ from the retained collector report. Compare like-for-like measurements
and report the actual Codecov result separately from raw collector totals.

`summary.json` lists every measured zero-hit line by file and the native source
files absent from the report. The latter inventory comes from the same frozen
build inputs, not a manually selected coverage list. Review both inventories:
100% of instrumented lines would still not establish whole-repository coverage.
Earlier optimized reports can lack reliable inline-code mappings and are not
directly comparable with Debug reports. Do not manufacture hits from successful
test names or collector heuristics. The collector's
[Debug guidance](https://github.com/OpenCppCoverage/OpenCppCoverage/wiki#release-versus-debug)
explains the mapping limitation.

### Investigating Zero-Hit Lines

Classify each gap against the exact source, binary and raw report before adding
tests. OpenCppCoverage's [FAQ](https://github.com/OpenCppCoverage/OpenCppCoverage/wiki/FAQ)
documents PDB mappings to unreachable exception epilogues and brace instructions.
This does not establish that any particular missing line is harmless. Trace the
mapped instruction or source region: a real unexecuted condition, cleanup action
or callback still needs a meaningful behavior test. A passing surrounding
function is insufficient evidence. Preserve the original report and do not
exclude braces, manufacture hits or reshape production control flow for counters.

An independent [source-counter measurement](https://clang.llvm.org/docs/SourceBasedCodeCoverage.html)
can help distinguish mapping artifacts from behavior gaps. A small calibration
does not qualify a replacement collector. Before adopting one, validate its
compiler/runtime identities, opposite branch controls, exceptions, linked-module
ownership, profile shutdown and merging, then replay the native, CLI and GUI
consumers in their actual configurations. Keep HIP host and device measurements
separate. Different mapping schemes have different denominators; compare their
scopes explicitly and retain the existing CI report until replacement is qualified.

## Python contract measurements

Python tooling and MCP measurements are separate from the native Codecov badge.
Freeze the selected production modules, tests, collector configuration and tool
identity. Select the appropriate offline or installed-provider contracts from
the verifier and retained measurement manifest; blanket test discovery does not
provision foreign provider dependencies or a Linux fuzz-build environment.
Keep those runtime qualification scopes explicit rather than silently skipping
them or claiming their coverage from unrelated tests.

Use coverage.py's standard subprocess tracing for same-interpreter children.
With parallel collection, combine the completed commands before reporting. When
adding observations to an existing consolidated data file, use
`coverage combine --append --keep`; a plain combine starts a new consolidated
result and can discard earlier observations. `coverage run --append` cannot be
used in parallel mode.

Reporting commands also combine parallel data automatically. Preserve raw files
during JSON export with `coverage json --keep-combined -o coverage.json`;
`combine --append --keep` does not control a later reporting invocation. See the
[JSON command](https://coverage.readthedocs.io/en/7.16.2/commands/cmd_json.html)
and [reporting change log](https://coverage.readthedocs.io/en/7.16.2/changes.html#version-7-15-0-2026-07-02).
Before and after each export, require the same complete raw-profile path and
SHA-256 inventory and unchanged consolidated-data SHA-256. Retain raw data and a
consolidated snapshot so an export or supplemental test cannot silently replace
the cohort. A small independent control should demonstrate loss without append,
preservation with it, and raw-file preservation across repeated exports.

Path aliases may unify exact source copies used by isolated process tests only
when the fixture proves byte equality with the frozen production inputs. Never
alias a test double or modified copy to a production module. Report production
totals separately from measured test bodies, retain zero-hit production lines,
and disclose line and branch results independently.

Resolve coverage data from the declared configuration, rather than assuming
that it lives beside the configuration file. Check that boundary with a real
export whose configuration and data directories differ. Require export to
preserve both the raw-profile inventory and consolidated bytes.

A zero-byte raw artifact contains no persisted observations. Preserve its exact
bytes and identify it as unbound; do not invent schema rows, execution hits or a
process identity. Keep this case distinct from a nonempty database with no
observations. Nonempty unknown schemas, malformed data and inconsistent rows
must still fail admission.

After a reader-only repair, completed tests need not run again when their
relevant source, tracer, configuration, process results and original raw bytes
remain verifiably unchanged. Retain the original failed collection, identify
the replacement reader, and record reanalysis separately from new execution.
For device profiles, cumulative reuse also requires identical device images
and compatible native counter identities; a cumulative union does not establish
that every retained test ran against the latest host binary.

## Meaningful tests

Choose a missing behavior or concrete failure mode before writing a test. Prefer
independent expected results, external-format interoperability, malformed input,
boundary sizes, lifetime transitions, cancellation, and corruption followed by
valid reuse. Prove a regression test fails with the defect present. Do not copy
the implementation into assertions, remove untested code from reports, or add
tests whose only purpose is increasing a percentage. A coverage increase without
new behavioral evidence is not an engineering improvement.

Assert the actual cardinality of pipeline phases and artifacts. When a contract
requires exactly one command, profile or output, a missing or duplicate match
must fail the test; selecting the first match cannot establish uniqueness.
For deliberately ordered selections that allow multiple matches, check absence
explicitly and retain the intended selection order.

### Oracle Selection

Choose the weakest plausible implementation that the test must reject. If that
implementation still passes, strengthen the observation rather than adding more
inputs to the same ineffective assertion.

| Behavior | Useful oracle and rejection control |
| --- | --- |
| Archive compatibility | Independently framed bytes or another supported implementation; a same-library round trip can share an encoder/decoder defect |
| Parser bounds | Valid records beside truncation, overflow and inconsistent-length cases; assert output preservation on failure |
| Ownership and publication | Real handle/file identity, final release, unchanged existing bytes and successful reuse after interruption |
| Stateful logic | An independently specified transition model; exercise failed admission, cancellation and recovery, not only the final flag |
| Concurrency | Controlled event ordering and actual completion/lifetime assertions; elapsed sleeps alone do not prove ordering |
| Tool orchestration | Actual command selection, refused stale identities and preserved failed outputs, rather than matching the implementation's text |

Generated tests complement these oracles. Use bounded domains, reproducible seeds
and minimized failing inputs; retain each confirmed failure as a deterministic
regression. Property and metamorphic assertions need an independent justification:
for example, preserving a file's bytes after rejection is stronger than merely
calling the same decoder twice. [Hypothesis](https://hypothesis.readthedocs.io/en/latest/quickstart.html)
and [RapidCheck](https://github.com/emil-e/rapidcheck) illustrate generation and
reduction; their documentation does not require adding either dependency here.

Use a targeted old-source negative control or isolated behavioral mutation when
it resolves uncertainty about an assertion. The [mutation-testing method](https://mull.readthedocs.io/en/latest/MutationTestingIntro.html)
checks sensitivity to changed behavior; surviving mutations need investigation,
and equivalent changes are not automatically missing tests. This is not a new
mandatory mutation tool or a reason to rewrite product behavior for coverage.

[Google's coverage guidance](https://testing.googleblog.com/2020/08/code-coverage-best-practices.html)
distinguishes execution from test quality, and [LLVM's coverage documentation](https://clang.llvm.org/docs/SourceBasedCodeCoverage.html)
distinguishes line, region and branch measurements. Apply the
[release acceptance policy](#release-acceptance) while keeping assertions,
measurement validity and unmeasured code visible as separate questions.

### Batch Development

Develop coverage in substantial, coherent batches from the retained uncovered-line
inventory. Define independent assertions and write the planned behavior cases
before measuring the batch; test quantity is not evidence of coverage or quality.
Compile the additions together and retain the result of each independently bounded
case. Stop a cohort if process ownership, deadlines or resource containment fail;
ordinary assertion failures remain visible for grouped diagnosis.

Use the resulting gap inventory to prioritize the next test additions. Group
exposed defects by their first violated contract for subsequent repair batches,
retaining the original failures and regression assertions. Correct harness defects
that prevent valid observation without weakening the product contract. A report
from failing tests remains diagnostic evidence, never a passing qualification.
Avoid complete recollection after individual assertions. Keep all planner-selected
checks pending until the batch is qualified, and perform the accumulated final
suites, hosted checks and release gates before acceptance. Focused development
results never replace that final qualification.

The canonical `Invoke-FlitzZipNativeTestSelection` executor in
`tools/native_test_selection.ps1` supports an opt-in
`-ContinueAfterAssertionFailure` development mode. It continues only when the
native runner reports one exact selected case and one ordinary assertion-failure
summary with the matching failure exit status. It retains each case's output,
checks required backend readiness after a failure, and throws a combined failure
at the end. Conflicting summaries, unexpected exits, missing cases and backend
loss stop immediately. Default verification remains fail-fast. Run either mode
inside the existing bounded process owner; the option does not add a timeout,
retry work, broaden the selected cohort or replace receipt validation.
The runner's complete failure result is not a root-cause classification: review
its diagnostics before deciding whether the product, assertion or fixture failed.

For focused native runs, use `^prefix_` to select a test-name prefix or `=full_name`
for one exact case. Unadorned filters retain substring matching and can cross
module boundaries: a format label may also occur inside another codec or mode
name. Check the emitted selected names before interpreting the cohort's scope.

### Research And Tool Adoption

Current upstream documentation and the primary studies below inform test design.
The research was checked with the repository's isolated Crawl4AI provider in
October 2026; native captures, access results and content digests remain in local
evidence. Current documentation is not proof that a different tool version is
installed or qualified here. Retain the repository's pinned runtimes and avoid
introducing a framework solely to reproduce a paper's workflow.

| Decision | Primary evidence and application here |
| --- | --- |
| Separate execution gaps from bug-detection strength | [CoverUp, FSE 2025](https://doi.org/10.1145/3729398) supports coverage-guided contextual feedback, but its regression-generation premise can assume existing source is correct. Keep independently specified assertions; never ratify current behavior just to turn a test green. |
| Check whether an assertion detects a relevant fault | [Meta's 2025 mutation-guided testing work](https://engineering.fb.com/2025/02/05/security/revolutionizing-software-testing-llm-powered-bug-catchers-meta-ach/) motivates targeted fault controls. Use isolated, reviewed mutations or original failing source; a killed mutant does not establish universal correctness. Inspect equivalent or irrelevant mutants instead of chasing their count. |
| Distinguish state space from input size | [Hypothesis stateful testing](https://hypothesis.readthedocs.io/en/latest/stateful.html) and [RapidCheck](https://github.com/emil-e/rapidcheck) motivate bounded action sequences and shrinking. Model Queue membership, publication ownership and cancellation independently. Preserve the reduced input, operation order and generator identity, not only a random seed. |
| Control the source of nondeterminism | [Hypothesis flaky failures](https://hypothesis.readthedocs.io/en/latest/tutorial/flaky.html) and [pytest's diagnosis guidance](https://docs.pytest.org/en/stable/explanation/flaky.html) distinguish global state, timing and lifetime causes. Own clocks, gates, temporary files and native handles; assert observable readiness and cleanup. A later pass does not explain a failure. |
| Measure the right denominator | [Clang source coverage](https://clang.llvm.org/docs/SourceBasedCodeCoverage.html), [LLVM libc coverage](https://libc.llvm.org/dev/code_coverage.html) and [coverage.py branches](https://coverage.readthedocs.io/en/latest/branch.html) distinguish lines, branches and condition independence. Keep uninstrumented files, device instructions and unreachable compiler mappings explicit. Do not label line coverage as branch or MC/DC coverage. |
| Use public behavior and readable data tables | [Google's behavior-focused tests](https://testing.googleblog.com/2013/08/testing-on-toilet-test-behavior-not.html), [GoogleTest parameterization](https://google.github.io/googletest/advanced.html) and [Google's unit-testing guidance](https://abseil.io/resources/swe-book/html/ch12.html) support stable consumer assertions and reusable fixtures. Keep the native test harness; apply the design principles without importing GoogleTest. |
| Cover interactions without blind Cartesian expansion | [NIST combinatorial testing](https://csrc.nist.gov/Projects/automated-combinatorial-testing-for-software) motivates explicit interaction matrices. Review boundaries and known higher-order interactions separately; pairwise input coverage alone does not prove archive or concurrency correctness. No new combinatorial runtime is required. |
| Preserve cheap feedback and necessary integration | [Google's larger-test guidance](https://abseil.io/resources/swe-book/html/ch14.html) and [Microsoft's testing strategy](https://learn.microsoft.com/en-us/azure/well-architected/operational-excellence/testing) motivate narrow contract tests plus real consumers. Use the changed-contract planner; do not import a fixed test-percentage quota or discard required tests because they are expensive. |
| Keep repeat experiments distinct from qualification | [CTest repeat modes](https://cmake.org/cmake/help/latest/manual/ctest.1.html) distinguish failure hunting from retry-until-pass. Use a bounded repeat only to test a stated scheduling hypothesis. [Resource locks](https://cmake.org/cmake/help/latest/prop_test/RESOURCE_LOCK.html) illustrate exclusive ownership; retain this repository's serial GPU and shared-memory policy. |
| Keep startup and live-input contracts distinct | Microsoft's [window lifecycle](https://learn.microsoft.com/en-us/windows/win32/winmsg/using-windows) and [SendMessageTimeout](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagetimeoutw) document native creation and dispatch semantics. A discovered window handle is not a completed initialization. Test both bounded startup refusal and responsiveness after admission. |

[Google's coverage guidance](https://testing.googleblog.com/2020/08/code-coverage-best-practices.html)
supports using coverage as actionable evidence rather than a complete quality
score. The maintained [Hypothesis changelog](https://hypothesis.readthedocs.io/en/latest/changelog.html)
also documents generator and stateful-testing corrections as recently as October
2026. Generator versions and replay inputs therefore belong in evidence;
upgrading dependencies is a separate compatibility decision.

### Behavioral Contract Examples

Container admission tests compare plain and compressed readers at structural
truncation boundaries, preserving existing destination bytes on rejection.
CAB and XAR fixtures additionally exercise binary header fields, optional reserve
regions and compressed-stream extents through their public extraction APIs.
Publication tests use real Windows sharing denial and callback cancellation,
then require successful reuse after cleanup. Native metadata tests exercise
the public verification and extraction paths with independently framed records;
malformed GPU block metadata must be rejected before decoder execution. These
tests complement actual HIP execution and independent format interoperability.

Publication lifetime tests interrupt every observed checkpoint, reject forged
reservation fields, and check that read-only files and directory junctions are
cleaned without changing outside files. Source-session tests independently vary
each recorded identity field, mutate real files in place, and verify that copied
or moved locks prevent writes until their final owner releases them. These are
behavioral assertions over the production boundary, not inferred coverage hits.
Controlled container-allocation failures also verify that source handles already
have RAII owners before insertion, so unwinding closes the new handle without
releasing previously retained locks.

Folder-cache regressions calibrate real CRT allocation refusal on the current
thread in MSVC Debug builds, without exhausting host memory. They require long-key
lookup, completion, invalidation and duplicate admission to use existing storage.
The preceding CRT hook is restored before assertions; Release runs retain the
same state-transition assertions without claiming that Debug failure control.
Scope allocation refusal to the component's ownership and exception contracts.
Debug-library bookkeeping can allocate inside nonthrowing standard-library
operations; a global refusal sweep does not automatically model Release failure
semantics. Retain such diagnostics, distinguish runtime limitations from product
defects, and qualify the relevant production configuration when necessary. Do not
disable iterator verification or rewrite valid application behavior merely to
make an unsupported injection sweep pass.

Native index I/O tests use independently packed wire bytes across every readable
version. Forward-only streams exercise every truncation boundary without relying
on seek-based size checks. Partial-seek fixtures also reject lost positions and
require unsupported end queries to preserve the caller's exception policy and
next unread byte. Bounded output streams fail at each byte position,
checking error propagation with and without stream exceptions and exact wire
output on success. Invalid identities and allocation counts fail before the
affected metadata is consumed. These fixtures remain in RAM and do not simulate
failure by exhausting storage or memory.

The collector also runs the executable's CLI admission contracts. These reject
malformed Unicode, integer overflow, missing values, unknown options and
incompatible format/GPU settings before work begins. Each case checks the exit
status, diagnostic, absence of success output and preservation of caller files;
unexpected publication debris fails the run. The fixtures are small local
correctness inputs, not benchmark workloads.

The requested target is complete meaningful coverage. Work from the gap inventory
in coherent behavior groups, retaining malformed-input, independent-oracle and
failure-recovery assertions. Keep device instructions and unmeasured executables
visible until a suitable measurement mechanism is qualified. Resource exhaustion,
driver hangs and destruction of host state are not acceptable coverage techniques.

Complete GUI/HIP-host qualification requires the native registry, CLI contracts
and all-page interactive smoke on one admitted source identity. Review every
capture, including compact layouts, Neutron selection, overwrite confirmation,
scrollable tables, monitor graphs and both license tabs. Focused runs qualify
their exercised paths; a failed combined transaction is not a complete coverage
measurement. Keep current source-bound results and remaining gaps in collector
artifacts rather than reusable instructions.

## Device counter qualification

Use the [HIP device coverage guide](development/hip-device-coverage.md) for profiler/toolchain
qualification, production object admission and bounded real-device measurement.
Its explicit tool, source, image and profile checks remain required.
