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
- CMake CPack WiX generator scope guidance: https://cmake.org/cmake/help/latest/cpack_gen/wix.html
- Microsoft Windows Installer `ProgramFiles64Folder`: https://learn.microsoft.com/en-us/windows/win32/msi/programfiles64folder

## Mission

SuperZip is a Windows-native, AMD-only GPU-accelerated archive application written in modern C++20. Preserve the fundamental architecture: HIP is the AMD GPU acceleration boundary, `.suzip` is the native SuperZip archive format, standard `.zip` remains compatibility-only through miniz 3.1.1, `.tar` remains native compatibility-only through the bounded TAR adapter, `.tar.gz`/`.tgz` remain compatibility-only through the TAR stream adapter over miniz raw deflate, `.tar.bz2`/`.tbz`/`.tbz2` remain compatibility-only through the TAR stream adapter over vendored libbzip2 1.0.8, `.tar.xz`/`.txz` extraction remains compatibility-only through the TAR stream adapter over vendored XZ Embedded, `.tar.zst`/`.tzst` remain compatibility-only through the TAR stream adapter over the bundled app-local libzstd 1.5.7 runtime, `.gz` remains single-file compatibility-only through miniz raw deflate, `.bz2` remains single-file compatibility-only through vendored libbzip2 1.0.8, `.xz` extraction remains single-file compatibility-only through vendored XZ Embedded, `.zst`/`.zstd` remain single-file compatibility-only through the bundled app-local libzstd 1.5.7 runtime, legacy Unix Compress `.Z` remains native single-file compatibility-only through the bounded LZW adapter, `.cpio` remains native compatibility-only through the bounded CPIO adapter, `.ar` remains native compatibility-only through the bounded AR adapter, `.deb` extraction remains native compatibility-only through the bounded AR adapter for outer package members, `.iso` extraction remains native read-only basic ISO 9660 compatibility through the bounded ISO adapter, `.rpm` extraction remains native read-only compatibility through the bounded RPM package adapter over supported CPIO payloads, and all security-sensitive extraction paths must be validated before writing to disk.

## Non-Negotiable Boundaries

- Do not commit credentials, tokens, personal paths, user profiles, local machine names, build artifacts, crash dumps, `.vs`, or generated binary outputs. The only archive files allowed in source control are pinned upstream provenance artifacts under `third_party/upstream/**` with recorded checksums and licensing notes.
- Workflows in this repository must never create GitHub deployment records. Do not add GitHub Actions `environment:` blocks or `deployment:` keys. Do not bind private scanner credentials directly in workflow YAML; use the documented OIDC broker pattern for Greenbone/OpenVAS live scan configuration.
- Do not persist GitHub tokens in remotes or config. Use short-lived authentication only for a single push.
- Do not use WSL for this project unless a maintainer explicitly asks. The supported development path is Windows-native PowerShell, CMake, MSVC, and optional AMD ROCm/HIP.
- Do not launch the GUI during automated verification unless the task explicitly requires visual testing. Prefer CLI tests and static inspection.
- Maintainer authorization is recorded for AI agents to accept EULAs required
  by pinned SuperZip build, benchmark, packaging, and verification tooling, such
  as the WiX v7 OSMF EULA, when that acceptance is necessary to run repository
  validation. Keep acceptance scoped to this repo's documented toolchain, record
  it in the command path, and do not accept unrelated software, driver,
  service, or personal-account agreements.
- Do not change the AMD-only HIP acceleration strategy to CUDA, WebGPU, OpenCL, or a cross-vendor abstraction without explicit approval.
- Before proposing alternate GPU compute stacks, read
  `docs/compression-backend-evaluation.md`. rocPRIM can be used as a HIP
  building block; rocSPARSE, OpenCL, SYCL, and OpenMP offload are not approved
  replacement archive-codec paths for this repository.
- Before changing block-size options, compression levels, benchmark policy, or
  CPU/GPU performance claims, read
  `docs/performance-block-size-validation.md` and
  `docs/compression-level-and-benchmark-suite.md`, then keep the RAM-only
  benchmark gates intact.
