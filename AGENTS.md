# SuperZip Agent Instructions

This file is the operating manual for AI agents and programmers working in this repository. It follows the public AGENTS.md convention and GitHub guidance that agent instructions should cover commands, tests, project structure, code style, git workflow, and boundaries.

References checked on 2026-06-14:
- AGENTS.md format: https://github.com/agentsmd/agents.md
- GitHub guidance on effective AGENTS.md files: https://github.blog/ai-and-ml/github-copilot/how-to-write-a-great-agents-md-lessons-from-over-2500-repositories/
- GitHub Copilot repository instructions: https://docs.github.com/copilot/how-tos/agents/copilot-coding-agent/best-practices-for-using-copilot-to-work-on-tasks
- OpenSSF secure development principles: https://openssf.org/blog/2023/12/03/openssf-releases-top-10-secure-software-development-guiding-principles/
- OpenSSF secure software concise guide: https://github.com/ossf/wg-best-practices-os-developers/blob/main/docs/Concise-Guide-for-Developing-More-Secure-Software.md
- OWASP Secure Coding Practices Quick Reference Guide: https://owasp.org/www-project-secure-coding-practices-quick-reference-guide/
- NIST SSDF SP 800-218 v1.1 and v1.2 draft: https://csrc.nist.gov/pubs/sp/800/218/final and https://csrc.nist.gov/pubs/sp/800/218/r1/ipd
- Microsoft high-DPI Win32 guidance: https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows
- Microsoft Defender command-line guidance: https://learn.microsoft.com/en-us/defender-endpoint/command-line-arguments-microsoft-defender-antivirus

## Mission

SuperZip is a Windows-native, AMD-only GPU-accelerated archive application written in modern C++20. Preserve the fundamental architecture: HIP is the AMD GPU acceleration boundary, `.szip` is the native SuperZip archive format, standard `.zip` remains compatibility-only through miniz 3.1.1, and all security-sensitive extraction paths must be validated before writing to disk.

## Non-Negotiable Boundaries

- Do not commit credentials, tokens, personal paths, user profiles, local machine names, build artifacts, archives, crash dumps, `.vs`, or generated binary outputs.
- Workflows in this repository must never create GitHub deployment records. Any job that uses a GitHub Actions `environment:` for secret governance must set `deployment: false`, and security scans must verify this before a push.
- Do not persist GitHub tokens in remotes or config. Use short-lived authentication only for a single push.
- Do not use WSL for this project unless a maintainer explicitly asks. The supported development path is Windows-native PowerShell, CMake, MSVC, and optional AMD ROCm/HIP.
- Do not launch the GUI during automated verification unless the task explicitly requires visual testing. Prefer CLI tests and static inspection.
- Do not change the AMD-only HIP acceleration strategy to CUDA, WebGPU, OpenCL, or a cross-vendor abstraction without explicit approval.
- Do not copy code, UI, or designs from reference repositories. Only use public projects for high-level comparison.

## Project Map

- `src/core/`: archive format, manifest, path safety, progress, Defender opt-in scan, SHA-256 integrity.
- `src/gpu/`: AMD HIP codec integration and CPU fallback used only when GPU is not required.
- `src/zip/`: ZIP compatibility using miniz 3.1.1.
- `src/cli/`: command-line entry point for deterministic automation.
- `src/app/`: native Win32 GUI. It must remain per-monitor-DPI aware and responsive at high refresh rates.
- `tests/cpp/`: focused C++ test harness.
- `tools/`: PowerShell build, test, security scan, benchmark, and HIP compile helpers.
- `.github/workflows/`: CI and opt-in security integrations.
- `.github/codeql/`: CodeQL scanning configuration.
- `.github/requirements/`: hash-locked Python scanner requirements for GitHub-hosted Linux jobs.
- `.github/workflows/release.yml`: manual x64 release, installer smoke, beta/stable track selection, and publishing workflow.
- `.agents/skills/` and `mcp/`: local helper skills/MCPs for future agents.

## Build And Test Commands

Use these from the repository root:

```powershell
tools\build.ps1 -Configuration Release
tools\test.ps1 -Configuration Release
tools\security_scan.ps1
```

Optional HIP build, only when AMD ROCm/HIP for Windows is installed and `HIP_PATH` is set:

```powershell
tools\build.ps1 -Configuration Release -EnableHip -HipArch gfx1201
build\Release\superzip_cli.exe gpu-info
```

Benchmark only after correctness tests pass:

```powershell
tools\bench.ps1 -Configuration Release
```

Package validation:

```powershell
tools\package.ps1 -Configuration Release
```

## Coding Standards

- Use C++20, RAII, value types, `std::filesystem`, `std::span`, explicit integer widths, and clear ownership.
- Keep memory bounded. Do not read whole archives or whole large files into memory when streaming APIs are available.
- Prefer deterministic behavior over hidden global state. Options must be explicit and safe by default.
- Keep dependencies minimal and pinned. New runtime dependencies require maintainer approval and a security rationale.
- Keep public errors actionable without revealing private paths beyond the user-selected path that failed.
- Keep comments concise and useful. Avoid restating syntax.

## Required Function Documentation

Every new or changed function must have a short contract comment immediately above its declaration or definition. Use this form:

```cpp
// Purpose: One sentence describing behavior.
// Inputs: Name each parameter and any important ownership, lifetime, unit, or security expectation.
// Outputs: Return value, mutation, thrown exception class, callback behavior, or side effect.
```

For simple private helpers, one compact line is acceptable if it still covers purpose, inputs, and outputs. Public headers should document every exported type and function. Security-sensitive functions must explicitly state trust boundaries and failure behavior.

## Security Rules

- Validate all archive entry names with `safe_join_archive_path` before extraction.
- Reject absolute paths, drive-rooted paths, UNC paths, traversal, reserved Windows device names, unsafe trailing dot/space, and invalid characters.
- Default to no overwrite. Overwrite is an explicit option.
- Verify block lengths, offsets, CRC32, and archive footer/index consistency before trusting payload metadata.
- Microsoft Defender scanning is opt-in and must run with `CREATE_NO_WINDOW`.
- SHA-256 integrity hashing is opt-in and must use Windows CNG on Windows.
- Keep CI layered: build, tests, secret scan, dependency review/security scanning, and automatic secrets-gated OpenVAS/Vulnetix lane.
- Before editing scanner workflows, read `docs/security-code-scanning.md`, verify action versions from official tags/releases, and pin actions by full commit SHA.
- Do not add GitHub Actions `environment:` blocks that can create deployment records. If an environment is necessary for secret governance, use `deployment: false` and document the reason in `docs/security-code-scanning.md`.
- Never commit Greenbone targets, credentials, Vulnetix organization IDs, scan credentials, or host-specific network details. Use GitHub repository secrets.

## GUI Rules

- Maintain `PerMonitorV2` DPI behavior in both manifest and startup code.
- All layout geometry must pass through DPI scaling or a DPI-aware layout helper.
- Rendering must be double-buffered or otherwise flicker-free.
- Progress repaint requests must be coalesced so fast jobs or 200 Hz displays do not flood the message queue.
- Text must use native DPI fonts and avoid bitmap scaling. Do not render low-resolution assets and stretch them.
- Check all pages, not only the main queue page, when making visual changes.
- Treat `resources/design/superzip-ui-iteration-4.png` as the current visual
  acceptance reference. Do not simplify the compact enterprise shell, command
  bar, icon rail, page-specific panels, bottom GPU status strip, or explicit
  settings/security pages.
- After any GUI rendering change, run `tools/gui_smoke.ps1 -Configuration
  Release` and inspect the generated screenshots for every page before calling
  the UI done.

## Git Workflow

- Inspect `git status --short` before staging.
- Stage only intentional source/docs/config changes.
- Never use destructive commands such as `git reset --hard` or `git checkout --` unless a maintainer explicitly requests them.
- Before pushing, run build/test/security scans and `rg` for token patterns and personal paths.
- After pushing, verify the remote URL does not contain credentials.
- Release changes must keep the package x64-only, attach SHA-256 checksum files,
  and run install/uninstall smoke tests before publishing.

## Agent Workflow

1. Read this file, `README.md`, `IMPLEMENTATION_PLAN.md`, and relevant local code before editing.
2. Make the smallest change that satisfies the request while preserving the architecture.
3. Add or update tests for behavior, security boundaries, and regressions.
4. Run the narrowest relevant tests first, then the standard build/test/security set.
5. Report what changed, what was verified, and any remaining risk.
