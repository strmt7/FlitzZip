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

Required checks:

```powershell
tools/build.ps1 -Configuration Release
tools/test.ps1
tools/security_scan.ps1
tools/github_post_push_audit.ps1
```

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
- The only acceptable open code-scanning findings are Scorecard
  `MaintainedID`, `CodeReviewID`, `BranchProtectionID`, and
  `CIIBestPracticesID`.
- `tools/security_scan.ps1` runs the changed-code refactor gate. Do not bypass
  it with broad exclusions; split large functions and add required function
  contracts before pushing.
