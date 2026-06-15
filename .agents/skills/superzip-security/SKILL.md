---
name: superzip-security
description: Security review and hardening workflow for SuperZip archive extraction, path handling, Defender integration, GitHub Actions scanning, secrets, and third-party dependency updates.
---

# SuperZip Security Skill

Before editing security-sensitive code, identify the boundary:

- Archive metadata parser.
- Destination path join and canonicalization.
- ZIP compatibility extraction.
- SUZIP block metadata validation.
- Microsoft Defender opt-in scan.
- GitHub Actions scanner integration.

Required checks:

```powershell
tools/build.ps1 -Configuration Release
tools/test.ps1
tools/security_scan.ps1
```

Add or update tests for:

- `..`, absolute paths, drive paths, UNC paths.
- Reserved Windows names.
- Existing-file overwrite refusal.
- Corrupt payload and CRC mismatch.
- Oversized or malformed archive metadata.

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
