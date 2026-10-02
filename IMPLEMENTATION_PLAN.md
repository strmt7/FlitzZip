# SuperZip Enterprise Implementation Plan

## Current Modernization Sequence

The modernization work proceeds serially. Imported scanner alerts are being
triaged in batches; a comprehensive final security audit remains a separate
gate. A local pass is not evidence of a clean remote Security tab or a published
release.

The [2026-10-01 archive engineering review](docs/archive-engineering-review.md)
compares selected practices from six established open-source projects and orders
the remaining work by security risk, aggregate resource accounting, measured CPU
and HIP costs, product parity, and final publication. It records research and
acceptance criteria, not completed improvements or a clean security assessment.

The October 2 numeric-field and MCP containment batch passes all 30 selected
full-profile local commands: 557 native tests, 24 MCP tests, the 36-format matrix,
independent interoperability, bounded sanitizer smoke and unpublished package
validation. All eight GUI pages were inspected after the smoke process closed.
CPIO uses checked, fixed-width integer conversion; TAR reuses its bounded octal
writer. Independent wire-format oracles cover uint32 boundaries, 65,536 CPIO
values and the TAR checksum. MCP launches remain suspended until aggregate
committed-memory containment is verified; actual parent/descendant allocation
tests and launch-failure regressions cover the new boundary. These are correctness
and hardening results, not new speed or compression-ratio measurements.

Hosted checks for commit `e79cee0` passed Windows CI, lint, security, fuzzing,
Scorecard, scanner-configuration validation and the explicitly dispatched graph
validation. The October 2 post-push snapshot has 210 open alerts, down from 216:
all six owned numeric-formatting alerts closed automatically. The remaining
alerts comprise 183 in pinned Zstandard source, six SDK fixed-size-copy warnings,
18 verified provenance-hash matches, one CMake compiler-probe warning and two
Scorecard residuals. Dependabot and open pull-request counts are both zero in
that snapshot. These counts describe GitHub alerts, not confirmed vulnerabilities.

Static triage retains per-alert counterevidence for four dictionary-job lifetime
alerts, six pointer-bound alerts and 20 packed-table pointer-stride alerts. Their
GitHub state is unchanged; static counterevidence is not a blanket vendor safety
claim or permission to suppress rules. Remaining scanner triage, CPU/GPU
workspace accounting, mixed-profile extraction diagnosis, final comparisons and
charts, end-user documentation and release acceptance remain open.

The subsequent owned-decoder instrumentation batch passes all 28 full-profile
local commands, including 560 native tests, the format matrix, independent
interoperability, bounded sanitizer smoke and unpublished package validation.
All eight GUI pages were inspected. Allocation, materialization and host-CRC
worker intervals now share production CPU/GPU instrumentation; overflowing
worker totals remain unavailable rather than wrapping. Six 10 GiB RAM diagnostic
lanes at effort 5 and 8 MiB blocks completed with unchanged archive sizes.
Materialization is the largest measured GPU owned-decode worker interval on the
Mixed and Incompressible profiles. These overlapping intervals are not wall-time
fractions or a new speedup claim. One run overlapped a short lint invocation;
none of these single-repeat diagnostics is final publication evidence. Hosted
validation for this subsequent batch remains pending until its push completes.

1. Refresh dependency graphs and pinned provenance. Current local checks cover
   updated Python locks, LZMA SDK 26.03, miniz 3.1.2, Lhasa 0.6.0, native build,
   tests, 36-format routing, and independent format interoperability. Hosted
   scanner runtime checks and Dependabot closure still require a verified push.
