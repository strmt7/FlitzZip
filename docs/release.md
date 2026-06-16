# Release Workflow

SuperZip releases are created by `.github/workflows/release.yml`. The workflow
is manual-only, versioned with SemVer, and publishes either a beta prerelease or
a stable GitHub release.

## Release Artifacts

Every product release must be Windows x64 and HIP-enabled. The portable ZIP and
MSI contain the same application binaries and the same documentation. The
portable package is installation-free, not feature-reduced.

The MSI defaults to the release deployment scope: per-machine install under
`C:\Program Files\SuperZip`. This matches normal Windows desktop installers and
requires elevation when the installing user does not already have administrator
rights.

The MSI exposes `Create Desktop shortcut` as an optional installer feature. Do
not use CPack's unconditional `CPACK_CREATE_DESKTOP_LINKS`; the desktop shortcut
must be an MSI-owned component selected through the installer UI so uninstall can
remove it cleanly.

For local coding and non-admin installer tests only, agents may build an
explicit per-user MSI with `tools\build.ps1 -MsiInstallScope perUser`, then run
`tools\package.ps1 -CreateMsi`. Per-user MSI artifacts are development/test
artifacts and must not be published as product releases.

The release workflow refuses to publish CPU-only artifacts. CPU-only builds are
reserved for internal CI validation on GitHub-hosted runners that do not have the
AMD HIP SDK.

## Repository Inputs

Before running a HIP-enabled hosted release, configure these repository
variables:

- `HIP_SDK_INSTALLER_URL`: repository variable containing the AMD HIP SDK
  installer URL accepted from AMD's download page.
- `HIP_SDK_INSTALLER_SHA256`: repository variable containing the expected SHA-256
  digest of that installer.
- `WIX_OSMF_EULA_ID`: repository variable set to `wix7` after the maintainer has
  accepted the WiX v7 OSMF EULA.

Do not use GitHub Actions environments for release inputs in this repository:
environment-gated jobs can create deployment records, and SuperZip workflows
must never create deployments. Do not put the HIP installer values in
repository secrets; Zizmor requires secrets to be environment-gated, and
environments are forbidden here.

Do not commit AMD installer URLs, driver packages, credentials, or local
download paths to the repository.

## Manual Inputs

- `release_version`: SemVer product version without a `v` prefix, for example
  `0.1.0`. Leave it empty to use the version declared in `CMakeLists.txt`.
  `0.1.0` is the current beta release. Until a maintainer explicitly opens the
  next version line, use `0.1.0` with `replace_existing=true` so the workflow
  replaces the existing release/tag instead of publishing a new version.
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
- Portable package staging, dependency check, SUZIP/ZIP/TAR/TAR.GZ/Gzip/CPIO/AR/DEB
  smoke tests, and
  checksum generation.
- MSI creation and silent install/uninstall smoke tests under the Program Files
  release install path when requested.
- GitHub release creation with attached SHA-256 files.

Release notes must list the actual fixes, created assets, validation work, and
known limitations for that release. Do not publish vague notes that hide what
changed.

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

That command packages the currently configured build. If the build was created
with the default scope, the MSI installs under `C:\Program Files\SuperZip` and
requires elevation. For a non-admin local MSI smoke, first configure with
`tools\build.ps1 -Configuration Release -MsiInstallScope perUser`.

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