- Before changing archive-format recognition or compatibility support, read
  `docs/archive-format-support.md`. Do not add ZIP-container aliases as
  user-facing archive formats unless a maintainer explicitly requests package
  inspection.
- Before performing repo-wide refactoring or automatic cleanup, read
  `docs/refactoring-governance.md` and run `tools\refactor_audit.ps1`.
- Do not copy code, UI, or designs from reference repositories. Only use public projects for high-level comparison.

## Engineering Quality Baseline

These rules were checked against official guidance on 2026-06-16: NIST SSDF,
CISA Secure by Design, OWASP Secure Coding Practices, Microsoft SDL, GitHub
Actions secure-use guidance, OpenSSF Scorecard, and SLSA v1.2.

- Keep changes small, reviewable, and behavior-preserving unless the task
  explicitly asks for a behavior change.
- Design secure-by-default paths: fail closed, make risky behavior opt-in, and
  keep trust boundaries visible in code comments and tests.
- Use least privilege everywhere: workflow permissions, filesystem writes,
  process creation, GPU/device access, and installer actions.
- Prefer pinned, provenance-recorded dependencies. Do not track extracted
  runtime binaries such as `.dll` or `.exe`; extract them from pinned upstream
  packages during build and verify checksums before use.
- Fix scanner findings at root cause. Suppressions need documented evidence and
  maintainer approval; they are not a default remediation strategy.
- Preserve supply-chain evidence: checksums, third-party notices, SBOM output,
  release notes, and reproducible build inputs.
- After every pushed remediation, verify workflows, deployments, and open
  code-scanning alerts with `tools\github_post_push_audit.ps1`.

## Project Map

- `src/ar/`: Unix AR and AR-based Debian outer-container compatibility adapter for regular-file members with two-pass path validation and verified file publication.
- `src/bzip2/`: Bzip2 compatibility streams and `.bz2` single-file adapter using vendored libbzip2 1.0.8.
- `src/core/`: archive format, manifest, path safety, progress, Defender opt-in scan, SHA-256 integrity.
- `src/cpio/`: CPIO compatibility adapter for SVR4 new ASCII archives with two-pass path validation and verified file publication.
- `src/gzip/`: Gzip compatibility streams using miniz raw deflate with CRC32/ISIZE verification.
- `src/gpu/`: AMD HIP codec integration and CPU fallback used only when GPU is not required.
- `src/iso/`: Read-only basic ISO 9660 compatibility adapter with two-pass path validation and verified file publication.
- `src/rpm/`: Read-only RPM package adapter that validates RPM headers, decodes supported CPIO payload compression, and delegates extracted package paths to the CPIO adapter.
- `src/tar/`: TAR, TAR.GZ, TAR.BZ2, TAR.XZ, and TAR.ZST compatibility adapter with two-pass path validation and verified file publication.
- `src/unix_compress/`: Unix Compress `.Z` single-file compatibility adapter with bounded LZW dictionaries and verified file publication.
- `src/xz/`: Extract-only XZ compatibility stream and `.xz` single-file adapter using vendored XZ Embedded.
- `src/zstd/`: Zstandard compatibility streams, runtime-DLL loader, and `.zst`/`.zstd` single-file adapter using the bundled app-local libzstd 1.5.7 runtime.
- `src/zip/`: ZIP compatibility using miniz 3.1.1.
- `src/cli/`: command-line entry point for deterministic automation.
- `src/app/`: native Win32 GUI. It must remain per-monitor-DPI aware and responsive at high refresh rates.
- `tests/cpp/`: focused C++ test harness.
- `fuzz/`: libFuzzer targets, dictionaries, and options for parser hardening.
- `tools/`: PowerShell build, test, security scan, benchmark, and HIP compile helpers.
- `third_party/miniz/`: patched production miniz 3.1.1 copy used by the build.
- `third_party/upstream/miniz/3.1.1/`: unmodified upstream miniz 3.1.1 source archive and checksum for provenance.
- `third_party/bzip2/`: production libbzip2 1.0.8 copy plus the SuperZip link shim for `bz_internal_error`.
- `third_party/upstream/bzip2/1.0.8/`: unmodified upstream bzip2 1.0.8 source archive and checksum for provenance.
- `third_party/xz_embedded/`: production XZ Embedded decoder copy used for extract-only `.xz` and `.tar.xz` compatibility.
- `third_party/upstream/xz-embedded/ae63ae3a36ed01724674e8f3d750dc47bf125410/`: upstream XZ Embedded source archive and checksum for provenance.
- `third_party/zstd/`: production Zstandard runtime metadata and license files. Do not track extracted runtime DLLs here.
- `third_party/upstream/zstd/v1.5.7/`: upstream Zstandard source archive, official Win64 runtime package, and checksums for provenance.
- `.github/workflows/`: CI and opt-in security integrations.
- `.clusterfuzzlite/`: ClusterFuzzLite build integration for C++ sanitizer fuzzing.
- `.github/codeql/`: CodeQL scanning configuration.
- `.github/requirements/`: hash-locked Python scanner requirements for GitHub-hosted Linux jobs.
- `.github/workflows/release.yml`: manual x64 release, installer smoke, beta/stable track selection, and publishing workflow.
- `.github/workflows/scorecard.yml`: default-branch OSSF Scorecard workflow.
- `.agents/skills/` and `mcp/`: local helper skills/MCPs for future agents.