2. Review product ownership, compression effort, CPU/HIP work allocation, GUI
   responsiveness, and format contracts. Preserve measured improvements;
   distinguish uncompressed containers and extract-only formats from codecs.
   October diagnostics found premature GPU histogram saturation at the default
   block size and non-monotone native CPU codec presets. The GPU encoder now
   uses distinct exact sample budgets and incremental tile-stratified counting,
   with phase scrambling to avoid the reproduced periodic-header sampling alias.
   Native CPU encoding retains complete lower-effort winners and reuses worker
   trial storage. These are production paths with independent size/read-back
   regressions, not benchmark alternatives. CPU search overhead, final repeated
   comparison data, hosted validation, and the scanner backlog remain separate
   gates; do not equate distinct GPU search budgets with universal distinct sizes.
   The [decoder-context investigation](docs/benchmarks/2026-10-02-decoder-context-review.md)
   rejected lazy heap-context reuse after 22 alternating RAM-only runs: the
   pinned one-shot decoder already uses stack contexts, and the candidate did
   not establish a repeatable gain. Independent-frame error and selective-decode
   regressions are retained; the experimental runtime API is not production code.
   The subsequent [cooperative CRC review](docs/benchmarks/2026-10-02-cooperative-crc-review.md)
   shortens resident GPU CRC serial chains while preserving ordered IEEE CRCs,
   compact result geometry, and archive bytes. It also repairs empty-job GPU
   work identity and empty-layout validation. All 555 native tests and 28 full
   local gates pass; three seven-block CPU/GPU sweeps cover the candidate.
   Alternating diagnostics establish profile-dependent verification gains,
   not a general application speedup. Mixed extraction remains slightly slower
   and needs stage-level investigation before final performance acceptance.
   Six-target HIP compilation is not six-device runtime validation.
   Native pipeline and RAM-benchmark buffer admission now share an overflow-safe
   physical-memory policy. It rejects insufficient/unknown capacity instead of
   forcing one window, and removes the benchmark's two-buffer discount.
   Synthetic host-size and reserve-boundary regressions cover the policy without
   stressing the host. This is not complete process-memory accounting: CPU codec
   workspace, candidate metadata, and retained pools remain review items.
   Zstandard Strong/Maximum now use bounded higher-effort backend settings.
   Apply this review to every CPU-backed format as well as SUZIP: share codec
   policy across single-file and container wrappers, verify supported levels,
   byte-exact roundtrips, complete encoded sizes, streaming/resource bounds,
   and independent readers. Do not mistake format-matrix correctness for
   measured efficiency or force identical capabilities onto uncompressed
   containers, fixed-policy codecs, and extract-only formats.
   Shared Gzip/Bzip2/Zstandard streams now reject invalid effort before touching
   output paths. Standalone Gzip reuses the container stream writer, whose
   redundant input copy has been removed with scoped input lifetime handling.
   [Common stream contracts](docs/compatibility-stream-contracts.md) cover all
   levels, caller-buffer reuse, close behavior, and byte identity; timing and
   the rest of the all-format efficiency review remain open.
   Bzip2 also consumes caller input without a staging copy. Standalone
   Zstandard now supplies exact source size to bound codec workspace, with
   regression-tested size admission and unknown-size container behavior.
   Measured small-file allocations fall substantially, but a held-out fixture
   exposed a ten-byte size increase; this is not a universal ratio/speed gain.
   TAR.ZST now counts the exact serialized stream using its actual header,
   PAX, padding, and footer writers, without reading payloads twice. New
   Unicode fixtures exposed an existing extraction bug: PAX encoding and
   internal publication paths now receive explicit UTF-8 conversion, with
   legacy code-page behavior retained where no UTF-8 declaration exists.
   Native SUZIP and ZIP now preserve Unicode filesystem names through their
   declared encodings; ZIP also decodes unmarked names as CP437. CLI process
   arguments and format detection no longer narrow Unicode paths through the
   host ANSI code page. Process-level Unicode checks cover the native, ZIP,
   and writable TAR containers. CPIO/CPIO.GZ and AR now default to strict UTF-8
   member decoding with an explicit Windows ANSI legacy option shared by CLI
   and GUI. DEB/RPM wrappers forward the same policy; archive verification and
   quarantine extraction capture the selection with the job.
   The 7z, WIM, and XAR readers now also preserve their Unicode metadata at
   filesystem joins. WIM staging lookup uses the same explicit UTF-8 contract.
   Shared publication diagnostics no longer lose the original overwrite error
   while trying to narrow a valid Unicode filename to the host code page.
   New fixtures cover supplementary names, empty entries, single/multiple WIM
   images, and exact overwrite behavior; legacy encoding policy remains open.
   Native CPU blocks no longer reject every sub-512-byte compression candidate.
   Actual encoded size selects Raw or Deflate with bounded candidate capacity.
   All-nine-level tests and 189 independently decoded real archives establish
   small-file/tail savings and backward-reader compatibility, not a speed gain.
   The shared miniz backend now compares fixed and dynamic Huffman costs before
   packing eligible blocks, without repeating match search. Independent checks
   across 300 native/ZIP/Gzip/TAR.GZ/CPIO.GZ archives found 57 size reductions
   and no growth; throughput and broader release validation remain separate.
   CPIO diagnostics now preserve Unicode destination paths rather than masking
   overwrite refusal with an ANSI conversion failure. A plain/compressed CPIO
   regression checks exact diagnostic text, unchanged refused output, and
   successful explicit overwrite. Unmarked-name regressions cover Unicode
   roundtrips, ANSI names under the actual host code page, malformed UTF-8,
   AR raw-byte offsets, and both CPIO stream passes. Broader legacy-format
   encoding review and production release validation remain open.
   GUI completion now consumes the applied destination-opening preference and
   the extraction-specific option from the captured job, dispatching one
   UI-thread request only after complete success. Compression/extraction
   history uses UTF-8 diagnostics for Unicode destination paths. Solid mode,
   timestamp policy, source deletion, and the optional metadata preflight still
   need end-to-end behavior review; these are not covered by the completion fix.
   Native HIP creation now batches up to 64 small files under an 8 MiB/chunk
   cap, preserving independent CRCs, source locks, and exact version-three
   archive bytes. All-level and all-block-size regressions pass; repeated
   RAM-only comparisons show large local small-file submission gains without
   ratio changes. See [the measured scope](docs/small-file-gpu-batching.md);
   sustained throughput and broader release validation remain separate gates.
   Production codec/diagnostic timing now binds HIP events to dispatches rather
   than separate stream markers. A standalone probe reproduced negative marker
   durations outside the app; repeated dispatch measurements and concurrent
   production regression tests passed. Invalid timing remains unavailable and
   still fails benchmark evidence gates. Full timing sweeps and portability
   beyond the available GPU remain separate requirements.
   Shared host CRC combination now reuses a fixed 8 KiB immutable operator
   table, with independent zlib golden checks across the full 64-bit length
   range. HIP codec readiness no longer gathers unused diagnostic metadata
   for every chunk; trusted loading, device checks, and allocation admission
   remain explicit. Paired diagnostic verification gains do not establish
   faster compression kernels or a general application speedup; see
   [the measured scope](docs/benchmarks/2026-09-30-crc-readiness.md).
   The latest review consolidates ten standalone file-size readers, seven
   filename-derived output policies, Gzip single-file/container decoding, and
   bounded Base64/UUencode/XXEncode line I/O. Unicode filename and overwrite
   regressions cover all seven affected compressed single-file readers.
   UUencode now rejects trailing data on zero-length terminators before output
   publication. Native index path budgets are enforced before allocation/I/O.
   Required-HIP copies use the same per-thread stream as their kernels while
   retaining synchronous host-buffer lifetimes. The benchmark and System GUI
   now select the busiest GPU engine rather than summing independent engines;
   the GUI first combines processes sharing a physical engine. Benchmark
   schema two identifies the corrected process metric without rewriting old
   records. A 54-run RAM-only sweep confirms unchanged encoded sizes and zero
   archive disk writes; GPU extraction remains slower than CPU in these
   profiles, and mixed-profile timing drift requires follow-up. These are not
   universal speed claims or completed all-format performance comparisons.
   Native comparison rounds now alternate CPU/GPU order and record their
   configurable pause outside the timed command; a pause is not evidence that
   thermal or resource drift has been eliminated.
   The runtime/allocation review now records the exact loaded HIP file version
   and shares the Windows compatibility manifest across CLI, GUI, and tests.
   Combined-device-allocation and zero-retention stream-allocator candidates
   passed selected correctness checks but regressed against matched controls;
   neither is shipped. Native extraction and its RAM benchmark now share a
   bounded owned decode buffer without a redundant zero-fill pass. Local
   repeated measurements retain identical archive sizes but do not establish
   a stable cross-workload speedup. See
   [the experiment and limits](docs/benchmarks/2026-09-30-runtime-allocation-review.md).
   Gzip, Bzip2, and Zstandard stream readers also share the UTF-8-safe file-size
   helper: a missing supplementary Unicode filename previously masked the real
   I/O failure with a host-code-page conversion exception. The regression now
   checks the original ArchiveError and intact filename across all three.
   The follow-up also repairs Unicode create/open diagnostics and the XAR
   reader's reversed stored-length/decoded-size fields. Independent stored and
   zlib XAR producers now have permanent interoperability checks. The corrected
   local fuzz seed is valid XML and restores exact bytes through the product
   CLI. All 22 full local verification commands passed with 517 C++ tests;
   hosted analysis, broader performance comparisons, and release acceptance
   remain open as recorded in the checkpoint above.
   The shared IEEE checksum now uses the existing pinned SDK backend with one
   initialization guard shared by native and 7z callers. Three paired rounds
   across all three 10 GiB profiles show repeatable local extraction gains,
   unchanged encoded sizes, and GPU extraction still slower than CPU. An
   independent all-effort archive smoke then exposed a dictionary admission
   cutoff: a better entropy result could suppress a still-smaller dictionary
   candidate at effort 6. Its removal passes the new before/after regression
   and exact CPU/HIP read-back checks. See
   [the controls, negative phases, and open gates](docs/benchmarks/2026-10-01-shared-crc-backend.md).
   The combined batch now passes all 22 local gates with 521 C++ tests.
   The subsequent owned-output review adds aggregate-bounded pinned storage,
   operation-local reuse, and a shared native/RAM decode worker policy. Queue
   admission and pool reuse alone regressed one profile; root-cause inspection
   found underused CPU checksum capacity. Bounded parallel checksums still
   validate the actual host output. Three matched rounds show GPU extraction
   gains of 1.38-1.47x, 1.60-1.66x, and 1.98-2.03x across the three profiles,
   with unchanged archive sizes; GPU still trails CPU on two profiles. The
   full pool snapshot passes 22 local gates with 527 tests, and the subsequent
   checksum/worker delta passes ten selected gates with 530 C++ tests. See the separate verifier
   evidence, unsuccessful candidates, counter limitations, and open acceptance
   gates in [the bounded-output checkpoint](docs/benchmarks/2026-10-01-pinned-output-pool.md).
   A final pre-push full run separately passes all 22 gates with 530 C++ tests.
   These are development measurements, not a release or general ratio gain.
   Build, test, packaging, and MSI identity checks now share pinned CMake
   4.4.3 and installed-toolchain discovery rather than host-specific paths.
   The follow-up VRAM review found that adapter-wide Windows usage was summed
   across GPUs but compared against only the selected HIP device's capacity.
   Adapter and process memory samples now match its Windows LUID through one
   PDH reader and a tested, overflow-checked identity aggregator. Unavailable
   identity leaves HIP memory accounting intact without borrowing another
   GPU's counters. This does not change the total-system GPU utilization graph.
   LZMA-Alone, lzip, and 7z now share an SDK allocator whose callback interface
   owns its byte budget directly. The former thread-local ownership stack is
   removed; lzip's budget remains scoped to the complete stream, not each
   concatenated member. The existing 528 MiB LZMA/lzip and 2 GiB 7z limits,
   zero-initialized allocations, and exception containment are preserved.
   Direct callback tests exercise isolated owners, exact/zero limits, refusal
   before oversized allocation, released capacity, and sequential thread
   handoff. Real lzip fixtures also cover interleaved stream lifetimes and
   cross-thread consumption. This is an ownership correction, not a measured
   compression-kernel speedup or a claim of concurrent access support.
   The standard-format timing review now exposes optional Zstandard setup,
   stream/I/O, and durable-publication intervals in normal CLI statistics.
   A block-buffer-only trial did not explain the measured compression gap.
   Shared worker admission now accepts known streams from 16 MiB and caps
   workers by complete 8 MiB units, CPU headroom, and four workers overall.
   Unknown, smaller, and high-effort streams remain synchronous. Paired local
   diagnostics show a substantial `nci` creation gain with a 4,720-byte size
   tradeoff; `mozilla` has unchanged size and no clear timing win. These are
   workload-specific diagnostics, not a universal competitor or GPU claim.
   Fresh clean-source comparisons, graphs, broad remaining review, and release
   acceptance are still required.
   Miniz now shares the existing IEEE checksum backend through its official
   external hook, covering ZIP and the common Gzip wrappers without a new
   dependency or removed checks. Independent CRC-oracle and ABI tests pass,
   as do 533 HIP-enabled tests, 532 isolated CPU-only tests, and 22 full gates.
   All fourteen native seven-block-size results retain prior encoded sizes.
   The application diagnostic has identical new/control archive hashes but
   includes extraction outliers and storage activity; it is not a published
   speed claim. See the [checksum controls and limits](docs/benchmarks/2026-10-01-compatibility-crc-hook.md).
   The completed `c21b435` analysis closes five owned-source quality warnings.
   Its 208 unapproved code-scanning alerts and scanner dependency conflict
   still block final acceptance; no release has been created.
   The subsequent scanner revision tests Semgrep 1.178.0 with PyJWT 2.15.1 and
   promotes the compatible pair into the canonical security workflow through
   a pinned, separately identified downstream wheel. Normal hash-locked
   installation and `pip check` pass; five runtime regressions and equivalent
   full scans retain the scanner's coverage. Eight packaging tests and all
   24 full local verification commands pass. This is local CI-path evidence,
   not hosted workflow success or alert closure. The MCP scripts no longer
   shadow the installed SDK package. CocoIndex also handles Git-confirmed
   unstaged deletions, with 12 passing offline regressions and a successful
   real index refresh. The follow-up full verifier and exact-SHA post-push
   audit remain required before accepting this batch.
