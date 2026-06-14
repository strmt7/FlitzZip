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
- `runner_profile`: `hosted-windows` validates build/package/release mechanics
  on GitHub-hosted Windows; `self-hosted-amd-hip` uses fixed self-hosted labels
  for GPU-enabled release binaries.
- `create_msi`: builds and smoke-tests an MSI with WiX in addition to the
  portable ZIP.
- `enable_hip`: builds with AMD HIP. This requires a self-hosted Windows x64
  runner with AMD HIP SDK, `HIP_PATH`, Visual Studio C++ tools, and a supported
  AMD GPU when runtime smoke tests use `--require-gpu`.
- `hip_arch`: HIP architecture such as `gfx1201`.

## Runner Contract

GitHub-hosted Windows runners are valid for release-mechanics validation: x64
build, tests, portable ZIP packaging, MSI creation, silent install, silent
uninstall, checksums, and GitHub release publication. They do not provide an AMD
GPU and must not publish HIP-enabled binaries.

GPU-enabled release binaries require a maintained self-hosted Windows AMD HIP
runner. The runner must be 64-bit Windows, have Visual Studio C++ tools and AMD
ROCm/HIP installed, expose `HIP_PATH`, and be validated with
`superzip_cli.exe gpu-info` before publishing.

## Triggering

Use the GitHub UI:

1. Open `Actions > release`.
2. Click `Run workflow`.
3. Enter a product version such as `0.1.0`.
4. Choose `release_track=beta` for the first public beta or `stable` for a
   normal release.
5. Keep `enable_hip=false` on GitHub-hosted Windows runners.

Use GitHub CLI for the first beta validation run:

```powershell
gh workflow run release.yml -R strmt7/SuperZip `
  -f release_version=0.1.0 `
  -f replace_existing=false `
  -f release_track=beta `
  -f runner_profile=hosted-windows `
  -f create_msi=true `
  -f enable_hip=false `
  -f hip_arch=gfx1201
```

Use GitHub CLI on a self-hosted AMD HIP runner after that runner is registered
and selected by repository runner labels:

```powershell
gh workflow run release.yml -R strmt7/SuperZip `
  -f release_version=0.1.0 `
  -f replace_existing=false `
  -f release_track=beta `
  -f runner_profile=self-hosted-amd-hip `
  -f create_msi=true `
  -f enable_hip=true `
  -f hip_arch=gfx1201
```
