# SuperZip Agent Instructions

This file is the operating manual for AI agents and programmers working in this repository. It follows the public AGENTS.md convention and GitHub guidance that agent instructions should cover commands, tests, project structure, code style, git workflow, and boundaries.

References checked on 2026-06-16:

- AGENTS.md format: <https://github.com/agentsmd/agents.md>
- GitHub guidance on effective AGENTS.md files: <https://github.blog/ai-and-ml/github-copilot/how-to-write-a-great-agents-md-lessons-from-over-2500-repositories/>
- GitHub Copilot repository instructions: <https://docs.github.com/copilot/how-tos/agents/copilot-coding-agent/best-practices-for-using-copilot-to-work-on-tasks>
- OpenSSF secure development principles: <https://openssf.org/blog/2023/12/03/openssf-releases-top-10-secure-software-development-guiding-principles/>
- OpenSSF secure software concise guide: <https://github.com/ossf/wg-best-practices-os-developers/blob/main/docs/Concise-Guide-for-Developing-More-Secure-Software.md>
- OWASP Secure Coding Practices Quick Reference Guide: <https://owasp.org/www-project-secure-coding-practices-quick-reference-guide/>
- NIST SSDF SP 800-218 v1.1 and v1.2 draft: <https://csrc.nist.gov/pubs/sp/800/218/final> and <https://csrc.nist.gov/pubs/sp/800/218/r1/ipd>
- CISA Secure by Design guidance: <https://www.cisa.gov/securebydesign> and <https://www.cisa.gov/resources-tools/resources/secure-by-design>
- GitHub Actions secure-use reference: <https://docs.github.com/en/actions/reference/security/secure-use>
- Microsoft Security Development Lifecycle practices: <https://www.microsoft.com/en-us/securityengineering/sdl/practices>
- C++ Core Guidelines: <https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines>
- SEI CERT C++ Coding Standard: <https://cmu-sei.github.io/secure-coding-standards/sei-cert-cpp-coding-standard/>
- Microsoft high-DPI Win32 guidance: <https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows>
- Microsoft Defender command-line guidance: <https://learn.microsoft.com/en-us/defender-endpoint/command-line-arguments-microsoft-defender-antivirus>
- CMake CPack WiX generator scope guidance: <https://cmake.org/cmake/help/latest/cpack_gen/wix.html>
- Microsoft Windows Installer `ProgramFiles64Folder`: <https://learn.microsoft.com/en-us/windows/win32/msi/programfiles64folder>

## Mission

SuperZip is a Windows-native, AMD-only GPU-accelerated archive application written in modern C++20. Preserve the fundamental architecture: HIP is the AMD GPU acceleration boundary, `.suzip` is the native SuperZip archive format, standard `.zip` remains compatibility-only through miniz 3.1.1, `.zipx` remains extract-only compatibility through the ZIP reader for ZIP-compatible records and must fail explicitly for unsupported ZIPX methods, `.tar` remains native compatibility-only through the bounded TAR adapter, `.tar.gz`/`.tgz` remain compatibility-only through the TAR stream adapter over miniz raw deflate, `.tar.bz2`/`.tbz`/`.tbz2` remain compatibility-only through the TAR stream adapter over vendored libbzip2 1.0.8, `.tar.xz`/`.txz` extraction remains compatibility-only through the TAR stream adapter over vendored XZ Embedded, `.tar.lz`/`.tlz` extraction remains compatibility-only through the TAR stream adapter over the lzip stream decoder and vendored LZMA SDK 26.01, `.tar.zst`/`.tzst` remain compatibility-only through the TAR stream adapter over the bundled app-local libzstd 1.5.7 runtime, `.gz` remains single-file compatibility-only through miniz raw deflate, `.bz2` remains single-file compatibility-only through vendored libbzip2 1.0.8, `.xz` extraction remains single-file compatibility-only through vendored XZ Embedded, `.lzma` extraction remains single-file compatibility-only through the vendored LZMA SDK 26.01 decoder, `.lz` extraction remains single-file compatibility-only through the lzip wrapper and vendored LZMA SDK 26.01 decoder, `.zst`/`.zstd` remain single-file compatibility-only through the bundled app-local libzstd 1.5.7 runtime, legacy Unix Compress `.Z` remains native single-file compatibility-only through the bounded LZW adapter, `.b64` remains native single-file compatibility-only through the bounded Base64 adapter, `.hqx` remains native extract-only data-fork compatibility through the bounded BinHex 4.0 adapter with header/data/resource CRC validation, `.macbin` and strongly header-detected MacBinary `.bin` streams remain native extract-only data-fork compatibility through the bounded MacBinary adapter, `.xxe` remains native single-file compatibility-only through the bounded XXEncode adapter, `.uue`/`.uu` remain native single-file compatibility-only through the bounded UUencode adapter, `.cpio` remains native compatibility-only through the bounded CPIO adapter, `.cpio.gz`/`.cpgz` remain native compatibility-only through the Gzip stream adapter over the bounded CPIO adapter, `.ar` remains native compatibility-only through the bounded AR adapter, `.deb` extraction remains native compatibility-only through the bounded AR adapter for outer package members, `.iso` extraction remains native read-only basic ISO 9660 compatibility through the bounded ISO adapter, `.rpm` extraction remains native read-only compatibility through the bounded RPM package adapter over supported CPIO payloads, `.cab` extraction remains native read-only compatibility through the bounded CAB metadata scanner and Windows FDI, `.7z` extraction remains native read-only compatibility through the vendored LZMA SDK 26.01 decoder, `.lha`/`.lzh` extraction remains native read-only compatibility through the vendored Lhasa 0.5.0 decoder, standalone `.wim` extraction remains read-only compatibility through the bundled app-local wimlib 1.14.5 DLL with SuperZip path validation and verified output publication, `.xar` extraction remains native read-only compatibility for the bounded no-TOC-checksum XAR subset through the native XAR adapter over miniz zlib inflate, `.arj` extraction remains native read-only compatibility for stored entries through the bounded ARJ adapter, `.arc`/`.ark` extraction remains native read-only compatibility for SEA ARC unpacked entries through the bounded ARC adapter, and all security-sensitive extraction paths must be validated before writing to disk.