3. Complete relevant frontend smoke, regression, sanitizer, packaging, and
   resource-aware RAM-only performance gates. Defer only timing-sensitive runs
   when host contention is material; leave unrelated tasks untouched.
   Before a possible proprietary edition, review contributor rights and every
   dependency license; do not assume repository privacy changes existing
   license grants. Audit public package contents, symbols, embedded build paths,
   and diagnostics. Evaluate selective native-code obfuscation only with
   measured CPU/HIP, size, compatibility, and debugging costs. Preserve private
   diagnostic symbols where needed, required notices, and format interoperability;
   do not claim that a distributed executable cannot be reverse-engineered.
4. Push the verified iteration, audit its exact commit's workflows, code-scanning
   results, Dependabot alerts, and open pull requests, and fix regressions in
   follow-up pushes. Run the installed Codex Security workflow with the minimum required worker
   count, enumerate all candidate instances, fix validated issues, and verify
   the fixes. Update the repo-local security skill after closure, not before.
5. Revisit kernel and pipeline efficiency after the post-push audit. Validate
   portability beyond the local host, distinguishing compiled targets, hosted
   CPU-only test configurations, and GPU hardware actually tested. Retain only
   performance changes supported by correctness and controlled measurements.
   Product release builds now select six HIP GPU targets through one shared,
   tested architecture resolver. The HIP library now links five codec objects;
   current local tests and GUI smoke exercise the available gfx1201 device.
   Release validation must check every requested target image in the final
   artifacts. No local result establishes hardware support for other families.
   Native HIP dictionary matching has nine bounded effort budgets. The prefix
   path now evaluates progressively sampled adaptive candidates at levels 2-9
   when useful, but repeated sizes remain on saturated or static-favored data.
   Continue developing meaningful measured compression strategies, not
   artificially weakened lower levels.
   Cover low-byte and shifted-byte entropy, longer repeats, small-file batches,
   and incompressible data. Track compression speed and encoded size together;
   removing redundant candidate work alone is not the requested dramatic joint
   speed-and-ratio improvement or the final kernel/pipeline redesign.
   Prioritize repeated-substring matching and stronger entropy coding over
   further small scheduling/bit-packing experiments unless profiling identifies
   those operations as limiting the full pipeline. Evaluate a learned codec or
   effort selector only after useful backend alternatives exist, against a
   deterministic selector on held-out workloads. Count inference time, memory,
   model/package size, portability, and decoding dependencies; do not add cloud
   calls or model downloads to ordinary archive operations.
   The production [HIP dictionary codec](docs/gpu-dictionary-codec-development.md)
   produces real independent block payloads with nine effort-dependent sizes
   on a controlled repeated-record fixture. Its cooperative HIP decoder now
   restores independent blocks with bounded workspace and exact-output checks.
   Dense/tiled search now preserves encoded bytes while reducing global GPU
   workspace in high-effort large batches. Automatic dispatch retains dense
   search where tiled search measured slower; controlled host-wall comparisons
   and their shared-host telemetry limits are recorded in the codec document.
   Native version-four archive integration, the bounded production CPU reader,
   and complete dictionary candidate-cost selection are implemented. Tests and
   diagnostics use the same codec core; standalone dictionary decoding now
   also shares production dispatch and timing. Historical prototype documents
   must not be treated as evidence of a separate shipping codec. Broader
   portability, controlled application timing, all-format comparisons, and
   final release validation remain open.
