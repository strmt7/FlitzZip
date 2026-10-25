# Code coverage

The README Codecov badge links to measured native CPU line coverage. Coverage
helps locate untested behavior; it does not establish correctness, security,
performance, or a release qualification by itself.

## Coverage Follow-up

The maintainer has reactivated coverage-gap closure. Preserve the existing
collectors, tests and CI uploads, and work through coherent groups of missing
behavior with independent correctness, failure and recovery assertions.

Local GUI/HIP-host measurement is retained under `out/coverage-hip-host-gui`.
Review its `summary.json`, uncovered-line inventory and raw report before choosing
additional tests. Its recorded source, binary, collector and scenario identities
determine whether it measures the current checkout; the directory name alone
does not establish freshness. Reuse individual evidence only after checking
those identities. The hosted
CPU report, GUI/HIP-host report, tooling and device instructions have distinct
scopes; none establishes whole-repository coverage.

Device measurement must first pass the helper-inclusive qualification control
described below, including the pinned compiler's registration repair. Unmodified
compiler output omits helper counters; the repaired measurement mechanism has
passed independent kernel/helper controls. Product device coverage remains
unfinished until its own behavior and measurement gaps close.

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
GPU device instructions; those still require the qualification described below.
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
distinguishes line, region and branch measurements. Retain the maintainer's
complete-coverage target while keeping assertions, measurement validity and
unmeasured code visible as separate questions.

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

