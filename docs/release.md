# Release Workflow

SuperZip releases are published by `.github/workflows/release.yml`. The workflow
is manual-only, versioned by SemVer, and can publish either a beta GitHub
prerelease or a stable GitHub release.

## Manual Inputs

- `release_version`: SemVer product version without a `v` prefix, for example
  `0.1.0`. Leave it empty to use the version declared in `CMakeLists.txt`.
- `replace_existing`: deletes the existing release/tag before publishing when
  set to `true`.
- `release_track`: `beta` publishes a prerelease; `stable` publishes a normal
  release.
- `create_msi`: builds and smoke-tests an MSI with WiX in addition to the
  portable ZIP.

## Runner Contract

GitHub-hosted Windows runners are valid for release-mechanics validation: x64
build, tests, portable ZIP packaging, MSI creation, silent install, silent
uninstall, checksums, and GitHub release publication. They do not provide an AMD
GPU and must not publish HIP-enabled binaries.

GitHub Actions in this repository must not use self-hosted runners because the
workflow-security scanner treats them as a repository security risk. GPU-enabled
HIP builds remain supported through the Windows-native local build path:

```powershell
tools\build.ps1 -Configuration Release -EnableHip -HipArch gfx1201
build\Release\superzip_cli.exe gpu-info
```

Add a GitHub-hosted AMD HIP release path only after a secure hosted AMD Windows
runner or deterministic HIP SDK setup is available and the workflow-security
scanner is clean without suppressions.

## Triggering

Use the GitHub UI:

1. Open `Actions > release`.
2. Click `Run workflow`.
3. Enter a product version such as `0.1.0`.
4. Choose `release_track=beta` for the first public beta or `stable` for a
   normal release.
5. Keep `create_msi=true` unless MSI validation is intentionally being isolated.

Use GitHub CLI for the first beta validation run:

```powershell
gh workflow run release.yml -R strmt7/SuperZip `
  -f release_version=0.1.0 `
  -f replace_existing=false `
  -f release_track=beta `
  -f create_msi=true
```