6. After implementation and performance changes, review the entire repository
   with a file-level coverage record. Include first-party source, frontend,
   backend, tests, scripts, workflows, build/release configuration, skills, and
   documentation. Account for generated files through their generators and
   vendored code through source/provenance review; preserve upstream archives.
   Apply consistent first-party coding/comment conventions and refactor only
   where reasoning and evidence establish a correctness, efficiency, clarity,
   or maintenance benefit. A green test alone is not a design review.
7. Perform the maintainer-requested extensive final bug-hunting pass after all
   planned code changes. Review complete user journeys and failure/lifecycle
   paths across frontend, backend, formats, resources, installation, and
   supported configurations. Track reviewed files and unresolved areas rather
   than implying unchecked code is complete. Every fix needs regression
   evidence, renewed affected checks, and exact-commit post-push verification.
   Dedicated security work remains deferred until the maintainer resumes it;
   deferred checks cannot count as passed or support a clean-security claim.
8. Push a coherent verified commit, reconcile all dependency PRs, wait for all
   required workflows including release fuzzing, audit remaining alerts and
   deployments, then publish a new appropriately bumped release. Do not alter
   findings merely to manufacture a target alert count.

## Direction

SuperZip is a native Windows x64 archive application written in modern C++20.
The GPU boundary is AMD HIP only. The native `.suzip` format is the
GPU-accelerated product path, while standard `.zip` support is compatibility
only through miniz 3.1.2. Uncompressed `.tar` support is compatibility only
through SuperZip's native bounded TAR adapter.

