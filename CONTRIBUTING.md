# Contributing

Read `AGENTS.md` before making changes. It contains the source-of-truth rules
for architecture, build/test commands, function documentation, security
boundaries, and git workflow.

## Required Checks

```powershell
tools\build.ps1 -Configuration Release
tools\test.ps1 -Configuration Release
tools\security_scan.ps1
```

If AMD ROCm/HIP for Windows is installed:

```powershell
tools\build.ps1 -Configuration Release -EnableHip -HipArch gfx1201 -VcvarsVersion 14.44
tools\bench.ps1 -Configuration Release -RequireGpu
```

## Pull Request Expectations

- Keep changes scoped.
- Add tests for behavior and security boundaries.
- Keep Defender and SHA-256 features opt-in.
- Do not commit build outputs, archives, credentials, personal paths, or local
  machine-specific configuration.
- Document every new or changed function with the contract format in
  `AGENTS.md`.