HIP device measurement requires both compiler instrumentation and an AMDGPU
profiling runtime. The product SDK remains pinned independently from development
coverage tools. Its existing Windows profiling support cannot produce the required
device report by itself. LLVM documents the
[AMDGPU runtime and collection prerequisites](https://clang.llvm.org/docs/HIPSupport.html#source-based-code-coverage-for-device-code);
its [Windows GPU profiling repair](https://github.com/llvm/llvm-project/pull/202095)
also fixes registration of the device profile sections.

`tools/hip_coverage.py` qualifies the measurement mechanism with a 768-byte GPU
operation across three workgroups, including a non-inlined device helper.
Separate processes take opposite branches, verify the independently expected
output and report the actual GPU architecture. The
tool extracts the AMDGPU ELF, merges native device profiles with LLVM and requires
counter pairs `(0, 192)`, `(192, 0)` and `(192, 192)` for the separate and combined
runs. The helper's opposite branch predicate must independently produce the
reversed pair in each separate run and the same merged pair. Missing, duplicated,
zero or incorrectly mapped helper counters fail qualification even when kernel
counters and readback are correct. It disables offload PGO sampling explicitly with
`-mllvm -offload-pgo-sampling=0`; the compiler otherwise samples one in eight
workgroups, which is unsuitable for coverage. The multiple-workgroup control
rejects that undercount, which a single-thread probe would miss.
Host executable mappings, an absent device profile, wrong readback, changed input
bytes, truncated diagnostics or incorrect counters fail qualification. Complete
LLVM JSON exports use an eight-MiB bounded artifact writer inside the existing
Windows process job; response-tail limits remain unchanged. Raw native
profiles, exports and failed-command diagnostics remain in the selected ignored
output directory. Existing output directories are never overwritten.
Before GPU execution, the qualifier freezes the probe source and records compiled
input and executable identities in `measurement-inputs.json`, explicitly marked
`not-qualified`. Image, execution, export or counter failures retain this evidence
without a pass receipt. Source drift during compilation or measurement prevents
acceptance; successful receipts also bind the frozen source artifact.

Kernel counters alone do not qualify device helpers or measure FlitzZip's
complete GPU implementation.
[Windows registration in this compiler](https://github.com/ROCm/llvm-project/blob/d6f6cb691863e9258034ab48c1dfce21689d5606/clang/lib/CodeGen/CGCUDANV.cpp#L1444)
enumerates emitted kernels; whether that explains a particular missing helper
counter requires separate source and runtime evidence. Product kernel reports
remain separate work. Kernel dispatch counts and CPU-side HIP API coverage
cannot substitute for device counters.

The helper-inclusive control fails with the tested unmodified ROCm 10.1
toolchain: GPU readback and kernel counters are correct, but the executed
non-inlined helper retains zero counters. This is a measurement limitation,
not evidence that those instructions were unexecuted or that the product GPU
operation failed. The emitted device IR contains the helper's counter increments.
The native profiler diagnostics report two device data records and four counters,
but copy only one record and two counters through the registered-section path;
`llvm-profdata show --all-functions --counts` contains only the kernel.
The matching `InstrProfilingPlatformROCm.cpp` copies the compiler-provided
registered section ranges. Preserve the IR, native diagnostics and raw profiles
when repairing this registration boundary; do not substitute another source
revision's runtime layout or copy unregistered device addresses.
Preserve that failed control and require correct helper
registration and counters before admitting whole-device coverage. Inlining
helpers solely to satisfy the control or deriving counts from dispatches would
not qualify the missing measurement.

`tools/hip_profile_registration` repairs this compiler emission boundary through
LLVM's typed IR and ELF APIs. It inventories the actual linked profiling records,
requires complete, nonoverlapping section coverage, and reconciles ordinary HIP
variable registrations and profiler section notifications with that inventory.
Some template kernels have host references to profiling globals absent from the
linked image. Only those validated profiling registrations are retired; every
original program value and instruction outside that metadata repair is preserved.
Missing actual profiling records receive normal registrations. The qualifier also extracts
the relinked device image and requires byte-for-byte equality before executing it.
The supported input is one static, non-RDC translation unit with the reviewed
Windows x64/AMDGPU profiling ABI. Unsupported layouts fail explicitly.

Added host shadows include the validated translation-unit identity. Helpers
shared by independent device images must retain distinct host registrations;
coalescing them can prevent a multi-unit kernel DLL from linking.

The compiler phases share an explicit `-cuid` selected before compilation. The
host-only frontend consumes the original source and that compilation's device
bundle. Temporary preprocessed-file compilation is unsuitable here because it
changes source mapping. LLVM's table suffix is an unpadded hexadecimal hash;
do not assume a fixed number of characters or substitute it for the driver's CUID.

The native tool's identity binds its exact C++ source, CMake recipe and reviewed
LLVM revision. Build and qualification consume the same validated
`tools/hip-coverage-toolchain.json` provenance record; the compiled identity also
binds that manifest and its parser. Before GPU execution, the qualifier automatically runs native
success, complete-IR idempotence, existing-output preservation and malformed-input
controls. Wrong contexts, flags, pointer storage, extents, section groups, source
identities and incomplete device symbols must fail without publishing output.
Raw failures remain in the owned evidence directory.

Completion reports must form a continuous inventory chain: the first report's
input matches the original inspection, its output matches the next inspection,
and the repeated completion matches the persisted output. Compare complete
inventories, not just their counts or IR digests. Existing-output refusal must
retain complete process diagnostics and unchanged destination bytes; a timeout,
truncated response or exceeded output limit cannot qualify that control.
The native tool parses its newly written bitcode to compute the output inventory
and digest, and requires all registration fields to survive serialization unchanged.

Idempotence compares LLVM's complete printed IR, excluding only its initial
input-filename comment. Source filenames, targets, instructions, attributes,
profiling metadata and embedded device bytes remain in the digest. Equivalent
LLVM bitcode can have different serialization order; the linked GPU executable
still requires byte-for-byte preservation.

The repaired mechanism has also passed a separate control with two kernels,
shared helpers with external, internal and inline linkage, and a deliberately
unentered helper. Independently expected readback, invocation counts, branch
counts and zero hits distinguish working measurement from inflated coverage.
These controls qualify the measurement mechanism; they do not measure all
FlitzZip kernels or establish release readiness.

Prepare an isolated coverage compiler from AMD's
[ROCm 10.1 distribution](https://rocm.docs.amd.com/en/docs-10.1.0/install/rocm.html).
The tested compiler source is the ROCm LLVM commit
`d6f6cb691863e9258034ab48c1dfce21689d5606`, identified by that distribution's
manifest and `clang++ --version`. Keep its LLVM license and LLVM exceptions with
the development installation. These tools and runtime are not shipped with the
application. A generic LLVM Windows distribution may omit the AMDGPU backend;
the qualifier checks `--print-targets` before compiling.

Build `clang_rt.profile-amdgpu` from that exact, unmodified
[ROCm LLVM source](https://github.com/ROCm/llvm-project) using its
`compiler-rt/cmake/caches/AMDGPU.cmake` cache. From an existing compatible x64
Visual Studio developer shell, set the following variables to your isolated
compiler, source checkout, Ninja, CMake, build directory and actual GPU target:

```powershell
& $coverageCmake -S "$coverageSource/runtimes" -B $coverageBuild -G Ninja `
  -C "$coverageSource/compiler-rt/cmake/caches/AMDGPU.cmake" `
  "-DCMAKE_MAKE_PROGRAM=$coverageNinja" `
  "-DCMAKE_C_COMPILER=$coverageCompiler/bin/clang.exe" `
  "-DCMAKE_CXX_COMPILER=$coverageCompiler/bin/clang++.exe" `
  "-DCMAKE_ASM_COMPILER=$coverageCompiler/bin/clang.exe" `
  "-DCMAKE_AR=$coverageCompiler/bin/llvm-ar.exe" `
  "-DCMAKE_RANLIB=$coverageCompiler/bin/llvm-ranlib.exe" `
  -DCMAKE_SYSTEM_NAME=Generic -DCMAKE_SYSTEM_PROCESSOR=amdgcn `
  -DCMAKE_C_COMPILER_TARGET=amdgpu-amd-amdhsa `
  -DCMAKE_CXX_COMPILER_TARGET=amdgpu-amd-amdhsa `
  -DCMAKE_ASM_COMPILER_TARGET=amdgpu-amd-amdhsa `
  "-DCMAKE_C_FLAGS=--target=amdgpu-amd-amdhsa -mcpu=$coverageArch -nogpulib -ffreestanding" `
  "-DCMAKE_CXX_FLAGS=--target=amdgpu-amd-amdhsa -mcpu=$coverageArch -nogpulib -ffreestanding" `
  "-DCMAKE_ASM_FLAGS=--target=amdgpu-amd-amdhsa -mcpu=$coverageArch -nogpulib" `
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DCMAKE_BUILD_TYPE=Release `
  '-DLLVM_ENABLE_RUNTIMES=compiler-rt;libc' -DRUNTIMES_USE_LIBC=llvm-libc `
  -DLLVM_DEFAULT_TARGET_TRIPLE=amdgpu-amd-amdhsa -DLLVM_RUNTIMES_TARGET=amdgpu-amd-amdhsa `
  -DLLVM_LIBC_FULL_BUILD=ON -DLLVM_INCLUDE_TESTS=OFF -DLIBC_INCLUDE_TESTS=OFF `
  "-DLIBC_LLVM_LINK=$coverageCompiler/bin/llvm-link.exe" `
  -DCOMPILER_RT_INCLUDE_TESTS=OFF -DCOMPILER_RT_BUILD_BUILTINS=OFF `
  -DCOMPILER_RT_BUILD_SANITIZERS=OFF -DCOMPILER_RT_BUILD_CTX_PROFILE=OFF
& $coverageCmake --build $coverageBuild --target clang_rt.profile-amdgpu
```

The omitted upstream runtime test targets are build options for this isolated
profiling library; FlitzZip's product tests and security workflows remain enabled.
Build the registration tool as a separate LLVM external project from the same
clean source checkout. Use a fresh tool build directory, the existing compatible
x64 developer shell, the pinned CMake/Ninja, and the selected coverage compiler.
Set `$repoRoot` to this checkout and `$coveragePython` to a supported Python
interpreter. This builds the required LLVM libraries and helper, without adding
LLVM to the product or installing a different system compiler:

The reader needs zlib for LLVM's compressed profile names and coverage mappings.
The shared toolchain manifest pins its official source archive, size and digest.
Prepare fresh, isolated `$coverageZlibSource`, `$coverageZlibBuild` and
`$coverageZlibPrefix` directories and an unused `$coverageZlibArchive` path:

```powershell
& $coveragePython (Join-Path $repoRoot 'tools/hip_coverage_toolchain.py') --field revision
if ($LASTEXITCODE -ne 0) { throw 'Coverage provenance admission failed.' }
$zlibPin = (Get-Content (Join-Path $repoRoot 'tools/hip-coverage-toolchain.json') -Raw | ConvertFrom-Json).zlib
Invoke-WebRequest -Uri $zlibPin.url -OutFile $coverageZlibArchive
if ((Get-Item -LiteralPath $coverageZlibArchive).Length -ne $zlibPin.bytes -or
    (Get-FileHash -LiteralPath $coverageZlibArchive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $zlibPin.sha256) {
  throw 'Coverage zlib archive identity mismatch.'
}
New-Item -ItemType Directory -Path $coverageZlibSource -ErrorAction Stop | Out-Null
tar -xf $coverageZlibArchive -C $coverageZlibSource --strip-components=1
if ($LASTEXITCODE -ne 0) { throw 'Coverage zlib extraction failed.' }
& $coverageCmake -S $coverageZlibSource -B $coverageZlibBuild -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$coverageNinja" -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL -DZLIB_BUILD_SHARED=OFF `
  -DZLIB_BUILD_TESTING=ON "-DCMAKE_INSTALL_PREFIX=$coverageZlibPrefix"
if ($LASTEXITCODE -ne 0) { throw 'Coverage zlib configuration failed.' }
& $coverageCmake --build $coverageZlibBuild --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Coverage zlib build failed.' }
& (Join-Path (Split-Path $coverageCmake) 'ctest.exe') --test-dir $coverageZlibBuild --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Coverage zlib tests failed.' }
& $coverageCmake --install $coverageZlibBuild
if ($LASTEXITCODE -ne 0) { throw 'Coverage zlib installation failed.' }
```

Preserve the archive's complete license with this development installation.
The static Windows library is named `zs.lib` by this pinned zlib release; supply
it explicitly because older CMake find-module naming lists may omit that name.

```powershell
$registrationSource = Join-Path $repoRoot 'tools/hip_profile_registration'
& $coverageCmake -S "$coverageSource/llvm" -B $registrationBuild -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$coverageNinja" `
  "-DCMAKE_C_COMPILER=$coverageCompiler/bin/clang.exe" `
  "-DCMAKE_CXX_COMPILER=$coverageCompiler/bin/clang++.exe" `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL `
  '-DLLVM_TARGETS_TO_BUILD=' '-DLLVM_ENABLE_PROJECTS=' '-DLLVM_ENABLE_RUNTIMES=' `
  -DLLVM_EXTERNAL_PROJECTS=FlitzZipProfileRegister `
  "-DLLVM_EXTERNAL_FLITZZIPPROFILEREGISTER_SOURCE_DIR=$registrationSource" `
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF `
  -DLLVM_INCLUDE_DOCS=OFF -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_ENABLE_ZLIB=FORCE_ON `
  "-DZLIB_ROOT=$coverageZlibPrefix" "-DZLIB_LIBRARY=$coverageZlibPrefix/lib/zs.lib" `
  -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_LIBEDIT=OFF `
  "-DPython3_EXECUTABLE=$coveragePython"
if ($LASTEXITCODE -ne 0) { throw 'HIP profiling registration configuration failed.' }
. (Join-Path $repoRoot 'tools/build_parallelism.ps1')
$registrationJobs = Resolve-BuildParallelism
Invoke-FlitzZipParallelBuild -Jobs $registrationJobs -Action {
  & $coverageCmake --build $registrationBuild --target flitzzip_hip_profile_register --parallel $registrationJobs
  if ($LASTEXITCODE -ne 0) { throw 'HIP profiling registration tool build failed.' }
}
```

Configuration rejects another LLVM revision or modified LLVM/runtime sources.
Preserve upstream LLVM notices with the development installation. The helper
uses this repository's license and is not shipped with the application.
Run the qualification with explicit paths, using the existing product SDK for
HIP headers and runtime. No machine-specific installation path is embedded:

```powershell
py -3 -B -m tools.hip_coverage --compiler-root $coverageCompiler --sdk $productSdk `
  --device-runtime "$coverageBuild/compiler-rt/lib/generic/libclang_rt.profile-amdgpu.a" `
  --registration-tool "$registrationBuild/bin/flitzzip_hip_profile_register.exe" `
  --arch $coverageArch --output out/hip-coverage-qualification
```

Repeat qualification when the compiler, profiler, runtime or device changes.
Run the offline counter-admission regressions with
`py -3 -B -m unittest tools.test_hip_coverage tools.test_hip_coverage_registration`; the existing component-contract
workflow selects these tests when their inputs change and never launches a GPU.

## Tool provenance and local use

### Production kernel objects

After the helper-inclusive qualification passes, prepare profiling objects from
the five canonical product HIP translation units:

```powershell
py -3 -B -m tools.hip_coverage_product build `
  --qualification out/hip-coverage-qualification/qualification.json `
  --output out/hip-product-objects
```

Configure a separate development build with
`-DFLITZZIP_HIP_COVERAGE_MANIFEST=<absolute-path>/product-objects.json`, the same
SDK and the qualified single GPU architecture. The manifest admits exact
source, repository/SDK headers, compiler, profiler, object and device-image
identities. CMake checks those identities again before linking the canonical
kernel DLL, whose normal generated checksum and POD ABI admission remain active.
No executable or DLL is replaced beneath an already qualified loader.
Object preparation binds its own compiler recipe. CMake linking, lifecycle code
and test consumers need their own source-bound measurement; an install-rule edit
does not change the standalone object's compiler inputs.

The supplied Windows ROCm host profiler uses the dynamic CRT. This development
DLL therefore consistently uses that CRT for its instrumented objects and module
entrypoint. Ownership and exceptions remain within the existing POD boundary.
The ordinary product DLL keeps its static CRT and pinned compiler path.
The profiling runtime uses explicit initialization and collection: the
executable's module owner drains device counters before releasing its verified
DLL. Automatic profiler exit callbacks must not perform HIP work during DLL
detach, where [Windows holds the loader lock](https://learn.microsoft.com/en-us/windows/win32/dlls/dllmain).
Missing lifecycle exports and
collection errors fail measurement rather than silently dropping profiles.
Instrumented builds cannot receive a production build receipt, install or enter
CPack. Run them only for measurement, never performance comparisons or release
qualification. Local consistency receipts are not authenticated attestations.

Preparing objects performs no GPU execution. Product device coverage still
requires the real test consumers, retained raw profiles, complete per-image LLVM
exports and source-bound results; successful compilation alone does not qualify it.

After configuring the isolated HIP build, explicitly build and measure selected
regressions with the portable consumer:

```powershell
py -3.13 -B -m tools.hip_coverage_measure build-and-measure `
  --manifest out/hip-product-objects/product-objects.json `
  --build-root out/hip-product-measurement `
  --filter '=neutron_dictionary_gpu_matches_exhaustive_oracle' `
  --filter '=gpu_diagnostic_checksum_independent_sum_and_guards' `
  --output out/hip-device-measurement
```

This command refreshes the existing configured `Debug` test target through the
pinned CMake launcher and shared memory policy. It verifies all five complete
device bundles in the linked DLL before execution, requires actual HIP admission
and every translation unit's profile. LLVM's native profile reader associates
each raw profile with exactly one ELF using its complete name-reference, CFG-hash
and counter-extent inventory. Only profiles from the same image are merged;
cross-image function-name collisions must not create or hide mapping mismatches.
Every image receives its own complete native export, with diagnostics blocking
acceptance. LLVM's line iterator supplies exact mapped-line hits; the product
summary unions hits by source path and line, counting shared headers once.
This distinct-source-line metric can differ from LLVM's instantiation-group
summary. Raw reports retain both representations.
Review each selected workload first: unoptimized instrumented kernels can
be much slower than the ordinary product. Exact selectors are mandatory to avoid
accidentally invoking large groups or opt-in benchmarks. Offline tool contracts
exercise refusals without compiling the product or executing a GPU.

The raw export retains SDK and compiler mappings. Product summaries include only
the checkout's `src` tree, even when a downloaded SDK resides in its `out` cache.
Source counts remain separate from host coverage and release qualification.

OpenCppCoverage 0.9.9.0 is a development-only GPL-3.0 tool, downloaded from its
[official release](https://github.com/OpenCppCoverage/OpenCppCoverage/releases/tag/release-0.9.9.0).
Its installer size, SHA-256, and complete executable/DLL identities are pinned in
`tools/native-coverage-toolchain.json`. It is installed noninteractively into the
checkout's ignored tool cache and is not distributed with FlitzZip.

On Windows, provision with `py -3 -B -m tools.native_coverage --provision-only`.
`py -3 -B -m tools.native_coverage` requires an existing current CPU-only
`Debug` build; it never changes or rebuilds the ordinary HIP product
configuration. Use a separate checkout when local coverage would otherwise
replace a qualified product build.
