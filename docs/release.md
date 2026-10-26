# Release Workflow

FlitzZip releases are created by `.github/workflows/release.yml`. The workflow
is manual-only and versioned with SemVer. By default it validates a candidate
without creating a tag or release; explicit publication can create a beta
prerelease or a stable GitHub release.

Release stages are declared directly in the workflow, whose workspace checkout
uses `github.sha`. Third-party actions use full commit pins. The single-use
local action has been removed: the latest stable actionlint 1.7.12 rejects
GitHub's newer `$/` syntax ([upstream issue](https://github.com/rhysd/actionlint/issues/711)),
and the current Semgrep action-pin rule also flags it. No scanner is suppressed.
`tools/release_workflow_policy.ps1` checks the checkout identity and preserves the
replacement acknowledgement and release-note guards. Its contracts never
publish a release or install a product. A bounded Windows Installer probe uses
a deliberately absent package to verify the workflow's native argument handling
and expected error logging. It runs the actual workflow functions in PowerShell 7.

## Release Artifacts

Every candidate must pass the exact-package install/repair/uninstall smoke and
the disposable-machine upgrade matrix, including skipped releases, downgrade
rejection, feature migration and failed-upgrade rollback. Follow the
[persistence and upgrade contract](development/persistence-and-upgrades.md) for
stable installer identities, numeric MSI version limits, historical settings
fixtures and future schema changes. Installing protocol fixtures on a
development or customer host is prohibited.

Every product release must be Windows x64 and HIP-enabled. The portable ZIP and
MSI contain the same application binaries and the same documentation. The
portable package is installation-free, not feature-reduced.

## GPU Targets

The release workflow selects the `release` architecture preset from
`tools/hip_architecture.ps1`: `gfx1100`, `gfx1101`, `gfx1102`, `gfx1151`,
`gfx1200`, and `gfx1201`. One Windows x64 binary contains HIP device images
for all six targets; the driver selects the matching image. This replaces
the previous release configuration that compiled only `gfx1201`.
Target selections are bounded, checked for duplicates, and passed as separate
`--offload-arch` arguments. CMake tracks the shared resolver as an object
dependency so changing the preset rebuilds affected kernels.

Ordinary wrapper builds, direct CMake configuration and standalone HIP object
compilation default to this same portable preset. Direct CMake resolves it
through the shared PowerShell policy before generating runtime metadata, so
the manifest contains concrete targets rather than the word `release`.
Use:

```powershell
tools/build.ps1 -Configuration Release -HipArch release
tools/test.ps1 -Configuration Release
tools/package.ps1 -Configuration Release
```

An explicit `-HipArch gfx1201` (or another compiler-supported target) remains
available for focused development; a single-target build is not the portable
release. Existing direct CMake caches preserve an explicitly selected target;
set `-DFLITZZIP_HIP_ARCH=release` to change that selection. This default change
does not expand the six-target hardware claim or qualify execution elsewhere.

