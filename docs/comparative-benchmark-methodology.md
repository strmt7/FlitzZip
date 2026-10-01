# Comparative Archive Benchmark Methodology

This protocol is for a bounded comparison of **applications**, not a claim that
one codec, CPU, or GPU is universally faster. It complements, but does not
replace, the 10 GiB RAM-only CPU/GPU tests in
`docs/performance-block-size-validation.md`. GitHub-hosted runners may check
records and regenerate graphs; their variable hardware is not the source of
published performance measurements.

**Permission takes precedence over measurement.** Read
[Benchmark Permissions And Tool Selection](benchmark-permissions.md) before
installing, executing or publishing any comparator or corpus. Silesia is
excluded from new runs and headline publication; licensed replacements are
used instead of depending on permission requests. Existing records document historical experiments;
they are not the next release's cleared benchmark set.

## Research Basis

[Benchmark Research And Adoption Decisions](benchmark-research.md) records
the dated source review, scientific papers, tool boundaries and access limits.
It distinguishes reviewed practices from work not yet implemented or measured.

- [Tom's Hardware's archive-tool comparison](https://www.tomshardware.com/reviews/winrar-winzip-7-zip-magicrar,3436-6.html)
  disclosed its hardware, mixed-file workload, formats, and exact commands.
  Its ZIP and proprietary-format results were separated rather than pooled.
- [PCWorld's system methodology](https://www.pcworld.com/article/468690/how_we_test_pcs.html)
  timed compression and decompression of the same mixed-file tree; its result
  was a storage-system test, not a codec-isolation test.
- [Tom's Hardware's newer test-suite account](https://www.tomshardware.com/pc-components/cpus/behind-the-scenes-of-our-massive-cpu-retest-for-bench-testing-at-1080p-choosing-new-apps-and-gathering-data-for-a-decade-of-cpus)
  treats compression and decompression as separate workloads and tests more
  than one application so a single favorable workload cannot decide a result.
- [PeaZip's application comparison](https://peazip.github.io/peazip-compression-benchmark.html)
  reported input corpora, machine/storage context, five repetitions, archive
  size, compression time, and extraction time. It also identified different
  default-effort choices as a source of apparent product differences.
- [Hyperfine's documentation](https://github.com/sharkdp/hyperfine#readme)
  explains warmups, cache effects, per-run JSON, and outlier diagnostics.
  These are useful practices regardless of which timing harness runs a case.
- [AnandTech's encoding methodology](https://www.anandtech.com/show/16195/a-broadwell-retrospective-review-in-2020-is-edram-still-worth-it/7)
  uses repeated 7-Zip CLI built-in scores separately from an application
  workload. Built-in ratings provide CPU/codec context, not archive size or
  whole-application creation time. OpenBenchmarking profile pages were not
  accessible in the current review; no current run-count policy is inferred.
- [lzbench](https://github.com/inikep/lzbench) is an in-memory multi-codec
  comparator with timed loops and round-trip verification. It requires codecs
  to be integrated into its binary; it cannot measure SuperZip's application,
  archive container, or GPU path without a maintained integration.
  Its results are contextual, not a substitute for archive measurements.
- The [Silesia corpus author](https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia)
  publishes file descriptions, exact byte counts, and original MD5 digests.
  [Canterbury](https://corpus.canterbury.ac.nz/descriptions/) explains why
  corpus selection matters and why no small set represents every user file.
  [Zstd's CLI manual](https://github.com/facebook/zstd/blob/dev/programs/zstd.1.md)
  expressly warns that its in-memory benchmark depends on file content and
  uses a different measurement boundary from normal CLI I/O.

PCMag's site was not accessible to the research tools when this protocol was
written. No PCMag-specific procedure is inferred or attributed here. The
sources above are examples of disclosed review methods, not authorities that
make any individual SuperZip result representative.

## Scope And Corpus

The historical product lane used the author's unmodified Silesia files. The
multi-file ZIP case contains `dickens`, `ooffice`, `samba`, `xml`, and `x-ray`
(51,770,558 input bytes in total). Single-file Zstandard cases use `mozilla`
(51,220,480 bytes) and `nci` (33,553,445 bytes) separately. Do not relabel a
subset as the full 211,938,580-byte Silesia corpus. Historical raw-file SHA-256
pins and exact byte counts remain part of the archived measurement identity.
The original MD5 values corroborated the initial pins but are not the
operational integrity gate. Retain source URLs and download SHA-256 values;
do not rerun these held inputs or relabel them as replacement-corpus data. No corpus
payload or generated archive belongs in Git.

The replacement workload plan includes licensed large structured data
(UCI HIGGS, with CC-BY-4.0 attribution) and separately reviewed mixed-file and
small-file corpora. No unreviewed corpus is eligible merely because a timing
harness supports it. HIGGS alone cannot represent documents, software,
media and small-file trees. Preserve historical records rather than renaming
Silesia measurements as results for a different corpus.

Each case stays below the repository's 64 MiB filesystem cap. This bounded
product comparison includes CLI startup, filesystem reads/writes, and normal
archive I/O. It is **not** interchangeable with the RAM-only native CPU/HIP
throughput graph. A second, separately labeled lane may use a disclosed
user-like document/media tree, but its licensing, manifest, and hashes must
be settled before it is charted. Incompressible controls and small-file trees
are useful diagnostics, not substitutes for real-file cases.

### Replacement Input Manifest

`python -m tools.run_archive_comparison --manifest manifest.json --corpus raw`
selects reviewed replacement inputs instead of the historical Silesia path.
Also provide `--superzip`, the `--sevenzip` or `--zstd` executable required by
each case, and a new `--output` beneath ignored `out/benchmarks/`. This path is
still a bounded filesystem diagnostic, not the large RAM-only throughput lane.
No payload download, data duplication or trial software is implied.

The manifest has exactly `schema_version: 1`, `name`, `files` and `cases`.
Each file has `name`, exact `bytes` and `sha256`, a catalog `subject`, a
`source_url` matching that subject's reviewed evidence, and `transformation`
describing any split or conversion, or explicitly stating `unmodified`.
Inputs are flat regular files with Windows-safe names; links, duplicate names,
incorrect hashes and cases above 64 MiB are refused. Verify the original
download and transformation independently before approving its manifest:
matching a declared hash does not itself prove that a file came from UCI.

Each case has `name`, `format`, `files` and `tools`. Current implemented pairs
are `["SuperZip", "7-Zip"]` for `zip` and `["SuperZip", "Zstd"]` for a
single-file `zst` case. Every listed file must be used. Bandizip and PeaZip
remain selected candidates, not implemented adapters or measured series.
Replacement runs produce schema-two records with the manifest hash; the
historical graph validators remain schema-one and cannot publish these new
records until the replacement-series chart review is completed.

For Zstandard diagnostics, the normal SuperZip CLI also reports disjoint
`setup_seconds`, `stream_seconds`, and `publication_seconds`. Setup includes
runtime integrity checks and path admission. Streaming includes codec work,
file reads/writes, frame validation, and stream close; it is not pure codec
time. Publication includes the durable flush and atomic commit. These optional
adapter intervals exclude process startup, final reporting, and any outer CLI
quarantine/publication wrapper, so they do not replace whole-command time or
describe competitors' internal phase costs.
Absent phase telemetry is not a zero-duration measurement. Do not remove
integrity checks or durability guarantees to improve comparison scores.

## Comparators And Configuration

Use a clean, built SuperZip commit on Windows and the latest stable,
provenance-checked official 7-Zip CLI for ZIP. Use the latest stable official
Zstd CLI for single-file `.zst`
cases. These are separate format panels: never rank `.zip`, `.zst`, and
`.suzip` archive bytes as if the containers and semantics were identical.
Record each executable's version, SHA-256, source URL, full arguments, build
mode, OS version, CPU/GPU models, RAM, storage type, and relevant thread limits.
No competitor is silently assigned SuperZip's GPU capability. Native SUZIP
CPU and GPU measurements remain a distinct paired study.

Bandizip Standard is the eligible commercial ZIP alternative, subject to
edition proof, exact command verification, and independent decoding. WinZip
is excluded under the current reviewed terms; WinRAR is excluded from the
free-tool plan. PeaZip's official packages are eligible for reviewed ZIP/7z
comparisons after application-adapter validation; NanaZip is not in the active
selection while package-level review remains incomplete. The permission matrix
records these decisions and their sources. Do not copy competitor software,
branding, manuals or graphs into the product or benchmark publications.

Set both ZIP writers to ZIP Deflate explicitly. This first study selects
numeric level 5 explicitly in SuperZip, 7-Zip, and Zstd; it is not described as
each tool's default. Record all actual flags. Optional low/high settings are
separately labeled tradeoff points.
Equal numeric levels across tools do **not** mean equal algorithmic effort or
equal compression strength. Compare time against achieved bytes, not speed at
an allegedly equivalent level. A tool's built-in benchmark is contextual
hardware data only and must not be plotted as product archive throughput.

The final effort study uses all supported levels 1 through 9. Earlier charts
with levels 1, 3, 5, 7 and 9 are historical records, not a complete nine-level study.
Each setting is a **tool-local control**, not equivalent work across tools:
SuperZip's `.zst` levels map to Zstandard backend efforts differently from
the official CLI, and `.suzip` selects among static, adaptive, Huffman, and
dictionary HIP candidates by complete encoded size. The
graph uses achieved whole-archive bytes and whole-command time, not numeric
level as a proxy for quality. It may show same-size plateaus; no result is
altered to make every control produce a distinct size. Native `.suzip`
CPU versus GPU effort sweeps are separately labeled and
RAM-only, never pooled with compatibility-format sizes.

## Execution And Correctness

### Machine And Software Disclosure

The published record must identify the exact SuperZip commit and executable
SHA-256, x64 Release/HIP build configuration and CMake cache digest, plus each
comparator's version, executable SHA-256, and official source URL. Report the
Windows edition/build; CPU model, physical core count, and logical thread
count; GPU model and driver; installed RAM, module count, and configured
transfer rate; and source/output volume's storage model and bus. Report any
explicit thread limits and active CPU/GPU mode. These fields are environment
description, not an assertion that competing tools use equivalent algorithms
or GPU acceleration. The current product comparison invokes the normal CLI
paths; it does not claim that HIP ran unless HIP telemetry proves it for the
specific command. Do not report GPU memory from the 32-bit Windows
`Win32_VideoController.AdapterRAM` field, which can truncate modern VRAM.

For every case, retain pre/post CPU load, free RAM, paging rate, disk busy
time, and GPU engine utilization, sampled outside timed commands. This is
context for competing host activity, not continuous telemetry or proof of an
idle machine. CPU load comes from Windows' `_Total` formatted processor
counter; a missing value fails the record rather than becoming a fictitious
zero. Record the measurement date, run order, command flags, corpus
hashes and bytes, individual timings, archive hashes and bytes, and source
reference archive identity. Disclose any changed power plan, thermal limit,
background workload, or observed throttling in the review notes; if unknown,
say so rather than inventing a control. Exclude usernames, unrelated process
details, machine serials, and private paths from committed results.

1. Inspect host CPU, GPU, free RAM, paging, and disk activity before each
   series. Do not stop unrelated user work. Defer timing only when contention
   is substantial; record observed interference and rerun the affected case.
2. Verify all input byte counts and hashes. Use the same immutable input tree
   on one volume for every tool. Precreate empty output parents outside the
   timed command, warm the input cache once, then run one untimed warmup per
   command. Do not mix cold-cache and warm-cache results.
3. Take at least five timed runs per tool, case, and direction. Alternate tool
   order between rounds (AB/BA), run no cases concurrently, and keep exact
   per-run wall times rather than only a mean. Timer boundaries encompass the
   whole product command. Cleanup and hash verification are outside timing;
   creation starts without an existing target and extraction starts with an
   empty destination. For extraction, both tools in a case read the **same**
   independently created reference archive: 7-Zip's ZIP or Zstd's `.zst`.
   Record the exact reference archive hash and size. This isolates reader
   comparison from differently sized writer outputs; it does not measure
   every possible archive bitstream. Never time `--verify-after-write` as
   ordinary compression unless every comparator does equivalent work.
4. Check exit code and exact archive bytes after every create. Extract each
   resulting archive with an independent compatible decoder and reject the
   run unless relative paths and every extracted SHA-256 match the manifest.
   Verify every timed reference extraction too. Include archive
   headers/container overhead in the size metric; do not substitute raw
   codec-buffer sizes.
5. Record run-start/run-end resource context and observed competing processes
   without identifying unrelated users or processes in committed data. Repeat
   noisy or order-sensitive cases once under calmer conditions; keep both
   attempts and their reason instead of selecting only the favorable result.

Use median elapsed seconds as the headline, with all individual samples
and labeled observed min/max error bars in every final time/speed plot.
These ranges show dispersion, not a confidence interval. Deterministic size
results retain an honest zero-width size interval. Archive size is
exact bytes; ratio is `archive_bytes / input_bytes` (lower is smaller).
Throughput is `input_bytes / elapsed_seconds`, with decimal MB/s labeled.
Do not call a small difference a win when run-to-run spread overlaps it. A
single-host result does not establish portability, a universal ranking, or
energy efficiency. The reference archive may favor one decoder's optimized
bitstream; a broad decoder-ranking claim needs a second crossed reference.
No overall score may conceal a worse size or extraction result.

For final multi-effort and native charts, take **at least five** independent
timed runs per point and direction. Repeat only an affected ambiguous series,
not every minor documentation or code iteration. Preserve all attempts and
the reason for a repeat. A final timing series is eligible for a sustained-speed
headline only when every sample lasts at least one second and each direction
totals at least ten seconds. These are project admission targets, not a
magazine standard. Shorter real operations remain explicitly labeled diagnostics;
do not add delays inside the timer or duplicate files to manufacture duration.
Recheck a representative case with official Hyperfine using a warmup,
at least five runs, its independently disclosed duration policy and exported timings.
State its version, binary SHA-256, timer boundary, setup command, and any
differences in run ordering. Hyperfine timing is a cross-check; the primary
hash-checked AB/BA harness remains authoritative for archive bytes and
correctness. Do not combine samples from the two timing harnesses or silently
replace noisy values. If their distributions disagree materially, investigate
host load, cache state, output cleanup, shell startup, and process behavior
before publishing a speed conclusion.

The effort sweep waits 250 ms after each paired filesystem round, outside the
timer, including after its warmup round. This small fixed rest limits
back-to-back write bursts and gives caches a brief recovery opportunity; it
does not prove a cold cache or that SSD write caching never saturates. The
source/input cache remains deliberately warm. Resource snapshots and repeated
timings are still needed to detect sustained contention. The separate native
10 GiB RAM-only CPU/GPU suite does not insert a disk-oriented cooldown.

The historical native effort chart used the deterministic 10 GiB Mixed
generator, 16 MiB blocks, and three alternating CPU/GPU pairs at product levels
1, 3, 5, 7 and 9. It records exact complete in-memory archive bytes, median
compression seconds, and observed min/max per lane. Verification and
extraction run for every sample but are timed separately. The runner requires
real HIP launches and zero disk writes; generation and CPU orchestration remain
part of the operation. Source SHA, dirty state, binary SHA-256, CPU/GPU models,
workload, and memory mode must match across records. The next final refresh
requires at least five pairs at every level 1 through 9, not the historical
three-sample/five-level design. Single-run size diagnostics never appear as
repeated timing evidence. Inspect chronological samples and resource context
for monotonic slowdown. A greater-than-5% first-to-last rise is a diagnostic
trigger, not automatic proof of cache exhaustion: examine competing load,
thermal state, order and phase timings, then repeat only the affected setting.

## Graph And Publication Contract

The comparison graph must separate format and corpus, show both compression
and extraction, and label exact output size and ratio. Time bars start at zero;
uncertainty whiskers or visible ranges show timing variation. A size-versus-
compression-time scatter is appropriate for multiple effort points, with each
point labeled by tool and setting and non-dominated points identified only
within the same format/corpus. Use accessible color plus labels, not color
alone. Bars start at zero. A scatterplot may focus its archive-size axis on
the observed range only when it explicitly labels the truncation, prints exact
bytes beside every point, and avoids interpreting visual distance as a large
percentage gain.

The effort graph places exact archive size on a zero-origin horizontal axis
and whole-command compression seconds on an explicitly labeled logarithmic
vertical axis because maximum Zstandard effort can be orders of magnitude
slower than low effort. Per-point observed min/max whiskers show timing
dispersion. Dark outlines mark **measured** Pareto points (no other tested
point is both smaller and faster); they are not statistical significance
claims, and the frontier is recalculated per input/format only. Exact bytes
and median/range seconds remain visible as a table next to each panel.

Raw JSON records contain the full commands, file manifest, tool and source
hashes, all individual timings, correctness outcomes, host context, and
publication caveats. The graph generator rejects missing/dirty source
identity, mismatched corpus hashes, unpaired cases, failed round trips,
missing directions, non-finite timings, and fewer than five samples. The
workflow regenerates and byte-checks the reviewed SVG from committed JSON;
measurement is an explicit local run, never an automatic noisy CI benchmark.
Every README chart links to this method and its reviewed record. Unmeasured
formats and systems are shown as unmeasured, not as failures or wins.

## Measurement Cache

Store competitor records separately from SuperZip records. A SuperZip source
change must not invalidate unchanged competitor timings merely because the
combined chart changed. Reuse requires an exact content-addressed identity
covering the methodology, relevant harness/adapter source hashes, immutable
corpus manifest, tool version, executable and dependency hashes, reviewed
edition/module scope, format/options/thread policy, timer/cache boundary and
relevant host hardware/software configuration. Reader timings also bind to
the exact extraction-reference archive SHA-256. An unchanged decoder is not
the same workload when its reference archive changes.
Record actual collection dates; a reused result is never a new measurement.

Changing only chart styling or explanatory text does not invalidate timing.
Changed inputs, measured code, competitor stable release, command flags,
thread counts or material host conditions do invalidate affected records.
Always refresh current version and permission checks before accepting a
cached comparator record for publication. Retain at least five raw samples,
correctness proof and original resource context; do not reuse corrupt,
undersampled, contested or context-incompatible records. Reuse is a savings
mechanism, not a waiver of the latest-version or uncertainty requirements.

`tools/benchmark_cache.py` currently provides metadata import and lookup;
the legacy comparison runner does not yet reuse it automatically. Invoke it
as `python -m tools.benchmark_cache --identity identity.json --result result.json`
to store reviewed evidence, or omit `--result` to look up that exact identity.
Output stays under ignored `out/benchmarks/cache/`; existing evidence is never
overwritten. A miss returns exit code `2`; invalid evidence returns `1`.

Identity JSON has exactly `methodology_sha256`, `corpus_sha256`,
`corpus_subject`, `tool`, `tool_version`, `binary_sha256`,
`binary_dependencies_sha256`, `extract_reference_sha256`, `scope`, `settings`
and `host`. Hashes must come from the actual reviewed bytes. Dependency hashes
map filenames to SHA-256 values; use an empty object only for a standalone
binary with no separately shipped measurement-relevant dependency. Scope has
an `edition` string or `null` and a sorted, unique `modules` list. The original
result retains its UTC collection date, both directions' raw samples,
archive sizes/hashes, matching reference hash, independent verification and
reviewed resource context. Manual metadata is not proof that measurements
were collected correctly; review that evidence before importing it.