## Non-Negotiable Boundaries

- Do not commit credentials, tokens, personal paths, user profiles, local machine names, build artifacts, crash dumps, `.vs`, or generated binary outputs. The only archive files allowed in source control are pinned upstream provenance artifacts under `third_party/upstream/**` with recorded checksums and licensing notes.
- Workflows in this repository must never create GitHub deployment records. Do not add GitHub Actions `environment:` blocks or `deployment:` keys. Do not bind private scanner credentials directly in workflow YAML; use the documented OIDC broker pattern for Greenbone/OpenVAS live scan configuration.
- Do not persist GitHub tokens in remotes or config. Use short-lived authentication only for a single push.
- Do not use WSL for this project unless a maintainer explicitly asks. The supported development path is Windows-native PowerShell, CMake, MSVC, and optional AMD ROCm/HIP.
- Do not launch the GUI during automated verification unless the task explicitly requires visual testing. Prefer CLI tests and static inspection.
- SuperZip-owned secondary windows, confirmations, prompts, and modal surfaces
  must use the same visual system, typography, colors, spacing, focus behavior,
  and interaction quality as the main window. Do not use raw `MessageBoxW` or
  other platform-default product dialogs for SuperZip confirmations. OS-owned
  brokered surfaces such as common file/folder pickers, UAC elevation prompts,
  Defender prompts, crash dialogs, and installer privilege prompts are the only
  normal exceptions, and any new exception must be documented at the call site.
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
- Before changing GPU/UI performance architecture, GPU utilization claims, HIP
  execution strategy, or any assertion about GPU-accelerated GUI behavior, read
  `docs/gpu-accelerated-ui-and-codec-research.md`. GPU-rendered UI work is not
  archive acceleration evidence; archive acceleration claims require required-HIP
  codec telemetry and RAM-only CPU/GPU comparisons at the same compression level
  and ratio.
- Before changing archive-format recognition or compatibility support, read
  `docs/archive-format-support.md`. Do not add ZIP-container aliases as
  user-facing archive formats unless a maintainer explicitly requests package
  inspection.
- Base64, XXEncode, and UUencode are legacy transfer encodings, not archive
  compression formats. Keep them extract-only in public create/compress
  selectors, CLI help, docs, and tests unless a maintainer explicitly approves a
  separate encoding feature area.
- Before changing the native `.suzip` format, extension handling, or format
  detection, read `docs/native-suzip-format.md`. `.suzip` is a native versioned
  archive format with SUZIP footer/index magic and AMD HIP block semantics, not
  a cosmetic ZIP extension. Version 2 required-HIP archives may use GPU
  static-prefix blocks, and version 3 may use adaptive GPU-prefix blocks; do not
  replace them with CPU-deflate fallback or present GPU CRC/classification work
  as evidence of general GPU entropy compression.
- Before adding product behavior learned from a mature archive tool, read
  `docs/product-behavior-audit.md`. Capture logic only; do not record external
  product names or copy another product's UI/help text into this repository.
- Before performing repo-wide refactoring or automatic cleanup, read
  `docs/refactoring-governance.md` and run `tools\refactor_audit.ps1`.
- Before changing workflow security, CI badges, README logo rendering, verifier
  behavior, or agent/tool routing, read `docs/engineering-learning-loop.md`.
  When a mistake reveals a repeatable failure class, fix the root cause and add
  the narrowest local invariant that prevents recurrence; do not add broad
  process, suppressions, or unrelated gates.
- Before broad bug hunting or native Windows GUI debugging, read
  `docs/debugging-strategy.md`. Reproduce the exact input path first, then
  trace hit testing, focus, state mutation, worker launch, backend result,
  progress/status publication, and repaint behavior before patching.
- GUI destination defaults must resolve the current user's Downloads known
  folder. Do not use the process current directory as a Compress or Extract
  default; elevated launches can otherwise point at system directories.
- The System page GPU graph and headline value represent total system GPU engine
  utilization. VRAM total and process-dedicated VRAM remain detail rows only.
  Do not replace the GPU graph with VRAM percentage or process-only GPU usage.
