---
name: superzip-build-test
description: Build, test, benchmark, and security-scan the SuperZip native C++ AMD HIP project on Windows. Use when changing SuperZip C++ source, build scripts, security checks, GPU code, ZIP/SUZIP archive logic, or GUI behavior.
---

# SuperZip Build/Test Skill

Use one of two lanes:

1. Portable CI lane:
   ```powershell
   tools/build.ps1 -Configuration Release
   tools/test.ps1
   tools/security_scan.ps1
   ```

2. Local AMD HIP lane:
   ```powershell
   tools/build.ps1 -Configuration Release -EnableHip -HipArch gfx1201
   build/Release/superzip_cli.exe gpu-info
   ```

Rules:

- Do not launch the GUI unless the user has been warned first.
- Use CLI smoke tests for archive validation.
- Keep `HIP_PATH`, Visual Studio, and architecture configurable.
- Never commit build output, archives, binaries, logs, or secrets.
- Run security tests after touching extraction, archive metadata, paths, subprocesses, workflows, or Defender integration.
