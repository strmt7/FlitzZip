# Contributing

Read `AGENTS.md` before making changes. It contains the source-of-truth rules
for architecture, build/test commands, function documentation, security
boundaries, and git workflow.

## Required Checks

```powershell
tools\build.ps1 -Configuration Release
tools\test.ps1 -Configuration Release
tools\security_scan.ps1
tools\package.ps1 -Configuration Release
```

Parser fuzzing runs in GitHub Actions through ClusterFuzzLite. When changing
archive metadata or path-safety code, also run the fuzz build locally if Docker
is available:

```powershell
tools\fuzz.ps1 -Runs 512
```

The default build is HIP-enabled and requires `HIP_PATH`. If a hosted CI or
static-analysis machine cannot install AMD HIP, use explicit CPU-only
validation:

```powershell
tools\build.ps1 -Configuration Release -CpuOnlyValidation
tools\test.ps1 -Configuration Release
```

Do not publish CPU-only validation packages.

## Pull Request Expectations

- Keep changes scoped.
- Add tests for behavior and security boundaries.
- Keep Defender and SHA-256 features opt-in.
- Do not commit build outputs, archives, credentials, personal paths, or local
  machine-specific configuration.
- Document every new or changed function with the contract format in
  `AGENTS.md`.