- Do not alter System graph history length, sampling cadence, or x-step
  behavior unless the maintainer explicitly requests graph-cadence work. Axis
  label clipping fixes must not change graph progression.
- Queue overflow must keep the header fixed, keep row checkbox hit testing
  aligned with the visible row, keep drag/drop accepted only inside the Queue
  table, and expose a scrollbar only when rows overflow the body.
- Queue Add files, Add folder, and destination selection must use the modern
  Windows shell picker (`IFileOpenDialog`) without fixed `OPENFILENAMEW`
  selection buffers, `SHBrowseForFolderW`, `SHGetPathFromIDListW`, or `MAX_PATH`
  assumptions. GUI smoke must exercise both picker-driven queue insertion and a
  many-file HDROP drag/drop payload before Queue input changes are accepted.
- The canonical logo artwork is the `superzip-logo-mark` group in
  `resources/brand/superzip-logo.svg`. AI agents may move or resize that mark
  for layout, and may edit surrounding wordmark/tagline copy, but must not alter
  the mark path geometry, layer count, stroke width, stroke color, line joins,
  line caps, or source-of-truth metadata.
- Before choosing local tests, security scans, GUI smoke, fuzzing, benchmarks,
  or post-push workflow waits, read `docs/targeted-verification.md` and run
  `tools\verification_plan.ps1 -IncludeUntracked`. Use the selected checks
  instead of broad habit-driven gates. If the plan escalates, a targeted check
  fails, changed paths are unknown, or a wider bug is suspected, use the full
  verification profile automatically with `tools\verify_changes.ps1 -Full`.
  Follow `workflowWaitPolicy`: opportunistic checks are acceptable during
  multi-commit iteration only when deferral is allowed, but final handoff,
  release work, workflow changes, verifier changes, MCP changes, skill changes,
  and full-escalation changes must complete the relevant final workflow wait.
  Long-running fuzzing is observed but not normally waited for; check it
  opportunistically during iteration and include it only with final/release
  workflow waits.
- Do not copy code, UI, or designs from reference repositories. Only use public projects for high-level comparison.

## Engineering Quality Baseline

These rules were checked against official guidance on 2026-06-20: NIST SSDF,
CISA Secure by Design, OWASP Secure Coding Practices, Microsoft SDL, GitHub
Actions secure-use guidance, OpenSSF Scorecard, SLSA v1.2, the C++ Core
Guidelines, and SEI CERT C++.

- Keep changes small, reviewable, and behavior-preserving unless the task
  explicitly asks for a behavior change.
- Design secure-by-default paths: fail closed, make risky behavior opt-in, and
  keep trust boundaries visible in code comments and tests.
- Use least privilege everywhere: workflow permissions, filesystem writes,
  process creation, GPU/device access, and installer actions.
- Use modern C++20 deliberately: RAII ownership, value semantics, explicit
  lifetimes, `std::span`/views for bounded buffers, `std::filesystem` for paths,
  typed sizes and counts, and no hidden ownership through raw pointers unless
  interop requires it and the contract comment says so.
- Keep the whole codebase resource-bounded by design. Every archive path, GPU
  path, GUI worker, benchmark, and installer action must have clear CPU, RAM,
  VRAM, disk-write, handle, and timeout limits that match the product contract.
- Make performance claims evidence-based. Any speed or ratio claim must name
  the workload shape, input bytes, output bytes, compression level, block size,
  CPU/GPU mode, and whether the run was RAM-only.
