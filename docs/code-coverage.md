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
described below. Its current zero helper counters are a measurement limitation,
not a reason to rewrite correct product code or invent line hits. Coverage
remains unfinished until the independent behavior and measurement gaps close.

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
lines. GUI executable-only paths, Python/PowerShell tooling, and GPU device
instructions are not measured by this report. Do not describe this
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
GUI executable, PDB, scenario and imported UI helper identities. A smoke failure
or a changed helper invalidates collection.
Evidence goes to `out/coverage-hip-host-gui` (or `out/coverage-gui` without HIP),
with `gui_host_coverage=true`. These local measurements do not replace the CI
badge or establish device-instruction coverage. The default collector does not
open GUI windows.

`tools/native_coverage.py` verifies the native build receipt, all selected
executables, their symbols and scenario drivers before and after collection.
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
used in parallel mode. Retain raw data and a consolidated snapshot so an export
or supplemental test cannot silently replace the cohort. A small independent
control should demonstrate both loss without append and preservation with it.

Path aliases may unify exact source copies used by isolated process tests only
when the fixture proves byte equality with the frozen production inputs. Never
alias a test double or modified copy to a production module. Report production
totals separately from measured test bodies, retain zero-hit production lines,
and disclose line and branch results independently.

## Meaningful tests

Choose a missing behavior or concrete failure mode before writing a test. Prefer
independent expected results, external-format interoperability, malformed input,
boundary sizes, lifetime transitions, cancellation, and corruption followed by
valid reuse. Prove a regression test fails with the defect present. Do not copy
the implementation into assertions, remove untested code from reports, or add
tests whose only purpose is increasing a percentage. A coverage increase without
new behavioral evidence is not an engineering improvement.

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

The local GUI/HIP-host collector has completed the full native registry, CLI
contracts and all-page interactive smoke on the same source identity. Its captures
were visually reviewed, including compact layouts, Neutron selection, overwrite
confirmation, scrollable tables, monitor graphs and both license tabs. This
qualifies those exercised paths; it does not make the remaining zero-hit lines
covered. The current source-bound totals and inventories remain in the collector
artifacts, rather than in reusable agent instructions.

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
Run the qualification with explicit paths, using the existing product SDK for
HIP headers and runtime. No machine-specific installation path is embedded:

```powershell
py -3 -B -m tools.hip_coverage --compiler-root $coverageCompiler --sdk $productSdk `
  --device-runtime "$coverageBuild/compiler-rt/lib/generic/libclang_rt.profile-amdgpu.a" `
  --arch $coverageArch --output out/hip-coverage-qualification
```

Repeat qualification when the compiler, profiler, runtime or device changes.
Run the offline counter-admission regressions with
`py -3 -B -m unittest tools.test_hip_coverage`; the existing component-contract
workflow selects these tests when their inputs change and never launches a GPU.

## Tool provenance and local use

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