Portable ZIP and MSI packages must be functionally identical HIP-enabled
artifacts. CPU-only builds exist only so hosted CI can run static analysis and
core archive tests where AMD HIP is unavailable.

## Requirements

- AMD HIP is the only GPU acceleration method.
- No CUDA, WebGPU, DirectCompute, OpenCL, or cross-vendor fallback.
- Windows x64 only.
- Portable and MSI packages must both contain the same HIP-enabled executables.
- The app must delay-load the AMD HIP runtime and report missing prerequisites
  through `dependency-check`, not fail with a Windows loader dialog.
- The MSI and release workflow must validate that the installed artifact is
  HIP-enabled. They must not silently install or downgrade AMD GPU drivers.
- Archive work must be chunked and bounded in memory.
- Native archive chunks are hard-capped at 128 MiB, archive metadata counts are
  bounded, and HIP allocations preflight available VRAM before kernel work.
- Native `.suzip` blocks may be fill, raw, or bounded miniz-deflate payloads;
  the GPU acceleration boundary remains AMD HIP-only.
- Compatibility archives must use in-process parsers/writers. Product code must
  not shell out to host archive tools or silently route through untracked
  fallback utilities.
- Extraction must reject traversal, absolute paths, UNC paths, reserved Windows
  device names, malformed metadata, CRC mismatches, and accidental overwrites.