- Treat refactoring and modernization as behavior-preserving engineering work,
  not cosmetic churn. Split large functions, remove real duplication, clarify
  ownership, and improve testability only with matching verification evidence.
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
- `src/arc/`: Read-only SEA ARC/ARK compatibility adapter for unpacked regular-file entries with CRC-16/ARC validation and verified file publication.
- `src/arj/`: Read-only ARJ compatibility adapter for stored regular-file and directory entries with header/payload CRC validation and verified file publication.
- `src/base64/`: Base64 single-file text encoding adapter with strict padding validation, optional wrapper headers, and verified file publication.
- `src/bzip2/`: Bzip2 compatibility streams and `.bz2` single-file adapter using vendored libbzip2 1.0.8.
- `src/cab/`: Read-only CAB adapter with native metadata validation and Windows FDI streaming decompression.
- `src/core/`: archive format, manifest, path safety, progress, Defender opt-in scan, SHA-256 integrity.
- `src/cpio/`: CPIO and CPIO.GZ compatibility adapter for SVR4 new ASCII archives with two-pass path validation and verified file publication.
- `src/gzip/`: Gzip compatibility streams using miniz raw deflate with CRC32/ISIZE verification.
- `src/gpu/`: AMD HIP codec integration and CPU fallback used only when GPU is not required.
- `src/hqx/`: Extract-only BinHex 4.0 `.hqx` data-fork adapter with strict HQX alphabet parsing, RLE expansion, header/data/resource CRC validation, path-safe header names, and verified file publication.
- `src/iso/`: Read-only basic ISO 9660 compatibility adapter with two-pass path validation and verified file publication.
- `src/lha/`: Read-only LHA/LZH adapter using the vendored Lhasa 0.5.0 decoder with two-pass validation and verified file publication.
- `src/lzip/`: Read-only lzip `.lz` stream adapter and TAR.LZ filter using the vendored LZMA SDK 26.01 decoder with CRC32/data-size/member-size validation and verified file publication.
- `src/lzma/`: Read-only LZMA-Alone `.lzma` single-file adapter using the vendored LZMA SDK 26.01 decoder with bounded allocation and verified file publication.
- `src/macbinary/`: Extract-only MacBinary data-fork adapter with path-safe ASCII header names, data/resource extent validation, MacBinary II/III header CRC validation, and verified file publication.
- `src/rpm/`: Read-only RPM package adapter that validates RPM headers, decodes supported CPIO payload compression, and delegates extracted package paths to the CPIO adapter.
- `src/sevenzip/`: Read-only 7z adapter using the vendored LZMA SDK 26.01 decoder with two-pass validation and verified file publication.
- `src/tar/`: TAR, TAR.GZ, TAR.BZ2, TAR.XZ, TAR.LZ, and TAR.ZST compatibility adapter with two-pass path validation and verified file publication.
- `src/unix_compress/`: Unix Compress `.Z` single-file compatibility adapter with bounded LZW dictionaries and verified file publication.
- `src/uue/`: UUencode `.uue`/`.uu` single-file compatibility adapter with strict begin-line parsing, bounded lines, path-safe header names, and verified file publication.
- `src/wim/`: Read-only standalone WIM adapter using the bundled app-local wimlib 1.14.5 runtime with staging, path validation, unsupported-entry rejection, and verified file publication.
- `src/xar/`: Read-only XAR subset adapter for no-TOC-checksum archives with zlib-compressed TOCs, stored/zlib regular-file payloads, and verified file publication.
- `src/xxe/`: XXEncode `.xxe` single-file compatibility adapter with strict alphabet parsing, bounded lines, path-safe header names, and verified file publication.
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
- `third_party/lhasa/`: production Lhasa 0.5.0 copy with SuperZip-local hardening patches documented in `README.SUPERZIP.md`.
- `third_party/upstream/lhasa/0.5.0/`: unmodified upstream Lhasa 0.5.0 release archive and checksum for provenance.
- `third_party/wimlib/`: wimlib 1.14.5 header and LGPL/libdivsufsort-lite notices. Do not track extracted runtime DLLs here.
- `third_party/upstream/wimlib/1.14.5/`: upstream wimlib Windows x64 runtime package and checksums for provenance.
- `.github/workflows/`: CI and opt-in security integrations.
- `.clusterfuzzlite/`: ClusterFuzzLite build integration for C++ sanitizer fuzzing.
- `.github/codeql/`: CodeQL scanning configuration.
- `.github/requirements/`: hash-locked Python scanner and linter requirements for GitHub-hosted jobs.
- `.github/requirements/requirements-lint-windows.txt`: hash-locked Python linter requirements for the Windows lint workflow.
- `.github/workflows/lint.yml`: language lint workflow for C/C++, PowerShell, Python helper scripts, YAML, Markdown, and CMake.
- `.github/workflows/release.yml`: manual x64 release, installer smoke, beta/stable track selection, and publishing workflow.
- `.github/workflows/scorecard.yml`: default-branch OSSF Scorecard workflow.
- `.agents/skills/` and `mcp/`: local helper skills/MCPs for future agents.
- `docs/engineering-learning-loop.md`: concrete mistake-to-invariant policy for
  preventing repeated workflow, badge, branding, verifier, and agent-routing
  failures without degrading product direction.
- `docs/debugging-strategy.md`: sanitized native Windows C++ debugging workflow
  for GUI, worker, telemetry, installer, and archive-operation defects.

## Build And Test Commands

Use these from the repository root. Start every change by asking the verifier
what is relevant:

```powershell
tools\verification_plan.ps1 -IncludeUntracked
tools\verify_changes.ps1 -IncludeUntracked
```

If local lint tooling is missing, bootstrap the repo-local pinned toolchain
before rerunning verification. Use `-PythonPath` or `SUPERZIP_PYTHON` only to
select the host Python executable; the linters themselves must come from the
hash-locked requirements file and the pinned PSScriptAnalyzer package:

```powershell
tools\bootstrap_lint_env.ps1 -PythonPath <path-to-python.exe>
tools\lint.ps1 -CppMode Changed -IncludeUntracked
```

If a larger bug is suspected, changed paths are broad or unknown, verification
tooling changed, or a targeted check fails, the system must run the full local
profile:

```powershell
tools\verify_changes.ps1 -IncludeUntracked -Full
```

The normal local build remains HIP-enabled and requires `HIP_PATH` when the plan
selects a product build:

```powershell
tools\build.ps1 -Configuration Release
tools\test.ps1 -Configuration Release
tools\lint.ps1 -CppMode Changed
tools\compatibility_interop_smoke.ps1 -Configuration Release
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
tools\bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1 -BlockSizeKiB 256,512,1024,2048,4096,8192,16384
build\Release\superzip_cli.exe benchmark-suite --profile Mixed --compression-level 5 --tune
```