The compiler supports multiple target images as documented in
[Clang HIP support](https://clang.llvm.org/docs/HIPSupport.html).
The original target selection used AMD's
[HIP SDK 7.1.1 Windows support table](https://rocm.docs.amd.com/projects/install-on-windows/en/docs-7.1.1/reference/system-requirements.html).
Current GPU, OS and driver prerequisites must be checked against AMD's
[ROCm 10 compatibility selector](https://rocm.docs.amd.com/en/docs-10.1.0/compatibility/compatibility-matrix.html)
and the [selected whole SDK](rocm-toolchain.md); historical support is not
current hardware qualification.
An unsupported target or incompatible SDK must fail compilation, not silently
drop a target. This does not add support for non-AMD GPUs, unlisted GPU
families, unsupported operating systems, or arbitrary driver versions.

Qualification must confirm all six target images in every production HIP object,
retain dependency scans, and exercise SDK and build paths containing spaces.
Record the exact source, SDK, binaries and devices used. Compilation establishes
target-image availability; correctness, timing and memory behavior require actual
execution on the reported hardware. Keep those results in candidate artifacts
and current benchmark records rather than treating historical test totals as
qualification of a newer candidate.

## Installation

The MSI defaults to the release deployment scope: per-machine install under
`C:\Program Files\FlitzZip`. This matches normal Windows desktop installers and
requires elevation when the installing user does not already have administrator
rights.

The Windows elevation prompt is controlled by the operating system before the
per-machine MSI can make changes. The MSI cannot shorten that OS-owned prompt
from inside the package. FlitzZip-owned installer launchers, release validation,
and smoke tests must therefore use bounded waits around the installer process:
MSI install, repair, and uninstall phases fail after 300 seconds. Whole-SDK
provisioning separately bounds download, inventory and contained extraction;
it does not launch an installer or request elevation.

The MSI exposes `Create Desktop shortcut` as an optional installer feature. Do
not use CPack's unconditional `CPACK_CREATE_DESKTOP_LINKS`; the desktop shortcut
must be an MSI-owned component selected through the installer UI so uninstall can
remove it cleanly.

## MSI Maintenance And Updates

FlitzZip uses Windows Installer identity rules directly:

The product rename keeps the established upgrade GUID so an installed predecessor
is detected and removed by the MSI major-upgrade transaction. Display names,
executables, shortcuts and new installation directories use FlitzZip. Per-user
settings are read from the predecessor directory only when no current settings
file exists; validated settings are then written to the new directory. The old
file is retained, current settings always take precedence, and smoke isolation
never reads a real user's predecessor settings.

- `UpgradeCode` is stable for all FlitzZip MSI releases so Windows Installer can
  detect installed FlitzZip products.
- `ProductCode` is deterministically derived from a SHA-256 identity seed
  containing the stable `UpgradeCode`, MSI numeric version, install scope, and
  package/build identity. Release builds use the GitHub run identity so a
  republished same-version MSI receives a distinct `ProductCode`.
- WiX `MajorUpgrade` has same-version upgrades enabled. When a replacement MSI
  has the same numeric version but a new ProductCode under the stable
  UpgradeCode, Windows Installer removes the installed package and installs the
  replacement instead of showing an already-installed conflict.
- New normal releases should still use a new SemVer numeric version. Same-version
  MSI replacement is exceptional and must be explicitly requested by the
  maintainer/user.
- `tools\test_msi_identity.ps1` verifies that same version/same scope/same
  product identity is stable, same-version replacement identity changes the
  `ProductCode`, patch updates receive a different `ProductCode`, all scopes
  keep the same `UpgradeCode`, and per-user test MSIs do not collide with product
  per-machine MSIs.
- Release smoke installs the MSI, verifies the installed CLI against the staged
  binary, removes that owned executable, and repairs the same MSI. Repair must
  restore the exact original bytes and pass dependency-check before uninstall.
  This proves recovery through the shipped package rather than merely invoking
  repair on an intact installation. This destructive control runs only in the
  disposable release-validation runner.

For local coding and non-admin installer tests only, agents may build an
explicit per-user MSI with `tools\build.ps1 -MsiInstallScope perUser`, then run
`tools\package.ps1 -CreateMsi`. Per-user MSI artifacts are development/test
artifacts and must not be published as product releases.

The release workflow refuses to publish CPU-only artifacts. CPU-only builds are
reserved for internal CI validation on GitHub-hosted runners that do not have the
AMD HIP SDK.

## Repository Inputs

The local and hosted HIP build use the complete distribution pinned in
`tools/rocm-sdk-lock.json`, provisioned by `tools/bootstrap_rocm_sdk.py`. See
[the ROCm toolchain contract](rocm-toolchain.md) for portable setup, provenance
and observed component versions. An arbitrary preinstalled `hipcc.exe` cannot
bypass the pinned SDK requirement. The former HIP installer URL/checksum
repository variables are no longer used.

Before running a hosted release, configure this repository variable:

- `WIX_OSMF_EULA_ID`: repository variable set to `wix7` after the maintainer has
  accepted the WiX v7 OSMF EULA.

Do not use GitHub Actions environments for release inputs in this repository:
environment-gated jobs can create deployment records, and FlitzZip workflows
must never create deployments. Public pinned distribution metadata belongs in
the shared repository lock; credentials do not.

Do not commit AMD installer URLs, driver packages, credentials, or local
download paths to the repository.

## Manual Inputs

- `publish_release`: defaults to `false`. Build, test, scan and retain exact-SHA
  candidate artifacts without publishing. Set it to `true` only after the
  maintainer approves publication. Validation has read permission; the separate
  dependent publisher alone receives release write permission.

- `release_version`: SemVer product version without a `v` prefix or build
  metadata. Leave it empty to use the version declared in `CMakeLists.txt`.
  Prerelease identifiers follow the [SemVer identifier rules](https://semver.org/#spec-item-9):
  mixed identifiers such as `1beta` are valid; numeric identifiers such as
  `01` are not.
- `replace_existing`: replaces the existing release/tag when set to `true`.
  This is exceptional and must be used only when the current maintainer/user
  request explicitly asks to replace that specific version. The workflow builds,
  tests, packages, and smoke-tests first, then deletes the existing release/tag
  immediately before publishing the replacement. Normal release runs use a new
  SemVer version with `replace_existing=false`. Replacement is rejected during
  validation-only runs.
- `replacement_acknowledgement`: required only when `replace_existing=true`.
  Enter exactly `replace <same-version>`, matching the explicit
  `release_version` value in the same run. The release
  workflow refuses to delete an existing release/tag without this exact
  version-specific acknowledgement.
- `release_track`: `beta` publishes a prerelease; `stable` publishes a normal
  release.
- `create_msi`: builds and smoke-tests the MSI in addition to the portable ZIP.

## Acceptance Criteria

Qualify a concrete candidate for its documented Windows x64, archive-format and
AMD HIP capabilities. Release readiness is a decision supported by the checks
below, their source/artifact identities and reviewed residual risks. It does not
promise universal hardware support, absence of every bug or an unmeasured
compression advantage. Publication still requires the maintainer's green light.

| Contract | Required evidence and blocking condition |
| --- | --- |
| Archive correctness | Applicable native registry and CTest suites, independent format-matrix readback and malformed-input, resource-bound and publication regressions pass. Any known byte mismatch, unsafe overwrite or incomplete rejection blocks. |
| CPU and GPU execution | Changed codec paths preserve independent decode results and resource ownership. Required-HIP tests exercise the real supported device; unavailable or incompatible HIP rejects Neutron explicitly. A host-only build or percentage cannot qualify device execution. |
| User journeys | The all-page GUI smoke and capture review pass for affected GUI behavior and the candidate qualification. Queue admission, create/extract/verify, cancellation, settings persistence and Neutron admission remain covered. An unrelated desktop obstruction is a failed environment qualification until resolved. |
| Coverage and review | Apply [coverage acceptance](code-coverage.md#release-acceptance): effective tests for critical and changed behavior, valid measurements, investigated regressions and reviewed residual gaps. A universal line percentage is not required. |
| Source and dependencies | Required workflows and the exact-commit post-push audit pass. Actionable open or reproduced source findings and applicable dependency vulnerabilities block unless their specific acceptance is already authorized under the existing security policy. |
| Distribution and licensing | Exact HIP-enabled portable ZIP/MSI, checksums, dependency admission and install/repair/uninstall smoke pass. Complete corresponding source, notices and action-specific redistribution rights accompany the candidate. |
| Claims and operations | Supported capabilities, installation, troubleshooting and known limitations are current. Changed performance claims have representative, bounded measurements with complete sizes and actual backend evidence. Unsupported superiority claims cannot enter release notes. |

Keep the candidate review, complete logs, scope-specific coverage and remaining
improvement work in retained artifacts. Reuse prior qualification only when its
relevant source, tools, configuration and outputs still match. A documentation
or policy change requires its own consumers and final exact-source offering;
it does not automatically invalidate unchanged codec behavior measurements.
After all applicable checks pass, continue optional improvements in the
implementation plan rather than reopening unrelated qualification indefinitely.

This approach follows the outcome and root-cause focus of
[NIST SSDF](https://csrc.nist.gov/projects/ssdf) and the behavior, integration and
critical-journey focus of [Google's testing guidance](https://testing.googleblog.com/2021/06/how-much-testing-is-enough.html).
These references guide engineering decisions; this repository does not claim
external certification or conformance assessment.

## Validation

Before publishing binaries, verify the exact release source offering. It must
contain the complete application sources and build scripts, the unmodified
`third_party/upstream/wimlib/1.14.5/wimlib-1.14.5.tar.gz` matching its recorded
SHA-256, and every other required corresponding source and notice. Make that
source available from the same GitHub release location. The workflow creates
a source ZIP directly from the exact commit, checks the complete tracked-file
inventory and unchanged wimlib source archive, and requires that checksummed ZIP
at publication. The selected GPLv3 option permits combination with the
application's AGPLv3 license; see [the wimlib source contract](third-party.md#wimlib-1145).
An upstream website link or a passing notice-text check alone is insufficient.

The workflow performs:

- Complete pinned ROCm SDK provisioning, SHA-256 verification and safe archive
  inventory through the same tool used by local developers and agents.
- HIP-enabled build and C++ tests.
- Local repository security scan.
- Portable package staging and dependency checks, followed by the shared
  registry-driven format matrix against the packaged CLI. This checks complete
  restored bytes, Unicode paths, aliases and overwrite failures for every
  supported format. A separate native SHA-256 roundtrip uses required HIP when
  hardware is available and explicitly selects the CPU reader/writer otherwise.
- ZIP/MSI checksum generation.
- MSI creation and silent install/repair/uninstall smoke tests under the
  Program Files release install path when requested. Each MSI phase has a
  300-second stale-wait timeout so an unanswered installer or elevation prompt
  cannot block validation indefinitely.
- Exact-SHA candidate artifact retention for fourteen days, including SHA-256
  sidecars, curated notes and build metadata. The publisher downloads those same
  artifacts and checks both ZIP/MSI digests before creating a release, only when
  `publish_release=true`.

Release notes must list the actual fixes, created assets, validation work, and
known limitations for that release. Do not publish vague notes that hide what
changed. Add `docs/releases/<version>.md` before dispatch; the workflow requires
its first line to match `# FlitzZip <version>` and includes its curated content
before build-specific verification facts.

## Local MSI Tooling

Install the pinned repo-local WiX CLI before local MSI packaging:

```powershell
tools\install_wix.ps1
```

That helper installs WiX under `out\tools\wix`. Maintainer authorization is
recorded in `AGENTS.md` for agents to accept EULAs required by pinned FlitzZip
verification tooling. To package the MSI locally, set:

```powershell
Set-Item Env:FLITZZIP_ACCEPT_WIX_OSMF_EULA "wix7"
```

Then run:

```powershell
tools\package.ps1 -Configuration Release -CreateMsi
```

That command packages the currently configured build. If the build was created
with the default scope, the MSI installs under `C:\Program Files\FlitzZip` and
requires elevation. For a non-admin local MSI smoke, first configure with
`tools\build.ps1 -Configuration Release -MsiInstallScope perUser`.

## Installer Dependency Contract

The MSI must install FlitzZip itself and validate external prerequisites. It must
not silently install or downgrade AMD GPU drivers. AMD's Windows HIP deployment
guidance treats the HIP runtime as part of the AMD GPU driver, and the HIP SDK
installer requires explicit license acceptance and administrator privileges.

The installer smoke test runs `flitzzip_cli.exe dependency-check` after install.
The accepted states are:

- `0`: release artifact is HIP-enabled, the HIP runtime is loadable, and an AMD
  GPU and the exact kernel module are available.
- `11`: the driver-provided runtime is absent. CPU functionality and capability
  reporting must still start; GPU features, including Neutron, are unavailable.
- `12`: release artifact is HIP-enabled and the HIP runtime is loadable, but the
  validation host has no supported AMD GPU.

Exit `10` blocks a release because its binary is CPU-only. Exit `13` blocks
qualification because a present device cannot admit its kernel module or other
required GPU initialization, or runtime device enumeration failed (reported as
`device_count=-1`). A successful zero count or HIP's documented
[no-device result](https://rocmdocs.amd.com/projects/HIP/en/latest/doxygen/html/group___device.html#ga8555d5c76d88c50ddbf54ae70b568394)
admits exit `12`; neither admits a runtime or kernel initialization error.
Missing or modified packaged DLLs independently
block qualification, including on hosts where the AMD driver is absent.

GitHub-hosted runners normally do not provide an AMD GPU. The workflow therefore
requires a HIP-enabled binary and exact packaged DLL integrity, retaining missing
driver/device diagnostics as hardware absence. It does not install a substitute
driver or claim GPU execution. Hardware execution with
`--require-gpu` is validated on AMD hosts through local smoke tests and should be
added to hosted release validation only when a secure AMD Windows runner profile
is available without introducing self-hosted-runner risk.

The first-party `flitzzip_hip_kernels.dll` contains HIP registration and device
code. Its digest is embedded into the host after linking and recorded in the
package manifest. Executables do not import this module at process startup.
The existing trusted loader pins its files, verifies bytes and admits it only
after the driver and device have passed their checks. A private versioned POD
dispatch table transfers no STL allocation, CRT owner or C++ exception between
the module and the host. Neutron still requires actual HIP execution.

## Triggering

Use the GitHub UI:

1. Open `Actions > release`.
2. Click `Run workflow`.
3. Enter the new product SemVer value.
4. Choose `release_track=beta` for the first public beta or `stable` for a
   normal release.
5. Keep `create_msi=true` unless MSI validation is intentionally isolated.
6. Leave `publish_release=false` to qualify a candidate. Enable publication only
   after explicit maintainer approval of the concrete candidate.

Use GitHub CLI:

```powershell
gh workflow run release.yml -R strmt7/FlitzZip `
  -f release_version=<new-semver> `
  -f publish_release=false `
  -f replace_existing=false `
  -f replacement_acknowledgement="" `
  -f release_track=beta `
  -f create_msi=true
```

Replacement example, only after the current maintainer/user request explicitly
asks to replace that specific version:

```powershell
gh workflow run release.yml -R strmt7/FlitzZip `
  -f release_version=<existing-semver> `
  -f publish_release=true `
  -f replace_existing=true `
  -f replacement_acknowledgement="replace <existing-semver>" `
  -f release_track=beta `
  -f create_msi=true
```
