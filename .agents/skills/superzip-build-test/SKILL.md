---
name: superzip-build-test
description: Build, test, benchmark, and security-scan the SuperZip native C++ AMD HIP project on Windows. Use when changing SuperZip C++ source, build scripts, security checks, GPU code, ZIP/SUZIP archive logic, or GUI behavior.
---

# SuperZip Build/Test Skill

Start by asking the change-aware verifier what is relevant:

```powershell
tools/verification_plan.ps1 -IncludeUntracked
tools/verify_changes.ps1 -IncludeUntracked
```

Use the full profile only when the plan escalates, the change is broad or
unknown, verification tooling changed, or a targeted command fails:

```powershell
tools/verify_changes.ps1 -IncludeUntracked -Full
```

For pushed commits, follow the plan's `workflowWaitPolicy`. During multi-commit
feature work, use `tools/wait_relevant_workflows.ps1 -Commit <sha> -Mode
opportunistic` only when deferral is allowed, and keep developing while runs are
active. Before handoff or release, and always for workflow/verifier/MCP/skill or
full-escalation changes, use `-Mode final`.
Fuzzing is long-running and must not be waited for during routine iteration.
Use `-Mode opportunistic -IncludeLongRunning` to sample it periodically, and use
`-Mode final -FinalCommit` for the final handoff or release commit.

Manual lanes remain available when the plan selects them:

1. Portable CI lane:
   ```powershell
   tools/build.ps1 -Configuration Release
   tools/test.ps1
   tools/lint.ps1 -CppMode Changed
   tools/security_scan.ps1
   tools/github_post_push_audit.ps1
   ```

2. Local AMD HIP lane:
   ```powershell
   tools/build.ps1 -Configuration Release -EnableHip -HipArch gfx1201
   build/Release/superzip_cli.exe gpu-info
   ```

Rules:

- Do not run broad checks merely because they exist. Run the plan-selected
  checks, then escalate automatically when evidence points to a wider problem.
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
- Brand/logo changes must start from `resources/brand/superzip-logo.svg`.
  Regenerate `resources/app/superzip.ico`, rely on the generated Win32 logo
  geometry header, and run `tools/verify_brand_assets.ps1`. Do not add alternate
  hand-drawn logo variants, and do not embed README or UI logo assets from any
  source other than the canonical SVG. AI agents may move or resize the
  `superzip-logo-mark` group for layout and may edit surrounding text, but must
  not modify mark path geometry, stroke styling, layer count, or
  source-of-truth metadata.
- Do not run builds or packaging in parallel with GUI smoke tests or an open
  build-output `SuperZip.exe`; file locks produce misleading linker failures.
- When a workflow, badge, verifier, or agent-routing mistake is found, read
  `docs/engineering-learning-loop.md` and add the narrowest enforceable
  invariant that prevents recurrence without changing product direction or
  adding unrelated heavyweight gates.
- LHA/LZH compatibility changes must exercise successful nested extraction,
  overwrite refusal, truncated/corrupt payload rejection, absolute-path
  rejection, parent-directory rejection, and symbolic-link rejection. Keep the
  decoder in process through the vendored Lhasa 0.5.0 library and preserve the
  original upstream release archive/checksum under `third_party/upstream/lhasa/`.
- WIM compatibility changes must exercise successful standalone WIM extraction,
  overwrite refusal, corrupt payload rejection, app-local `libwim-15.dll`
  loading, and documentation/runtime-manifest updates. Keep extraction
  in-process through the bundled wimlib 1.14.5 DLL, preserve the original
  upstream package/checksums under `third_party/upstream/wimlib/`, and do not
  call `wimlib-imagex.exe`, DISM, PowerShell, Explorer, or host WIM tools.
- LZMA compatibility changes must exercise successful `.lzma` single-file
  extraction, overwrite refusal, truncated/corrupt stream rejection, oversized
  dictionary rejection, and fuzz coverage. Keep extraction in-process through
  the vendored LZMA SDK 26.01 decoder and do not call `7z.exe`, PowerShell, or
  host archive tools.
- Lzip compatibility changes must exercise successful `.lz` single-file
  extraction, concatenated-member extraction, `.tar.lz`/`.tlz` TAR extraction,
  overwrite refusal, corrupt trailer rejection, invalid dictionary rejection,
  unsafe TAR path rejection, and fuzz coverage. Keep extraction in process
  through SuperZip's lzip wrapper over the vendored LZMA SDK 26.01 decoder.
- CPIO.GZ compatibility changes must exercise successful `.cpio.gz`/`.cpgz`
  extraction, overwrite refusal, unsafe inner CPIO path rejection, inner
  `070702` checksum rejection, Gzip trailer rejection, and CPIO parser fuzz
  coverage. Keep the Gzip wrapper and CPIO parser in process and do not stage
  a full decoded archive to disk.