- Microsoft Defender scanning and SHA-256 hashing remain opt-in.
- No secrets, personal paths, local machine names, build artifacts, or release
  archives may be committed.

## Architecture

```text
src/
  app/        Native Win32 GUI and DPI-aware layout
  cli/        Command-line automation and dependency checks
  core/       Archive model, safety validation, integrity, progress
  gpu/        AMD HIP device discovery and GPU codec boundary
  tar/        TAR compatibility adapter with two-pass validation
  zip/        ZIP compatibility adapter using miniz 3.1.2
tests/       C++ regression tests
tools/       Build, package, benchmark, and security automation
third_party/ Patched production dependencies plus upstream provenance copies
```

The only layer allowed to call HIP is `src/gpu/`. Higher layers consume typed
archive options, progress snapshots, and errors.

## Iterations

### Iteration 1: Architecture

- Replaced prototype-level shell assumptions with a native C++/Win32 app.
- Preserved AMD HIP as the fundamental acceleration boundary.
- Separated native `.suzip` from `.zip` compatibility.

### Iteration 2: Responsiveness

- Added background archive work.
- Kept progress state explicit and sampled.
- Designed the GUI around PerMonitorV2 DPI and high-refresh repaint coalescing.

### Iteration 3: Security

- Added archive path validation and malicious-entry tests.
- Added optional Defender scanning and optional SHA-256 integrity checks.
- Added layered CI security scans and OpenVAS/Vulnetix integration lanes.

