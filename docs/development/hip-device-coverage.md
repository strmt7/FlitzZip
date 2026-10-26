# HIP Device Coverage

This development guide qualifies actual AMDGPU instruction counters and measures
selected product kernels. It is separate from the ordinary release build and
from native CPU, GUI and HIP-host coverage. Read the
[coverage acceptance policy](../code-coverage.md#release-acceptance) before using
any result for a candidate decision.

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
