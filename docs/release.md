# Release Workflow

SuperZip beta releases are published by
`.github/workflows/beta-release.yml`. The workflow is manual-only and creates a
GitHub prerelease with versioned Windows x64 assets.

## Manual Inputs

- `release_version`: SemVer product version without a `v` prefix, for example
  `0.1.0`. Leave it empty to use the version declared in `CMakeLists.txt`.
- `replace_existing`: deletes the existing release/tag before publishing when
  set to `true`.
- `create_msi`: builds and smoke-tests an MSI with WiX in addition to the
  portable ZIP.
- `enable_hip`: builds with AMD HIP. This requires a self-hosted Windows x64
  runner with AMD HIP SDK, `HIP_PATH`, Visual Studio C++ tools, and an AMD GPU
  when `--require-gpu` runtime smoke tests are expected.
- `hip_arch`: HIP architecture such as `gfx1201`.
- `runner_labels_json`: JSON `runs-on` value. Use `"windows-2022"` for hosted
  installer validation, or an array such as
  `["self-hosted","Windows","X64","AMD","HIP"]` for a HIP release runner.

## HIP Runner Requirement

GitHub-hosted Windows runners do not provide an AMD GPU or preinstalled AMD HIP
SDK. AMD's current Windows HIP SDK installer requires administrator privileges
and has a graphical installer lifetime even for command-line installation, so
the supported HIP release path is a maintained self-hosted Windows AMD runner.

The hosted workflow path still validates the x64 build, tests, portable ZIP,
MSI creation, silent install, silent uninstall, checksums, and release
publication mechanics. It must not be represented as a HIP-enabled binary.

## Triggering

Use the GitHub UI:

1. Open `Actions > beta-release`.
2. Click `Run workflow`.
3. Enter a product version such as `0.1.0`.
4. For true HIP binaries, set `enable_hip=true` and select a self-hosted runner
   through `runner_labels_json`.

Use GitHub CLI:

```powershell
gh workflow run beta-release.yml -R strmt7/SuperZip `
  -f release_version=0.1.0 `
  -f replace_existing=false `
  -f create_msi=true `
  -f enable_hip=false `
  -f runner_labels_json='"windows-2022"'
```

For a HIP runner:

```powershell
gh workflow run beta-release.yml -R strmt7/SuperZip `
  -f release_version=0.1.0 `
  -f create_msi=true `
  -f enable_hip=true `
  -f hip_arch=gfx1201 `
  -f runner_labels_json='["self-hosted","Windows","X64","AMD","HIP"]'
```