## Build And Test Commands

Use these from the repository root. The normal local build is HIP-enabled and
requires `HIP_PATH`:

```powershell
tools\build.ps1 -Configuration Release
tools\test.ps1 -Configuration Release
tools\security_scan.ps1
tools\github_post_push_audit.ps1
```

Use CPU-only validation only for hosted CI or static-analysis jobs that cannot
install AMD HIP:

```powershell
tools\build.ps1 -Configuration Release -CpuOnlyValidation
tools\test.ps1 -Configuration Release
```

Benchmark only after correctness tests pass:

```powershell
tools\gpu_proof.ps1 -Configuration Release
tools\storage_smoke.ps1 -Configuration Release
tools\bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1 -BlockSizeKiB 256,1024,4096,16384
build\Release\superzip_cli.exe benchmark-suite --profile Mixed --compression-level 5 --tune
```

`tools\bench.ps1` is memory-only by default and must report
`memory_only=true` and `disk_write_bytes=0` for every CPU and GPU lane. During
development, CPU/GPU benchmarking must stay RAM-only to avoid unnecessary SSD
wear. Do not run multi-GB generated filesystem benchmarks. The only normal
storage validation is `tools\storage_smoke.ps1` or `tools\bench.ps1 -Mode
Filesystem` with a bounded smoke payload of at most 64 MiB.

Parser fuzzing is automatic in GitHub Actions. Local sanitizer fuzzing requires
Docker and uses ClusterFuzzLite's Clang/libFuzzer toolchain:

```powershell
tools\fuzz.ps1 -Runs 512
```

HIP package validation:

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
- Treat refactoring as behavior-preserving work. Use small, reviewable steps,
  measure before/after when performance is involved, and never combine broad
  cleanup with functional changes unless the coupling is documented.

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
- Compatibility archive support must use in-process parsers/writers. Do not
  shell out to `tar`, 7-Zip, WinRAR, PowerShell compression cmdlets, or other
  host tools from product code.
- CPIO compatibility is limited to regular files and directories in the SVR4
  new ASCII variants. Keep links, hard-link metadata, devices, FIFOs, and
  special files rejected unless a maintainer approves a dedicated security and
  UI policy.
- AR compatibility is limited to regular-file members. Keep symbol-table and
  string-table metadata non-extracting, preserve nested paths through supported
  long-name variants, and validate all member names before output.
- DEB compatibility is extraction-only and uses the native AR outer-container
  path. Do not install packages, execute maintainer scripts, or silently unpack
  nested package payloads without a dedicated product requirement and tests.
- RPM compatibility is extraction-only and uses the native RPM header scanner
  plus the native CPIO adapter for package payloads. Do not install packages,
  execute scripts, trust package names as extraction paths, or support payload
  formats other than CPIO without a dedicated requirement and tests. Keep
  compressed and decoded temporary payload spooling explicitly capped.
