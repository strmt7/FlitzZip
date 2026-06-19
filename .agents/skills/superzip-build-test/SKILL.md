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
The waiter resolves local refs to full commit SHAs and performs a GitHub CLI
preflight before polling. If authentication, authorization, or `gh run list`
fails, fix that root cause immediately; do not keep waiting on a state where
every selected workflow appears missing.
Fuzzing is long-running and must not be waited for during routine iteration.
Use `-Mode opportunistic -IncludeLongRunning` to sample it periodically, and use
`-Mode final -FinalCommit` for the final handoff or release commit.

Manual lanes remain available when the plan selects them:

1. Portable CI lane:
   ```powershell
   tools/build.ps1 -Configuration Release
   tools/test.ps1
   tools/lint.ps1 -CppMode Changed
   tools/compatibility_interop_smoke.ps1 -Configuration Release
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
  `-BlockSizeKiB 256,512,1024,2048,4096,8192,16384`.
- Run security tests after touching extraction, archive metadata, paths, subprocesses, workflows, or Defender integration.
- Run `tools/compatibility_interop_smoke.ps1 -Configuration Release` after
  changing ZIP, TAR, compressed TAR, CPIO, CPIO.GZ, AR, or shared
  compatibility stream writers. Non-SUZIP create formats must be readable by
  independent standard readers, not only by SuperZip.
- Release/deployment MSIs use the default `perMachine` scope, preselect
  `C:\Program Files\SuperZip`, and require normal Windows elevation. Local
  non-admin MSI tests must explicitly configure
  `tools\build.ps1 -Configuration Release -MsiInstallScope perUser`; never
  publish that per-user MSI as a product release.
- Installer launch and release-validation waits must be bounded. MSI
  install/uninstall smoke phases default to 300 seconds, and HIP SDK installer
  setup must fail explicitly on timeout. Windows UAC consent is OS-owned and
  cannot be timed from inside the MSI.
- Product installers must expose a user-visible `Create Desktop shortcut`
  option. Treat silent shortcut creation as an installer bug.
- The GUI is intentionally fixed-size for release until responsive resizing is
  deliberately rebuilt and tested. Keep PerMonitorV2 DPI behavior, crisp vector
  or multi-resolution icons, and no stretched bitmap assets.
- SuperZip-owned confirmations, prompts, secondary windows, and modal surfaces
  must use the same visual system, typography, colors, spacing, focus behavior,
  and interaction quality as the main window. Do not use raw `MessageBoxW` or
  other platform-default product dialogs for SuperZip confirmations. OS-owned
  file/folder pickers, UAC/elevation prompts, Defender/security broker prompts,
  crash dialogs, and installer privilege prompts are the normal exceptions.
- Keep first-frame rendering dark and flicker-free: the Win32 class background
  and `WM_ERASEBKGND` must use the app background color, and all normal painting
  remains double-buffered.
- For GUI changes, run `tools/gui_smoke.ps1 -Configuration Release`. It must
  open every tab and click/use every visible button, toggle, dropdown/select
  field, and main action path at least once, including Add files, Add folder,
  Clear, drag/drop, destination selection, Queue/Compress/Extract/Security/System/
  History/Settings actions, then inspect every generated page screenshot. It
  must visibly verify Add files, Add folder, native drag/drop, and every
  dropdown menu instead of only clicking controls.
- GUI smoke source contracts must continue to reject process-cwd destination
  defaults, process-only GPU graph sampling, VRAM-as-GPU-graph history, missing
  Queue overflow scrolling, and unrequested System graph cadence changes.
- Compress and Extract defaults must display the current user's Downloads known
  folder when no destination has been applied. Do not regress to `System32` or
  any process current directory path.
- Queue overflow must keep the header fixed, accept drops only inside the table,
  keep row/header tick hit testing aligned, and prove scrollbar rendering in
  screenshots when queued rows exceed visible capacity.
- Queue Add files, Add folder, and destination selection must use the modern
  Windows shell picker (`IFileOpenDialog`) without fixed `OPENFILENAMEW`
  selection buffers, `SHBrowseForFolderW`, `SHGetPathFromIDListW`, or `MAX_PATH`
  assumptions. GUI smoke must exercise picker insertion and a many-file HDROP
  payload before Queue input changes are accepted.
- The System GPU graph and headline value must show total system GPU engine
  utilization. Keep VRAM total and SuperZip dedicated VRAM as detail rows only.
- Do not change the left navigation rail hover/active visuals unless the user
  explicitly requests a navigation redesign. Queue header/body separation,
  select-all tick alignment, column resize cursors, and graph axis label
  overlays are visual regression boundaries.
- GUI archive-format labels must come from `archive_format_info(...).display_name`;
  do not re-create format label arrays in the app layer. Visible format labels
  use official format names only and must not add compatibility/single-file/
  encoded-file/stream/file qualifiers.
- Settings changes are draft-only until `Apply`; leaving Settings without
  `Apply` must restore the last applied snapshot. `Apply` must atomically write
  the validated per-user settings JSON under Local AppData, and GUI smoke must
  redirect to the fixed temp smoke settings file with
  `SUPERZIP_GUI_SMOKE_SETTINGS_REDIRECT=1` and assert both persisted values and
  unapplied-draft reversion. Do not reintroduce path-valued settings
  overrides.
- Settings log retention choices are exactly `1 week`, `2 weeks`, and
  `1 month`, and must be enforced by timestamped pruning logic plus C++ tests;
  label-only changes are not sufficient.
- Elevated drag/drop must keep the narrow UIPI message allowlist for shell drop
  messages and an exception-safe HDROP handler. Do not solve elevated
  drag/drop by adding broad process privileges or bypassing Windows integrity
  boundaries.
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