`tools\bench.ps1` is memory-only by default and must report
`memory_only=true` and `disk_write_bytes=0` for every CPU and GPU lane. During
development, CPU/GPU benchmarking must stay RAM-only to avoid unnecessary SSD
wear. Do not run multi-GB generated filesystem benchmarks. The only normal
storage validation is `tools\storage_smoke.ps1` or `tools\bench.ps1 -Mode
Filesystem` with a bounded smoke payload of at most 64 MiB.
The Mixed profile must continue to include a low-entropy non-pattern region so
required-HIP compression regressions are visible without writing large
workloads to storage.

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
- New or changed functions must pass
  `tools\refactor_audit.ps1 -ChangedOnly -CheckContracts -MaxFunctionLines 120 -MaxComplexityMarkers 35 -FailOnFindings`.
  Split large functions before pushing; do not wait for CodeQL to report
  poorly documented or oversized functions.

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
- Non-SUZIP create formats must produce standards-compatible files. When
  changing ZIP, TAR, compressed TAR, CPIO, CPIO.GZ, AR, or shared compatibility
  stream writers, run `tools\compatibility_interop_smoke.ps1 -Configuration
  Release` so independent Windows readers prove the outputs are not only
  readable by SuperZip.
- CPIO compatibility is limited to regular files and directories in the SVR4
  new ASCII variants. Keep links, hard-link metadata, devices, FIFOs, and
  special files rejected unless a maintainer approves a dedicated security and
  UI policy.
- CPIO.GZ compatibility is a Gzip-filtered CPIO stream, not a generic
  multi-format Gzip fallback. Keep both decompression passes in process, validate
  the inner CPIO entry set before output, reject malformed Gzip trailers, and do
  not stage multi-GB decoded CPIO streams on disk.
- AR compatibility is limited to regular-file members. Keep symbol-table and
  string-table metadata non-extracting, preserve nested paths through supported
  long-name variants, and validate all member names before output.
- ARJ compatibility is extract-only and currently limited to stored regular
  files and directories. Keep encrypted entries, multi-volume entries,
  compressed methods, labels, chapters, and SFX-prefixed scanning rejected until
  a dedicated decoder/security increment implements them with tests. Validate
  basic and extended header CRCs plus stored payload CRCs before destination
  writes, and publish output only through verified temporary paths.
- SEA ARC/ARK compatibility is extract-only and currently limited to unpacked
  method-1 and method-2 regular-file entries. Keep compressed methods,
  unrelated `.arc` formats, SFX-prefixed scanning, unsafe paths, duplicate
  paths, trailing bytes after the end marker, and corrupt CRC-16 payloads
  rejected until a dedicated decoder/security increment implements them with
  tests. Publish output only through verified temporary paths.
- DEB compatibility is extraction-only and uses the native AR outer-container
  path. Do not install packages, execute maintainer scripts, or silently unpack
  nested package payloads without a dedicated product requirement and tests.
- RPM compatibility is extraction-only and uses the native RPM header scanner
  plus the native CPIO adapter for package payloads. Do not install packages,
  execute scripts, trust package names as extraction paths, or support payload
  formats other than CPIO without a dedicated requirement and tests. Keep
  compressed and decoded temporary payload spooling explicitly capped.
- CAB compatibility is extraction-only and uses the native CAB metadata scanner
  plus Windows FDI for streaming decompression. Reject spanned cabinets, validate
  names and sizes before FDI output is accepted, and publish only through the
  verified temporary-file path.
- 7z compatibility is extraction-only and uses the vendored LZMA SDK 26.01
  decoder in process. Keep SDK allocations bounded, reject unsafe paths and
  unsupported special-file attributes before output, validate payload CRC/size
  before publishing, and do not advertise 7z creation until a vetted
  in-process writer path is deliberately added with tests.
- LHA/LZH compatibility is extraction-only and uses the vendored Lhasa 0.5.0
  decoder in process. Do not call Lhasa's archive helper, do not shell out to
  host `lha` tools, reject symbolic links and unsafe paths before output, run a
  full CRC/size validation pass before destination writes, and keep the
  upstream release archive unmodified under `third_party/upstream/lhasa/0.5.0/`.
- XAR compatibility is extraction-only and uses the native bounded parser in
  `src/xar/`. Keep it limited to archives with no TOC checksum mode until exact
  checksum verification is implemented; reject signatures, links, hard links,
  special files, unsupported heap encodings, unsafe paths, and corrupt payloads
  before destination writes.
- Unix Compress `.Z` compatibility is single-file only. Keep the LZW dictionary
  bounded by the stream-declared maxbits, validate magic/header flags, and
  publish extraction output only through the verified temporary-file path.
- UUE compatibility is single-file for `.uue`/`.uu`. Keep UUencode parsing in
  process, bound preamble and line lengths, validate the begin-line filename
  through SuperZip path safety before creating output, reject malformed payload
  lines and trailing data, and publish only through the verified temporary-file
  path.