- Unix Compress `.Z` compatibility is single-file only. Keep the LZW dictionary
  bounded by the stream-declared maxbits, validate magic/header flags, and
  publish extraction output only through the verified temporary-file path.
- Bzip2 compatibility is single-file for `.bz2` and TAR-stream-only for
  `.tar.bz2`/`.tbz`/`.tbz2`. Keep libbzip2 in-process, reject trailing data in
  single-member streams unless concatenated-member support is deliberately added
  with tests, and publish extraction output only through verified temporary
  paths.
- XZ compatibility is extract-only for `.xz` and TAR-stream-only for
  `.tar.xz`/`.txz`. Keep XZ Embedded in-process, keep the decoder memory cap,
  reject unsupported filters/checks instead of skipping verification, and do not
  advertise XZ creation until an encoder path is deliberately added with tests.
- Zstandard compatibility is single-file for `.zst`/`.zstd` and TAR-stream-only
  for `.tar.zst`/`.tzst`. Keep libzstd in-process through the app-local DLL
  loader, keep the decoder window cap, validate runtime version `10507`, keep
  SuperZip-created frame checksums enabled, and reject malformed streams or
  trailing garbage before publishing output.
- Reject absolute paths, drive-rooted paths, UNC paths, traversal, reserved Windows device names, unsafe trailing dot/space, and invalid characters.
- Default to no overwrite. Overwrite is an explicit option.
- Verify block lengths, offsets, CRC32, and archive footer/index consistency before trusting payload metadata.
- Microsoft Defender scanning is opt-in and must run with `CREATE_NO_WINDOW`.
- SHA-256 integrity hashing is opt-in and must use Windows CNG on Windows.
- Keep CI layered: build, tests, secret scan, dependency review/security scanning, an always-on Greenbone/OpenVAS integration audit, and an OIDC-brokered authorized live OpenVAS/Vulnetix lane.
- Keep ClusterFuzzLite fuzzing active for archive metadata and path-handling code. Fuzz targets must exercise real product parser code and must not be placeholder functions added only to satisfy scanner heuristics.
- Product release artifacts must be HIP-enabled. Do not publish CPU-only portable ZIPs or MSIs as SuperZip releases.
- Product benchmark claims must sweep the production SUZIP block-size choices:
  256 KiB, 1 MiB, 4 MiB, and 16 MiB. The benchmark harness must compare
  forced-CPU and required-AMD-HIP lanes in RAM and must not write multi-GB
  generated workloads to SSDs.
- Product benchmark claims must compare CPU and GPU at the same compression
  level and report compression ratio. Level 5 is the balanced release baseline;
  the exposed non-store tuning levels are 1, 3, 5, 7, and 9.
- Product MSI releases must be per-machine installers that preselect
  `C:\Program Files\SuperZip` and use normal Windows elevation when required.
  Use `tools\build.ps1 -MsiInstallScope perUser` only for local non-admin
  coding or installer smoke tests, and never publish that per-user MSI as a
  product release.
- Installer UI must offer an explicit `Create Desktop shortcut` choice. Keep
  the option visible to the user instead of silently creating or suppressing the
  shortcut.
- Do not redistribute AMD's HIP runtime DLL from a developer SDK. AMD documents that runtime as supplied by the AMD GPU driver; SuperZip delay-loads it and reports missing prerequisites through `dependency-check`.
- Keep event-specific checks in event-specific workflows. Do not add jobs that are expected to show as skipped in normal push CI.
- Before editing scanner workflows, read `docs/security-code-scanning.md`, verify action versions from official tags/releases, and pin actions by full commit SHA.
- Do not add GitHub Actions `environment:` blocks or `deployment:` keys. This repository uses repository variables plus an external OIDC broker instead of Actions environments so workflows cannot create deployment records.
- Never commit Greenbone targets, credentials, Vulnetix organization IDs, scan credentials, or host-specific network details. Keep live scanner secrets outside GitHub Actions and return them only from the OIDC broker after claim validation.
- Preserve the patched/original miniz split: production changes go under `third_party/miniz`, and the upstream archive under `third_party/upstream/miniz/3.1.1` stays unmodified.

