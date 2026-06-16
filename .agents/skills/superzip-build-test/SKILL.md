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
   tools/github_post_push_audit.ps1
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
- Pinned upstream provenance archives under `third_party/upstream/**` are the
  only source-controlled archive exception. Do not commit extracted `.dll`,
  `.exe`, `.obj`, or package staging output.
- CPU/GPU performance benchmarks must use `tools/bench.ps1` in its default
  memory-only mode. Required proof fields are `memory_only=true` and
  `disk_write_bytes=0` for both forced-CPU and required-AMD-HIP lanes. Do not
  run multi-GB filesystem benchmarks during development; use
  `tools/storage_smoke.ps1` or the 64 MiB-capped filesystem smoke only for the
  archive write/read path.
- CPU/GPU performance benchmarks must compare the same compression level and
  report compression ratio. Use level 5 as the balanced release baseline unless
  the task is explicitly a level sweep.
- Block-size changes must validate every product option:
  `-BlockSizeKiB 256,1024,4096,16384`.
- Run security tests after touching extraction, archive metadata, paths, subprocesses, workflows, or Defender integration.
- Release/deployment MSIs use the default `perMachine` scope, preselect
  `C:\Program Files\SuperZip`, and require normal Windows elevation. Local
  non-admin MSI tests must explicitly configure
  `tools\build.ps1 -Configuration Release -MsiInstallScope perUser`; never
  publish that per-user MSI as a product release.
- Product installers must expose a user-visible `Create Desktop shortcut`
  option. Treat silent shortcut creation as an installer bug.
- The GUI is intentionally fixed-size for release until responsive resizing is
  deliberately rebuilt and tested. Keep PerMonitorV2 DPI behavior, crisp vector
  or multi-resolution icons, and no stretched bitmap assets.
- For GUI changes, run `tools/gui_smoke.ps1 -Configuration Release`. It must
  open every tab and click/use every visible button, toggle, dropdown/select
  field, and main action path at least once, including Add files, Add folder,
  Clear, drag/drop, destination selection, Queue/Compress/Extract/Security/GPU/
  History/Settings actions, then inspect every generated page screenshot. It
  must visibly verify Add files, Add folder, native drag/drop, and every
  dropdown menu instead of only clicking controls.
- Visual changes must preserve the compact enterprise design reference in
  `resources/design/superzip-ui-iteration-4.png` and the polish reference in
  `resources/design/superzip-ui-imagegen-polish-20260615.png`. Do not simplify
  or remove the brand-only top bar, Queue-local Add files/Add folder/Clear
  actions, left navigation, bottom GPU status strip, or dedicated settings pages.
- LHA/LZH compatibility changes must exercise successful nested extraction,
  overwrite refusal, truncated/corrupt payload rejection, absolute-path
  rejection, parent-directory rejection, and symbolic-link rejection. Keep the
  decoder in process through the vendored Lhasa 0.5.0 library and preserve the
  original upstream release archive/checksum under `third_party/upstream/lhasa/`.
