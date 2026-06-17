---
name: superzip-security
description: Security review and hardening workflow for SuperZip archive extraction, path handling, Defender integration, GitHub Actions scanning, secrets, and third-party dependency updates.
---

# SuperZip Security Skill

Before editing security-sensitive code, identify the boundary:

- Archive metadata parser.
- Destination path join and canonicalization.
- ZIP compatibility extraction.
- LHA/LZH compatibility extraction.
- SUZIP block metadata validation.
- Microsoft Defender opt-in scan.
- GitHub Actions scanner integration.

Required selection step:

```powershell
tools/verification_plan.ps1 -IncludeUntracked
tools/verify_changes.ps1 -IncludeUntracked
```

Use the full profile automatically when the plan escalates, a targeted check
fails, the changed path set is broad or unknown, or a wider security regression
is suspected:

```powershell
tools/verify_changes.ps1 -IncludeUntracked -Full
```

After push, wait only for relevant workflows selected by the classifier:

```powershell
tools/wait_relevant_workflows.ps1 -Commit <sha> -Mode final
```

For non-critical intermediate commits, `-Mode opportunistic` may be used only
when `workflowWaitPolicy.deferAllowed=true`. Security, workflow, verifier, MCP,
skill, and full-escalation changes must not be reported complete until final
mode and any required post-push audit pass.
Fuzzing is intentionally long-running. It is observed separately from normal
blocking waits; check it opportunistically during iteration and include it in
the blocking wait only for final handoff or release.

Add or update tests for:

- `..`, absolute paths, drive paths, UNC paths.
- Reserved Windows names.
- Existing-file overwrite refusal.
- Corrupt payload and CRC mismatch.
- Oversized or malformed archive metadata.
- Symbolic links and other unsupported special-file entries in compatibility
  formats.

Do not store credentials, PATs, org IDs, scan targets, or Defender results in
tracked files.

Workflow and release hardening rules:

- Do not add GitHub Actions `environment:` blocks, `deployment:` keys, or any
  workflow mechanism that creates deployment records.
- Do not hide scanner findings with broad exclusions, placeholder tests, or
  event-specific jobs that normally show as skipped. A narrow generated-output
  skip is acceptable only when the skipped path is documented and not product
  source.
- Keep release artifacts HIP-enabled, Windows x64-only, and equivalent between
  MSI and portable ZIP. CPU-only artifacts are validation-only and must not be
  published.
- Do not track extracted runtime binaries. Keep upstream packages under
  `third_party/upstream/**`, record checksums and license notes, and extract
  runtime DLLs into the build tree only after checksum verification.
- LHA/LZH support must stay extraction-only, in-process, and backed by the
  vendored Lhasa 0.5.0 decoder. Keep SuperZip's two-pass path/payload
  validation around Lhasa and preserve the unmodified upstream archive under
  `third_party/upstream/lhasa/0.5.0/`.
- WIM support must stay extraction-only for standalone WIMs, in-process, and
  backed by the bundled app-local wimlib 1.14.5 DLL. Keep split WIMs rejected
  until multipart handling is deliberately implemented, reject reparse points,
  hard links, alternate data streams, device entries, encrypted/offline/virtual
  files, and publish only rechecked staged regular files through SuperZip's
  verified temporary-file path.
- LZMA support must stay extraction-only for legacy `.lzma` LZMA-Alone streams,
  in-process, and backed by the vendored LZMA SDK 26.01 decoder. Keep it
  single-file, reject oversized dictionaries and decoded output, and publish
  only through SuperZip's verified temporary-file path.
- Lzip support must stay extraction-only for `.lz` and TAR-stream-only for
  `.tar.lz`/`.tlz`, in-process, and backed by SuperZip's lzip wrapper over the
  vendored LZMA SDK 26.01 decoder. Keep version, dictionary, EOS, CRC32,
  data-size, and member-size checks mandatory for every member, reject trailing
  non-member data, and publish only through verified temporary paths.
- CPIO.GZ support must stay in-process as a Gzip-filtered SVR4 new ASCII CPIO
  stream. Keep the two-pass validation model, reject unsafe inner paths,
  unsupported special files, overwrite attempts, malformed Gzip trailers, and
  `070702` checksum mismatches before publishing output.
- The only acceptable open code-scanning findings are Scorecard
  `MaintainedID`, `CodeReviewID`, `BranchProtectionID`, and
  `CIIBestPracticesID`.
- `tools/security_scan.ps1` runs the changed-code refactor gate. Do not bypass
  it with broad exclusions; split large functions and add required function
  contracts before pushing.
- Do not wait for unrelated post-push workflows. Use
  `tools/wait_relevant_workflows.ps1`, which supports final, opportunistic, and
  defer modes and also runs `tools/github_post_push_audit.ps1` when workflow or
  verifier changes require it.
- Keep `.github/workflows/lint.yml` aligned with languages actually present in
  the repo. The lint lane covers C/C++ formatting, PowerShell static analysis,
  Python helper lint/format, YAML, Markdown, and CMake; do not add misleading
  badges or jobs for unused languages.
- Keep CodeQL C++ on `windows-2022` with `build-mode: manual` and
  `tools/build.ps1 -Configuration Release -CpuOnlyValidation`. Build-free C/C++
  analysis under-models this Win32/HIP/vendored-C codebase and produces
  parser-artifact alerts; do not restore it for speed.
- Workflow `run` blocks must not interpolate `${{ github.* }}` directly. Route
  GitHub context through quoted environment variables so script-injection
  scanners and local hygiene checks agree.
- The canonical `superzip-logo-mark` artwork is immutable for AI agents. Text
  around the logo can change, but the mark's path geometry and stroke contract
  must stay under `tools/verify_brand_assets.ps1`.
- Use `docs/engineering-learning-loop.md` for repeat mistakes: fix the root
  cause, add a narrow invariant, and avoid broad suppressions or process-only
  rules.