## GUI Rules

- Maintain `PerMonitorV2` DPI behavior in both manifest and startup code.
- All layout geometry must pass through DPI scaling or a DPI-aware layout helper.
- Keep the release GUI fixed-size and non-resizable until resizing is
  deliberately re-enabled. Do not ship a resizable window until every page has a
  responsive layout pass, high-DPI screenshot coverage, and click-hit regression
  coverage at small, default, 4K, and high-refresh display settings.
- Rendering must be double-buffered or otherwise flicker-free.
- Progress repaint requests must be coalesced so fast jobs or 200 Hz displays do not flood the message queue.
- Tab transitions and toggle changes may use short non-blocking animations, but
  they must run on bounded timers, never block archive work, never grow memory
  usage, and remain visually stable when repaint cadence exceeds 200 Hz.
- Text must use native DPI fonts and avoid bitmap scaling. Do not render
  low-resolution assets and stretch them.
- Icons must be crisp DPI-aware vector drawing or multi-resolution resources.
  The application icon must use the same SuperZip stacked-logo mark as the
  in-app brand mark, and navigation icons must have consistent optical size,
  stroke weight, and alignment.
- Visible controls must share drawing and hit-test geometry. After drag/drop,
  Queue, Compress, Extract, Security, History, GPU, Preferences, and About page
  controls must remain clickable; fix the shared geometry instead of adding
  page-specific click offsets that can drift from rendering.
- Never add or redesign a GUI feature by breaking an existing interaction.
  File-picker queueing, folder-picker queueing, drag/drop queueing, row
  selection, destination selection, dropdowns, toggles, tab changes, and primary
  actions are regression boundaries and must keep working unless a maintainer
  explicitly removes that capability.
- Dropdown arrows must be vertically centered in their value boxes and use one
  consistent shape and inset throughout the app.
- Toggle rows must rely on the switch state itself. Do not add redundant
  `Enabled` or `Disabled` text labels next to settings toggles.
- Check all pages, not only the main queue page, when making visual changes.
- Treat `resources/design/superzip-ui-iteration-4.png` as the current visual
  acceptance reference. Do not simplify the compact enterprise shell, command
  bar, icon rail, page-specific panels, bottom GPU status strip, or explicit
  settings/security pages.
- After any GUI rendering or interaction change, run `tools/gui_smoke.ps1
  -Configuration Release`. The smoke must open every tab, click every visible
  button, toggle, dropdown/select field, and main action path at least once,
  exercise Add files, Add folder, Clear, drag/drop, destination selection,
  Queue/Compress/Extract/Security/GPU/History/Settings actions, and close
  without leaving modal dialogs or windows open. The smoke must assert that
  queued items are visibly rendered after file picker, folder picker, and native
  drag/drop injection; clicking a control without verifying its effect is not
  enough. Inspect the generated screenshots for every page before calling the UI
  done.

## Git Workflow

- Inspect `git status --short` before staging.
- Stage only intentional source/docs/config changes.
- Never use destructive commands such as `git reset --hard` or `git checkout --` unless a maintainer explicitly requests them.
- Before pushing, run build/test/security scans and `rg` for token patterns and personal paths.
- After pushing, verify the remote URL does not contain credentials.
- Release changes must keep the package x64-only, attach SHA-256 checksum files,
  and run install/uninstall smoke tests before publishing.
- `0.1.0` is the current beta release. Until a maintainer explicitly opens the
  next version line, GitHub release runs should replace the existing `0.1.0`
  release/tag and their release notes must list the actual fixes, created
  assets, validation work, and known limitations.

## Agent Workflow

1. Read this file, `README.md`, `IMPLEMENTATION_PLAN.md`, and relevant local code before editing.
2. Make the smallest change that satisfies the request while preserving the architecture.
3. Add or update tests for behavior, security boundaries, and regressions.
4. Run the narrowest relevant tests first, then the standard build/test/security set.
5. Report what changed, what was verified, and any remaining risk.
