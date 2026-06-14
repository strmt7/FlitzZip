# Release Workflow

SuperZip releases are created by `.github/workflows/release.yml`. The workflow
is manual-only, versioned with SemVer, and publishes either a beta prerelease or
a stable GitHub release.

## Release Artifacts

Every product release must be Windows x64 and HIP-enabled. The portable ZIP and
MSI contain the same application binaries and the same documentation. The
portable package is installation-free, not feature-reduced.

The MSI defaults to a per-user install scope so silent install/uninstall smoke
tests work without elevation. Per-machine MSI builds remain supported for
managed deployment by configuring with
`tools\build.ps1 -MsiInstallScope perMachine` and running install smoke from an
elevated/admin validation environment.

The release workflow refuses to publish CPU-only artifacts. CPU-only builds are
reserved for internal CI validation on GitHub-hosted runners that do not have the
AMD HIP SDK.

## Repository Inputs

Before running a HIP-enabled hosted release, configure these repository secrets
and variables:

- `HIP_SDK_INSTALLER_URL`: repository secret containing the AMD HIP SDK
  installer URL accepted from AMD's license-gated download page.
- `HIP_SDK_INSTALLER_SHA256`: repository secret containing the expected SHA-256
  digest of that installer.
- `WIX_OSMF_EULA_ID`: repository variable set to `wix7` after the maintainer has
  accepted the WiX v7 OSMF EULA.

Do not use GitHub Actions environments for release secrets in this repository:
environment-gated jobs can create deployment records, and SuperZip workflows
must never create deployments.

Do not commit AMD installer URLs, driver packages, credentials, or local
download paths to the repository.

## Manual Inputs

- `release_version`: SemVer product version without a `v` prefix, for example
  `0.1.0`. Leave it empty to use the version declared in `CMakeLists.txt`.
- `replace_existing`: deletes the existing release/tag before publishing when
  set to `true`.
- `release_track`: `beta` publishes a prerelease; `stable` publishes a normal
  release.
- `create_msi`: builds and smoke-tests the MSI in addition to the portable ZIP.

## Validation

The workflow performs:

- HIP SDK installation and checksum verification when `HIP_PATH` is not already
  present on the runner.
- HIP-enabled build and C++ tests.
- Local repository security scan.
- Portable package staging, dependency check, SUZIP/ZIP smoke tests, and
  checksum generation.
- MSI creation and silent install/uninstall smoke tests when requested.
- GitHub release creation with attached SHA-256 files.

## Local MSI Tooling

Install the pinned repo-local WiX CLI before local MSI packaging:

```powershell
tools\install_wix.ps1
```

That helper installs WiX under `out\tools\wix`. Maintainer authorization is
recorded in `AGENTS.md` for agents to accept EULAs required by pinned SuperZip
verification tooling. To package the MSI locally, set:

```powershell
$env:SUPERZIP_ACCEPT_WIX_OSMF_EULA = "wix7"
```

Then run:

```powershell
tools\package.ps1 -Configuration Release -CreateMsi
```

## Installer Dependency Contract

The MSI must install SuperZip itself and validate external prerequisites. It must
not silently install or downgrade AMD GPU drivers. AMD's Windows HIP deployment
guidance treats the HIP runtime as part of the AMD GPU driver, and the HIP SDK
installer requires explicit license acceptance and administrator privileges.

The installer smoke test runs `superzip_cli.exe dependency-check` after install.
The accepted states are:

- `0`: release artifact is HIP-enabled, the HIP runtime is loadable, and an AMD
  GPU is available.
- `12`: release artifact is HIP-enabled and the HIP runtime is loadable, but the
  hosted runner has no supported AMD GPU.

Exit codes `10` and `11` are release-blocking because they mean the installed
artifact is CPU-only or cannot load the AMD driver-provided HIP runtime.

GitHub-hosted runners normally do not provide an AMD GPU. The workflow therefore
requires a HIP-enabled binary and a loadable HIP runtime, but it treats a missing
GPU device as a hosted-runner limitation. Hardware execution with
`--require-gpu` is validated on AMD hosts through local smoke tests and should be
added to hosted release validation only when a secure AMD Windows runner profile
is available without introducing self-hosted-runner risk.

## Triggering

Use the GitHub UI:

1. Open `Actions > release`.
2. Click `Run workflow`.
3. Enter a product version such as `0.1.0`.
4. Choose `release_track=beta` for the first public beta or `stable` for a
   normal release.
5. Keep `create_msi=true` unless MSI validation is intentionally isolated.

Use GitHub CLI:

```powershell
gh workflow run release.yml -R strmt7/SuperZip `
  -f release_version=0.1.0 `
  -f replace_existing=false `
  -f release_track=beta `
  -f create_msi=true
```