- XXEncode compatibility is single-file for common `.xxe` begin/payload/end
  streams. Keep XXEncode parsing in process, use only the
  `+-0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz` alphabet
  plus the documented zero-line terminator variants, bound preamble and line
  lengths, validate the begin-line filename through SuperZip path safety before
  creating output, reject malformed payload lines and trailing data, and publish
  only through the verified temporary-file path. Historical post-end byte-count
  and CRC trailer variants remain rejected until their exact syntax and checksum
  contract are implemented with tests.
- Base64 compatibility is single-file for `.b64`. Keep RFC-style Base64 parsing
  in process, bound preamble and line lengths, validate optional
  `begin-base64` wrapper filenames before creating output, reject malformed
  padding and trailing data, and publish only through the verified
  temporary-file path. Do not advertise full MIME multipart parsing until a
  dedicated parser and tests exist.
- BinHex compatibility is extract-only for `.hqx`. Keep BinHex parsing in
  process, require the standard BinHex 4.0 transfer marker, decode only the HQX
  alphabet plus whitespace, expand `0x90` RLE as a bounded stream, validate the
  header filename before output reservation, verify header/data/resource CRCs,
  publish only the data fork through the verified temporary-file path, and do
  not publish resource-fork metadata or advertise BinHex creation until a
  deliberate policy and tests exist.
- MacBinary compatibility is extract-only and data-fork-only for `.macbin` and
  strongly header-detected `.bin`. Keep generic `.bin` extension detection
  gated by MacBinary II/III version/signature markers plus header CRC, validate
  path-safe ASCII header names and all fork extents before output publication,
  reject truncated data/resource/comment regions, publish only the data fork
  through the verified temporary-file path, and do not advertise MacBinary
  creation or resource-fork restoration until a deliberate Windows policy and
  tests exist.
- Bzip2 compatibility is single-file for `.bz2` and TAR-stream-only for
  `.tar.bz2`/`.tbz`/`.tbz2`. Keep libbzip2 in-process, reject trailing data in
  single-member streams unless concatenated-member support is deliberately added
  with tests, and publish extraction output only through verified temporary
  paths.
- XZ compatibility is extract-only for `.xz` and TAR-stream-only for
  `.tar.xz`/`.txz`. Keep XZ Embedded in-process, keep the decoder memory cap,
  reject unsupported filters/checks instead of skipping verification, and do not
  advertise XZ creation until an encoder path is deliberately added with tests.
- Lzip compatibility is extract-only for `.lz` and TAR-stream-only for
  `.tar.lz`/`.tlz`. Keep the lzip wrapper decoder in process over the vendored
  LZMA SDK 26.01 decoder, validate version `1`, dictionary-size codes, EOS
  markers, CRC32, data size, and member size for every concatenated member,
  reject trailing non-member data and empty members before another member, and
  publish single-file output only through verified temporary paths.
- Zstandard compatibility is single-file for `.zst`/`.zstd` and TAR-stream-only
  for `.tar.zst`/`.tzst`. Keep libzstd in-process through the app-local DLL
  loader, keep the decoder window cap, validate runtime version `10507`, keep
  SuperZip-created frame checksums enabled, and reject malformed streams or
  trailing garbage before publishing output.
- WIM compatibility is extract-only for standalone `.wim` archives through the
  bundled app-local wimlib 1.14.5 DLL. Do not call `wimlib-imagex.exe`, DISM,
  PowerShell, Explorer, or host WIM tools. Keep split `.swm` sets rejected until
  multipart reference handling has explicit tests. Validate every image path,
  reject reparse points, hard links, alternate data streams, device entries,
  encrypted/offline/virtual files, and missing resources before staging, then
  publish only rechecked regular files through the verified temporary-file path.
- Reject absolute paths, drive-rooted paths, UNC paths, traversal, reserved Windows device names, unsafe trailing dot/space, and invalid characters.
- Default to no overwrite. Overwrite is an explicit option.
- Verify block lengths, offsets, CRC32, and archive footer/index consistency before trusting payload metadata.
- Microsoft Defender scanning is opt-in and must run with `CREATE_NO_WINDOW`.
- SHA-256 integrity hashing is opt-in and must use Windows CNG on Windows.
- Keep CI layered: build, tests, secret scan, dependency review/security scanning, an always-on Greenbone/OpenVAS integration audit, and an OIDC-brokered authorized live OpenVAS/Vulnetix lane.
- Keep language linting active for the languages actually used by SuperZip:
  C/C++ formatting, PowerShell static analysis, Python helper lint/format,
  YAML workflow lint, Markdown lint, and CMake lint. Do not add badge-only
  workflows for languages or services that are not part of the repo.
- Workflow `run` blocks must pass GitHub context values through quoted
  environment variables instead of direct `${{ github.* }}` interpolation.
  CI tool installs must be version-pinned and hash-verified when the package
  manager bootstrap is known to be mutable or runner-dependent.
- CodeQL C++ must use a manual Windows 2022 build database through
  `tools/build.ps1 -Configuration Release -CpuOnlyValidation`; build-free C/C++
  analysis produces unacceptable parser-artifact alerts for this Win32/HIP
  codebase and must not be restored for speed.