### Iteration 4: Packaging

- Made HIP the default local build.
- Added explicit CPU-only validation mode for hosted CI.
- Switched to the static MSVC runtime so packages do not need a VC redistributable.
- Delay-loaded the AMD HIP runtime and added `dependency-check`.
- Made portable packaging fail closed for CPU-only builds.
- Added release inputs for HIP SDK installer checksum verification and WiX v7
  EULA acknowledgement.

### Iteration 5: Vendored Dependency Hardening

- Preserved an unmodified miniz 3.1.1 upstream source archive under
  `third_party/upstream/miniz/3.1.1/`.
- Kept the patched production copy under `third_party/miniz/`.
- Removed static-analysis findings without changing ZIP compatibility behavior.

### Iteration 6: Benchmarking And Tuning

- Exposed native `.suzip` compression levels 1, 3, 5, 7, and 9 in the GUI and
  CLI, with level 5 as the balanced default.
- Added RAM-only compression-ratio reporting so CPU and GPU throughput is
  compared at equivalent compression strength.
- Added the built-in `benchmark-suite` command for numerical system scoring and
  production block-size autotuning without multi-GB SSD writes.
- Added refactoring governance and an audit helper so future cleanup is planned,
  measured, and behavior-preserving.

### Iteration 7: Real Archive Compatibility

- Added an archive-format registry and `formats`/`identify` CLI commands.
- Added native uncompressed TAR create/extract support with archive-wide path
  validation before any output is written.
