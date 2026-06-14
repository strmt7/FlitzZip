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