- Do not run build/package jobs in parallel with GUI smoke tests or a manually
  opened build-output `SuperZip.exe`. The build script fails early when the
  target GUI binary is running so agents do not misdiagnose linker file-lock
  errors as code defects.
- Keep ClusterFuzzLite fuzzing active for archive metadata and path-handling code. Fuzz targets must exercise real product parser code and must not be placeholder functions added only to satisfy scanner heuristics.
- Product release artifacts must be HIP-enabled. Do not publish CPU-only portable ZIPs or MSIs as SuperZip releases.
- Product benchmark claims must sweep the production SUZIP block-size choices:
  256 KiB, 512 KiB, 1 MiB, 2 MiB, 4 MiB, 8 MiB, and 16 MiB. The benchmark harness must compare
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
- SuperZip-owned installer launch, release, and smoke-test paths must use
  bounded waits. MSI install and uninstall phases default to a 300-second
  timeout, and HIP SDK installer setup in hosted release validation must time
  out explicitly instead of waiting indefinitely. The Windows UAC consent prompt
  is OS-owned and cannot be timed from inside the MSI; do not claim otherwise.
- Installer UI must offer an explicit `Create Desktop shortcut` choice. Keep
  the option visible to the user instead of silently creating or suppressing the
  shortcut.
- Do not redistribute AMD's HIP runtime DLL from a developer SDK. AMD documents that runtime as supplied by the AMD GPU driver; SuperZip delay-loads it and reports missing prerequisites through `dependency-check`.
- Keep event-specific checks in event-specific workflows. Do not add jobs that are expected to show as skipped in normal push CI.
- Before editing scanner workflows, read `docs/security-code-scanning.md`, verify action versions from official tags/releases, and pin actions by full commit SHA.
- Do not add GitHub Actions `environment:` blocks or `deployment:` keys. This repository uses repository variables plus an external OIDC broker instead of Actions environments so workflows cannot create deployment records.
- Never commit Greenbone targets, credentials, Vulnetix organization IDs, scan credentials, or host-specific network details. Keep live scanner secrets outside GitHub Actions and return them only from the OIDC broker after claim validation.
- Preserve patched/original dependency splits: production changes go under the
  active vendored source directory, and upstream archives under
  `third_party/upstream/**` stay unmodified.

## GUI Rules

- Maintain `PerMonitorV2` DPI behavior in both manifest and startup code.
- All layout geometry must pass through DPI scaling or a DPI-aware layout helper.
- Keep the release GUI fixed-size and non-resizable until resizing is
  deliberately re-enabled. Do not ship a resizable window until every page has a
  responsive layout pass, high-DPI screenshot coverage, and click-hit regression
  coverage at small, default, 4K, and high-refresh display settings.
- Rendering must be double-buffered or otherwise flicker-free. The Win32 class
  background and `WM_ERASEBKGND` path must use the app's dark background so the
  first visible frame never flashes white before custom painting.
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
- `resources/brand/superzip-logo.svg` is the single source of truth for the
  SuperZip logo. Regenerate `resources/app/superzip.ico` with
  `tools\generate_app_icon.ps1`, let CMake generate the Win32 logo geometry
  header, and keep `tools\verify_brand_assets.ps1` passing. Do not hand-redraw
  divergent logo marks in code or docs.
- Visible controls must share drawing and hit-test geometry. After drag/drop,
  Queue, Compress, Extract, Security, History, System, Settings, and About page
  controls must remain clickable; fix the shared geometry instead of adding
  page-specific click offsets that can drift from rendering.
- Keep the left navigation rail's existing hover/active behavior unless a
  maintainer explicitly requests a rail redesign. Do not apply global clickable
  hover or keyboard-focus visual changes to the rail as a side effect of
  changing page content controls.
- Queue table header rows must remain visually distinct from body rows through
  a restrained header band and separator line. Keep the select-all tick aligned
  with body ticks, keep the first column narrow, and keep Name/Size/Type/Path
  column resizing hover cursors and minimum readable header widths intact.
- Elevated Windows drag/drop is a supported shell workflow. Keep `WM_DROPFILES`
  and the drag-query message narrowly allowed through UIPI for elevated
  SuperZip windows, keep the HDROP handler exception-safe, and do not replace
  this with a broad UIAccess manifest or unrelated privilege workaround.
- Never add or redesign a GUI feature by breaking an existing interaction.
  File-picker queueing, folder-picker queueing, drag/drop queueing, row
  selection, destination selection, dropdowns, toggles, tab changes, and primary
  actions are regression boundaries and must keep working unless a maintainer
  explicitly removes that capability.
- User-facing archive-format labels must come from
  `archive_format_info(...).display_name`. Do not duplicate GUI archive-format
  label arrays, and do not add visible format labels with invented qualifiers
  such as compatibility, single-file, encoded-file, stream, or file when the
  selected input may be a folder.
- The compression level dropdown shows names only: `Fastest`, `Fast`,
  `Balanced`, `Strong`, and `Maximum`. Do not append numeric parentheticals to
  GUI labels, and do not reintroduce plural compression-setting wording in GUI
  source.