- Kept document/package ZIP containers out of the user-facing format matrix.
- Updated GUI extraction to auto-route implemented SUZIP, ZIP, and TAR formats
  while reporting recognized unsupported formats explicitly.
- Added extract-only XZ and TAR.XZ compatibility through vendored XZ Embedded,
  preserving two-pass TAR validation and bounded decoder memory.
- Added Zstandard and TAR.ZST compatibility through the bundled app-local
  official libzstd 1.5.7 runtime, preserving single-file stream semantics,
  frame-checksum creation, and two-pass TAR validation.
- Added read-only ARJ stored-entry extraction through a native bounded parser
  with header CRC validation, payload CRC validation, archive-wide path
  validation, and explicit rejection of compressed/encrypted/multi-volume ARJ
  variants until a vetted decoder path is added.
- Added read-only SEA ARC/ARK unpacked-entry extraction through a native
  bounded parser with CRC-16/ARC payload validation, archive-wide path
  validation, strict end-marker handling, and explicit rejection of compressed
  methods or unrelated `.arc` file families until dedicated decoders exist.
- Added extract-only lzip `.lz` and TAR.LZ `.tar.lz`/`.tlz` compatibility over
  the vendored LZMA SDK, with lzip version/dictionary validation,
  EOS-enforced decoding, CRC32/data-size/member-size trailer checks,
  concatenated-member handling, two-pass TAR validation, and fuzz coverage.
- Added CPIO.GZ `.cpio.gz`/`.cpgz` compatibility through the Gzip stream adapter
  over the bounded CPIO adapter, preserving two-pass inner CPIO validation,
  checksum/trailer rejection, no decoded-archive disk staging, and fuzz
  coverage for CPIO metadata/path handling.
- Added Base64 `.b64` single-file compatibility through a native bounded text
  adapter with strict RFC-style padding validation, optional wrapper-header
  filename validation, overwrite refusal, and verified temporary-file
  publication.
- Added extract-only BinHex 4.0 `.hqx` data-fork compatibility through a native
  bounded text adapter with strict HQX alphabet parsing, RLE expansion,
  path-safe header filenames, header/data/resource CRC validation, resource
  fork discard on Windows, overwrite refusal, and verified temporary-file
  publication.
- Added extract-only MacBinary `.macbin` and strongly header-detected `.bin`
  data-fork compatibility through a native bounded adapter with path-safe ASCII
  header filenames, data/resource extent validation, MacBinary II/III header CRC
  validation, generic `.bin` false-positive protection, overwrite refusal,
  verified temporary-file publication, and fuzz coverage.
- Added XXEncode `.xxe` single-file compatibility through a native bounded text
  adapter with strict alphabet validation, path-safe begin-line filenames,
  overwrite refusal, and verified temporary-file publication.

## Validation Gates

For HIP-capable Windows hosts:

```powershell
tools\build.ps1 -Configuration Release
tools\test.ps1 -Configuration Release
build\Release\superzip_cli.exe dependency-check
tools\gpu_proof.ps1 -Configuration Release -SizeMiB 8
tools\package.ps1 -Configuration Release
tools\bench.ps1 -Configuration Release -SizeMiB 10240 -Profile Mixed -CompressionLevel 5 -Iterations 1
build\Release\superzip_cli.exe benchmark-suite --profile Mixed --compression-level 5 --tune
```

For hosted CI without HIP:

```powershell
tools\build.ps1 -Configuration Release -CpuOnlyValidation
tools\test.ps1 -Configuration Release
tools\security_scan.ps1
```

Before publishing, GitHub workflows must complete without skipped user-authored
jobs, deployments must remain absent, and open vulnerability alerts must be
triaged through real fixes or documented external governance constraints.
Release performance notes must include forced-CPU and required-HIP runs with
CPU, GPU, logical-disk active time, and disk throughput telemetry for mixed,
compressible, and incompressible workloads.
Required-HIP claims must also include backend `gpu_*` counters proving HIP
kernel launches, HIP event time, transfer bytes, and device allocation bytes.
