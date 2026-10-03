# Native Windows ROCm Toolchain

SuperZip uses the complete ROCm Core SDK Windows distribution pinned in
`tools/rocm-sdk-lock.json`. This is one whole-distribution update: SDK headers,
compiler, libraries and device files remain as AMD supplied them. No individual
bundled component is replaced or independently upgraded.

## Repeatable Setup

The same command provisions the SDK for developers, AI agents and hosted
releases. Run it from the checkout on any native Windows build host:

```powershell
py -3 tools/bootstrap_rocm_sdk.py
tools/build.ps1 -Configuration Release -HipArch release
tools/test.ps1 -Configuration Release
build/Release/superzip_cli.exe dependency-check
```

The ordinary build now defaults to the portable `release` target preset.
Direct CMake and the standalone object compiler use the same resolver;
explicit single-target development builds remain supported. Existing CMake
cache choices are preserved. The preset contains the six targets whose
production kernels were compiled below; it is not a promise to support every
GPU in AMD's evolving matrix. Additional architectures require qualification.

The cache derives from the checkout and pinned version. It requires space for
the entire distribution, checks available storage before downloading/extracting,
and verifies archive SHA-256 and exact byte counts. Archive inventory rejects
Windows traversal, reserved names, collisions, links, special entries and
resource-limit violations before native extraction. Extraction uses the shared
Windows process containment implementation with bounded RAM, output and lifetime.
Provisioning changes neither host installations nor global environment settings.

An already downloaded whole archive and a custom cache location are supported:

```powershell
py -3 tools/bootstrap_rocm_sdk.py --archive <complete-sdk-archive> --parent <sdk-cache-parent>
tools/build.ps1 -Configuration Release -HipPath <sdk-cache-parent>/sdk
```

Existing manually extracted SDK content at that destination is adopted only
after byte-exact comparison of every archive file. A successful provisioning
receipt permits bounded subsequent checks of critical compiler inputs; those
checks are not a repeated audit of every library in the SDK. Changed caches are
preserved and rejected. Provision a fresh parent to replace them.

`-HipPath` selects an explicit complete installation. Without it, the build
prefers the repository's pinned cache and then examines `HIP_PATH`. Direct CMake
users can set `SUPERZIP_HIP_PATH`. That selected root controls host includes,
import libraries, runtime metadata, every HIP compilation and every dependency
scan. The build script passes it on each configure, preventing an older CMake
cache from silently selecting a different SDK. CPU-only validation does not
require ROCm and remains limited to hosted/static-analysis checks.

## Native Build Receipts

`tools/build.ps1` owns an exclusive Windows sharing lock for the entire CMake
build tree, across configurations. It records an incomplete attempt before
configuration, freezes actual Git-visible native input contents and configured
options, and publishes a successful receipt only after the build succeeds and
the source/configuration/observed toolchain remain unchanged. GUI, CLI, runtime
manifest and all app-local DLL hashes bind the outputs. Configure-only, failed,
interrupted or active attempts cannot qualify for RAM measurement. Old receipt
and transaction bytes remain under ignored `build/native-build-history`; they
are historical evidence, not acceptance of the next attempt. The lock's file
presence is not proof of a live process; an unlocked incomplete attempt can be
replaced by an ordinary new build without manual marker deletion.

The first receipt and changed configuration, observed toolchain or output bytes
require `--clean-first`. Ordinary source changes retain CMake's incremental
dependency behavior. Toolchain evidence records the CMake MSVC probe, hashes of
CMake and MSVC's `cl.exe`, `c1xx.dll`, `c2.dll`, and, for HIP builds, the SDK lock,
`hipcc.exe`, Clang, HIP import library/version header and device bitcode files.
It does not verify every SDK/compiler/header/library byte or authenticate which
compiler produced each cached object. Complete distribution installation stays
the shared provisioner's responsibility; no installed component is modified.
Receipts contain relative filenames and hashes rather than installation paths.
This is local invocation consistency evidence, not signed or reproducible-build
attestation. Direct CMake and standalone object compilation remain supported,
but do not themselves publish the wrapper's successful invocation receipt.

Validate the current build without timing a workload:

```powershell
py -3 tools/native_build_receipt.py validate --configuration Release --require-hip
```

RAM benchmarks now require this receipt and preserve its digest around every
observation. A documentation-only commit does not change native input contents;
a modified compiled resource or an additional nonignored source does. Failed
validation requires a normal rebuild, rather than editing the receipt.

## Distribution And Component Versions

Checked on 2026-10-02 against AMD's [release notes](https://rocm.docs.amd.com/en/docs-10.0.0/about/release-notes.html)
and [native Windows installation guide](https://rocmdocs.amd.com/en/latest/install/rocm.html).
The shared lock records the official complete Windows multiarchitecture archive
and its locally observed HTTPS SHA-256. That digest is not an independently
published AMD checksum.

The archive identifies ROCm `10.0.0` and package build `10.0.0rc4`, but reports
HIP `7.15.26333-6b0e43f341` and AMD Clang `23.0.0git`. AMD's release-note table
instead lists HIP 10 and LLVM 24. The whole distribution identity controls
admission; API availability and performance claims must use the observed headers,
compiler and runtime. Renaming component versions or substituting individual
packages would conceal the discrepancy and is prohibited.

The six-target compatibility fixture compiled with MSVC 14.44. The latest local
MSVC 14.51 rejected this compiler's device math declarations. The wrapper keeps
its compatible toolset selection and an explicit `-VcvarsVersion` override;
it never patches AMD or Microsoft headers to force compatibility. Main product
host compilation can use the installed CMake-selected MSVC toolchain.

All five production HIP objects and their dependency scans also compiled for
all six release targets using a fresh build tree and the provisioned SDK at
paths containing spaces. The main product passed 570 native tests and all 33
full local verification commands. Hardware execution covers the available
`gfx1201`; compilation alone does not qualify the other GPUs.

## Compiler And Runtime Environment

AMD's installation guide documents `HIP_DEVICE_LIB_PATH` and `LLVM_PATH` for
compiler discovery. SuperZip sets these, `HIP_PATH`, `ROCM_PATH` and
`HIP_PLATFORM` only around HIP compilation and its dependency scans, restoring
the caller's values on both success and failure. No global PATH modification or
SDK runtime redistribution is required.

An isolated fixture on one driver configuration crashed with `0xC0000005` when
the new SDK's `LLVM_PATH` was present, then passed with that variable unset and
the same System32 runtime loaded. This is finite host evidence, not a claim that
every AMD driver is affected. Keep compiler-only settings out of application
launch environments. SuperZip retains its trusted driver runtime loader and
required-HIP failure semantics.

Compilation for six targets does not establish runtime compatibility on every
GPU. See [release targets and validation](release.md#gpu-targets). Any speed or
compression-ratio improvement still requires the documented byte-exact RAM-only
CPU/GPU comparison with measured HIP telemetry and corrected statistical planning.

## Fresh Hosted Compile Qualification

The manually dispatched `rocm-qualification` workflow uses a fresh selected
Windows runner and the same complete-distribution provisioner and native build
helpers as local development. It builds every release target with read-only
repository permissions. Storage admission checks actual available capacity;
insufficient capacity fails rather than removing runner software or extracting
only selected SDK components.

Run it on the intended pushed source before release qualification. Its successful
result establishes fresh SDK provisioning and HIP compilation/linking for that
runner image. It does not establish GPU execution, driver compatibility,
compression speed or package acceptance, and it never publishes a release.
