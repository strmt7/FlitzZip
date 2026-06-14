# SuperZip Enterprise Implementation Plan

## Current Direction

SuperZip is a native Windows desktop application written in modern C++ with an
AMD-only GPU acceleration boundary. The original plan's `hipcomp-core` boundary
is preserved as the hardware abstraction contract, but the implementation is
native C++ instead of Python/PyQt because HIP/ROCm acceleration and Windows
packaging are best served by a direct native runtime.

The application delivers two explicit archive modes:

- `SuperZip GPU (.szip)`: AMD HIP accelerated archive mode using chunked,
  bounded-memory compression and decompression.
- `ZIP compatibility (.zip)`: standards-oriented ZIP read/write path for
  interoperability, kept separate from GPU-native mode so the UI never implies
  unsupported GPU DEFLATE behavior.

The installed ROCm tree on the target host exposes HIP 7.1 and an AMD Radeon RX
9070 XT (`gfx1201`). The build remains configurable for other AMD GFX targets.

## Product Requirements

- Native C++20/C++23 codebase.
- Native Windows GUI with a simple, modern, enterprise interface.
- AMD GPU acceleration only. No CUDA, no vendor-neutral WebGPU path, and no
  hidden GPU fallback to non-AMD devices.
- Clear AMD GPU diagnostics page showing ROCm/HIP status, device name, target
  architecture, and acceleration availability.
- Main queue page, compress page, extract page, security review page,
  preferences page, history page, and diagnostics page.
- Responsive UI: all archive work runs on worker threads, progress is sampled
  atomically, cancellation is cooperative, and the message pump is never blocked.
- Secure extraction: reject absolute paths, drive-rooted paths, UNC paths,
  traversal (`..`), unsafe names, malformed metadata, overflowed sizes, and
  archive entries that would resolve outside the destination root.
- No personal secrets in code, docs, logs, commits, CI, or screenshots.

## Architecture

```text
src/
  app/        Native Win32 GUI, windows, layout, history, settings
  cli/        Command-line entry point for tests and automation
  core/       Archive model, path safety, streaming I/O, progress contracts
  gpu/        AMD HIP device context and SuperZip GPU codec
  zip/        Standard ZIP compatibility adapter
resources/   UI assets and generated design references
tests/       C++ test binaries and PowerShell integration tests
tools/       Build, benchmark, security, and release automation
.agents/     Repo-local skills for future agents
mcp/         Local MCP helper for safe SuperZip build/test operations
```

The boundary is:

```text
GUI/CLI -> Controller/JobRunner -> ArchiveService -> HardwareEngine -> HIP/hipcomp-core boundary
```

`HardwareEngine` is the only layer allowed to call HIP, ROCm, or future
`hipcomp-core` APIs. Higher layers operate on typed progress snapshots,
settings objects, and archive plans.

## Iterative Execution Log

### Iteration 1: Plan Correction

- Replaced the Python/PyQt shell with native C++/Win32.
- Kept the AMD HIP/hipcomp-core abstraction as the fundamental hardware method.
- Added explicit product pages beyond the main screen.
- Added secure extraction and no-secret handling as core requirements.
- Added benchmark comparison requirements against `bea4dev/cozip` without
  copying code, UI, shaders, or implementation details.

### Iteration 2: Performance Contract

- Added chunked streaming to avoid whole-archive memory spikes.
- Added worker-thread job execution for GUI responsiveness.
- Added atomic progress snapshots and throughput reporting.
- Added benchmark harness requirements for compress, extract, verify, and UI
  startup latency.

### Iteration 3: Security Contract

- Added path traversal and malformed archive tests.
- Added static no-secret scans.
- Added CI gates inspired by the user's reference repository style: tests, lint-like
  checks, security scans, and documentation validation.

### Iteration 4: UI Surface Contract

- Expanded the UI scope to all core pages:
  - Main queue
  - Compress settings
  - Extract settings
  - Security review
  - Operation history
  - AMD GPU diagnostics
  - Preferences/About
- Generated design references with the built-in image generation tool and will
  store selected references under `resources/design/`.

### Iteration 5: Responsiveness, DPI, And Opt-In Security

- Added PerMonitorV2 DPI manifest support and startup DPI awareness.
- Rebuilt GUI layout around DPI-scaled integer geometry and native ClearType
  fonts.
- Added double-buffered painting and coalesced repaint requests for high-refresh
  displays.
- Wired Preferences/Security toggles into real job behavior for AMD GPU
  requirement, overwrite, SHA-256 integrity hashing, and Microsoft Defender
  scanning.
- Replaced ZIP compatibility whole-file buffering with miniz file streaming APIs
  to reduce SuperZip RAM usage on large archives.

### Iteration 6: CI And Bounded-Memory Extraction

- Fixed GitHub Actions to run on a Visual Studio 2022 hosted image and emit
  toolchain diagnostics.
- Added repository `.gitattributes` for stable line endings and binary assets.
- Changed `.szip` verification and extraction to decode bounded block windows
  instead of allocating whole archive entries.
- Added test coverage that forces multi-window extraction.

## Validation Gates

No iteration is complete until these gates pass:

1. `tools/build.ps1 -Configuration Release`
2. `tools/test.ps1`
3. `tools/security_scan.ps1`
4. `tools/bench.ps1`
5. Manual GUI smoke test for startup, navigation, compress, extract, cancel,
   error state, and GPU diagnostics.

## GitHub Publication

The final repository is pushed to `strmt7/SuperZip`. The provided PAT is a
runtime credential only. It must never be written to disk except through Git's
credential flow, never echoed, never committed, and never included in logs.