- Dropdown arrows must be vertically centered in their value boxes and use one
  consistent shape and inset throughout the app.
- Live graph axis labels must render as plain top-layer text over the graph;
  do not put min/max labels in opaque boxes, cards, badges, or other
  backgrounds.
- Toggle rows must rely on the switch state itself. Do not add redundant
  `Enabled` or `Disabled` text labels next to settings toggles.
- The Settings page uses a draft/apply model. Changing a Settings control
  affects the current draft only; leaving Settings without `Apply` must restore
  the last applied snapshot. `Apply` must atomically persist the validated
  per-user configuration to `%LOCALAPPDATA%\SuperZip\settings.json`; GUI smoke
  must redirect to the fixed temp smoke settings file with
  `SUPERZIP_GUI_SMOKE_SETTINGS_REDIRECT=1`, assert the persisted values, and
  verify that an unapplied draft is not persisted. Do not reintroduce
  path-valued settings overrides.
- Settings log retention options are exactly `1 week`, `2 weeks`, and
  `1 month`. They must be backed by timestamped pruning so expired in-memory
  log rows and over-capacity rows are actually removed; keep the focused C++
  retention tests and GUI smoke label checks passing.
- Check all pages, not only the main queue page, when making visual changes.
- Treat `resources/design/superzip-ui-iteration-4.png` as the current visual
  acceptance reference. Do not simplify the compact enterprise shell, command
  bar, icon rail, page-specific panels, bottom GPU status strip, or explicit
  settings/security pages.
- After any GUI rendering or interaction change, run `tools/gui_smoke.ps1
  -Configuration Release`. The smoke must open every tab, click every visible
  button, toggle, dropdown/select field, and main action path at least once,
  exercise Add files, Add folder, Clear, drag/drop, destination selection,
  Queue/Compress/Extract/Security/System/History/Settings actions, and close
  without leaving modal dialogs or windows open. The smoke must assert that
  queued items are visibly rendered after file picker, folder picker, and native
  drag/drop injection; clicking a control without verifying its effect is not
  enough. Inspect the generated screenshots for every page before calling the UI
  done.

## Git Workflow

- Inspect `git status --short` before staging.
- Run `tools\verification_plan.ps1 -IncludeUntracked` before deciding which
  checks to run. Run `tools\verify_changes.ps1 -IncludeUntracked` for the
  selected local checks, and use `-Full` when the classifier escalates or a
  broader regression is suspected.
- Stage only intentional source/docs/config changes.
- Never use destructive commands such as `git reset --hard` or `git checkout --` unless a maintainer explicitly requests them.
- Before pushing, run the checks selected by the verification plan and the
  changed-file hygiene gate. Do not run unrelated heavyweight checks unless the
  selector escalates or there is evidence of a broader problem.
- After pushing, verify the remote URL does not contain credentials.
- After pushing, follow the current plan's `workflowWaitPolicy`. Use
  `tools\wait_relevant_workflows.ps1 -Commit <sha> -Mode opportunistic` only
  during intermediate commits when the plan allows deferral. Use `-Mode final`
  before final handoff or release, and always use final mode for workflow,
  verifier, MCP, skill, or full-escalation changes. If the verifier requires a
  post-push audit, the final waiter runs `tools\github_post_push_audit.ps1`.
  The waiter must resolve short refs to full commit SHAs and pass its GitHub
  CLI preflight before polling. If `gh` is unauthenticated, unauthorized, or
  returns an error, stop and fix authentication instead of treating every
  workflow as missing.
  Fuzzing is a long-running observed workflow: do not block on it during normal
  iteration, but sample it with `-IncludeLongRunning` and wait for it with
  `-FinalCommit` when the current commit is the final handoff or release.
- Release changes must keep the package x64-only, attach SHA-256 checksum files,
  and run install/uninstall smoke tests before publishing.
- Do not replace an existing GitHub release or tag by default. Release
  replacement is exceptional and is allowed only when the current maintainer or
  user request explicitly asks for that specific replacement. A replacement run
  must use an explicit `release_version`, `replace_existing=true`, and
  `replacement_acknowledgement` set exactly to `replace <version>`, for example
  `replace 0.1.0`. Normal release runs use a new SemVer version with
  `replace_existing=false`, and release notes must list the actual fixes,
  created assets, validation work, and known limitations.

## Agent Workflow

1. Read this file, `README.md`, `IMPLEMENTATION_PLAN.md`, and relevant local code before editing.
2. Make the smallest change that satisfies the request while preserving the architecture.
3. Add or update tests for behavior, security boundaries, and regressions.
4. Run `tools\verification_plan.ps1 -IncludeUntracked`, then run the selected
   targeted checks with `tools\verify_changes.ps1 -IncludeUntracked`.
5. Escalate automatically to `tools\verify_changes.ps1 -IncludeUntracked -Full`
   when the plan requires it, a targeted check fails, or a wider bug is
   suspected.
6. During multi-commit work, sample relevant workflows opportunistically only
   when the plan allows it, keep implementing while runs are active, and do not
   report completion until the final relevant workflow wait has passed.
7. Report what changed, what was verified, which workflows were waited for, and
   any remaining risk.
