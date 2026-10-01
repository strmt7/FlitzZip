# Runtime Identity And Decode Allocation Review

This is an uncommitted development investigation, not a release benchmark or
proof of superiority over other archive applications. Public comparison graphs
remain historical until clean-source comparative measurements are reviewed.

## Scope And Controls

Native measurements used the HIP-enabled Release build on a Ryzen 9 9950X and
Radeon RX 9070 XT (`gfx1201`), 10 GiB generated inputs, effort 5, and 8 MiB
blocks. Each profile has three CPU/GPU pairs with alternating lane order and a
250 ms pause outside timing. Archive data remained in RAM and every run reported
zero archive disk writes. The pause does not prove an exclusive or thermally
stable host. Baseline and candidate batches were not interleaved with each
other, so their median differences can include host drift.

The actual signed loaded HIP DLL matters: the installed SDK supplied file
version `10.0.3665.0`, while the driver-preferred runtime supplied
`10.0.3679.0`. No driver or unrelated host process was changed. A bounded
four-worker allocation/copy probe reproduced cross-worker output corruption
with the older SDK runtime despite successful API statuses. The newer runtime
passed the tested probe and production concurrent roundtrips. This does not
establish that every later runtime or device is correct.

The CLI and tests lacked the GUI's Windows compatibility manifest. Adding the
shared manifest changed the CLI's numeric file-version query from `6.2.3679.0`
to the independently observed `10.0.3679.0`. The build now shares that platform
contract; a regression inspects all three embedded executable manifests without
launching the GUI and checks that its DPI declaration remains present.
[Microsoft documents the compatibility manifest contract](https://learn.microsoft.com/en-us/windows/win32/sbscs/application-manifests).

New RAM records include optional `hip_runtime_version` provenance. Graph
validation rejects mixed known/unknown runtime identities and different runtime
versions. Historical records are not rewritten to fabricate this metadata.

## Rejected Device Allocator Candidates

Both experiments used production codec paths, not a benchmark-only decoder:

- Combining payload, output, and descriptors into one allocation passed
  correctness checks but slowed whole-operation extraction. Aligning each
  region to 256 bytes did not recover the regression. Allocation count alone
  is not a valid performance objective; the precise allocator/transfer cause
  remains unresolved.
- Stream-ordered allocation with zero pool retention passed production
  concurrent correctness checks on the newer runtime but was slower than the
  manifest-matched legacy allocator. API tracing confirmed 720 successful
  asynchronous allocations and frees, with no legacy allocation calls in the
  traced operation. Trace timings are not performance measurements.

| Profile | Legacy GPU Extraction Median (s) | Stream-Ordered GPU Extraction Median (s) |
| --- | ---: | ---: |
| Mixed | 2.18328 | 2.49073 |
| Compressible | 1.53835 | 1.85940 |
| Incompressible | 2.43327 | 3.50594 |

These candidates are not shipped. Production still uses legacy HIP allocation
with explicit operation ownership. The diagnostic
`hip_stream_ordered_allocator_supported` reports guarded runtime/device/pool
eligibility, not that an archive operation used that allocator. Private bounded
pool probes failed allocation and were not adopted. Retained-pool strategies
remain a separate, unproven research direction; beta status alone is not a ban.
[AMD documents ordered lifetimes, pool retention, and Windows limitations](https://rocm.docs.amd.com/projects/HIP/en/latest/reference/hip_runtime_api/modules/memory_management/stream_ordered_memory_allocator.html).

## Shared Owned Decode Path

Production extraction and the RAM benchmark previously duplicated output
allocation and zero-initialized every byte before the decoder overwrote it.
They now share `decode_owned_chunk`: bounded block lengths and exact output
extent are checked before allocation, the normal CPU/HIP decoder fills private
owned storage, and CRC is computed only after success. Failed decoding returns
no partial owner. Moved-from owners expose an empty view. Codec formats,
selection policy, and archive bytes are unchanged.

| Profile | Lane | Complete Archive Bytes | Previous Extraction Median (s) | Owned Decode Median (s) |
| --- | --- | ---: | ---: | ---: |
| Mixed | CPU | 4,603,345,552 | 1.33431 | 1.33679 |
| Mixed | GPU | 4,527,858,919 | 2.18328 | 2.03249 |
| Compressible | CPU | 1,074,642,523 | 1.34849 | 1.27334 |
| Compressible | GPU | 1,073,857,115 | 1.53835 | 1.34784 |
| Incompressible | CPU | 10,737,441,371 | 1.37950 | 1.16718 |
| Incompressible | GPU | 10,737,441,371 | 2.43327 | 2.23344 |

The candidate's incompressible GPU extraction samples were 2.57999, 2.23344,
and 2.05950 seconds. That variation prevents treating the median as a stable
gain. CPU Mixed extraction was effectively unchanged. GPU extraction is still
slower than CPU in these profiles; RAM-only results rule out archive SSD writes
as the cause in this lane, not transfer, memory bandwidth, runtime scheduling,
or real filesystem bottlenecks. No compression-ratio improvement is claimed.

The local evidence lives under `out/benchmarks/` in
`manifest-legacy-control-*.json`, `stream-allocator-*.json`, and
`owned-decode-*.json`; these are dirty-source research records, not publication
inputs. CLI SHA-256 identities for this comparison are:

```text
legacy control: 3e425ddad986162cc00da04aebb8e90e9d7373b1683db6fe0c0f9c34b83e39db
stream candidate: a47e935d27be4ad7343f400b0fa872e00b512635dda11b4871401d1a4bae9069
owned decode: 5ba8e0cb9ef3ce595fabfdc2905cba278aa6e582a1b4022deafd71e6b8bd9b3b
```

## Dictionary Event Timing Repair

The bounded 8 MiB GPU filesystem proof exposed an unavailable compression
duration: dictionary work executed, but its separate event markers yielded an
invalid elapsed measurement. The dictionary encoder, compaction, and diagnostic
search now use the same dispatch-bound timing as the other production kernels.
The multi-kernel index interval begins at the key-building dispatch and ends at
the predecessor-linking dispatch, including the intervening ordered radix sort.
This follows [AMD's optional dispatch-event contract](https://rocm.docs.amd.com/projects/HIP/en/latest/doxygen/html/group___execution.html).

Focused tests require finite, nonnegative indexed stage times at efforts 1, 5,
and 9 and independently decode the blocks with LZ4 and HIP. Periodic production
coverage at 1, 8, and 16 MiB also requires finite aggregate kernel time. The
8 MiB compress, compress-and-verify, verify, and extract proof passed, including
exact restore hashes. Its archive remained 4,235,292 bytes before and after the
timing repair. Invalid durations remain unavailable; no clamping, substituted
wall-clock time, or CPU fallback was added. This is a measurement repair, not a
compression-speed claim.

The proof log is `out/gpu-proof-dispatch-bound-timing.log`; the tested CLI
SHA-256 is
`893c64a6e50e49b55906a08920d50ce93fc4457216807f2b5796c22ec5092961`.
The subsequent full local verification is recorded below; this earlier proof
identity is retained rather than attributed to a different executable.

## Follow-Up Correctness And Performance

### Seven-Block RAM Regression Sweep

The post-repair sweep used 10 GiB Mixed input and effort 5 at every supported
block size, with one CPU/GPU pair per size. All 14 records report
`memory_only=true`, `disk_write_bytes=0`, and finite HIP timing in the GPU lane.
With only one pair, CPU preceded GPU at each size; this is a regression sweep,
not a repeated or counterbalanced publication study. Preflight CPU was 1-3%,
system busiest-engine GPU load about 0.77%, available RAM about 30 GiB, paging
zero, and disk activity 0-1%. Postflight was similarly calm. These outside-run
snapshots do not prove exclusive resources or stable thermals throughout.

| Block (KiB) | CPU Archive Bytes | GPU Archive Bytes | CPU Compression (s) | GPU Compression (s) | CPU Extraction (s) | GPU Extraction (s) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 | 4,575,843,733 | 4,572,118,255 | 4.89104 | 3.53405 | 1.27779 | 2.04631 |
| 512 | 4,586,765,248 | 4,567,774,151 | 4.95626 | 3.67562 | 1.31634 | 1.96082 |
| 1,024 | 4,595,283,793 | 4,546,486,431 | 4.93652 | 3.67334 | 1.32917 | 2.04237 |
| 2,048 | 4,599,908,453 | 4,535,840,891 | 4.92373 | 3.69949 | 1.34425 | 2.12136 |
| 4,096 | 4,602,200,797 | 4,530,521,903 | 5.02057 | 3.35301 | 1.29703 | 2.02240 |
| 8,192 | 4,603,345,552 | 4,527,858,919 | 4.94244 | 3.35138 | 1.31067 | 1.85097 |
| 16,384 | 4,603,919,674 | 4,526,531,571 | 4.99973 | 3.35087 | 1.32692 | 1.92792 |

Exact per-phase values, verification time, and telemetry are retained in
`out/benchmarks/dispatch-timing-seven-blocks-serial.json`, from CLI SHA-256
`c92338e5177f491c4cbed5c85bdd6c3423632bbaca659707fffefa4db4610a89` and runtime
`10.0.3679.0`. Compression plus verification plus extraction gave a 1.15-1.29x
GPU/CPU speed ratio in this sweep. GPU extraction remained slower at every
size. No kernel rewrite, before/after speed gain, universal ranking, or
compression-ratio improvement from the event repair is claimed. The generated
Mixed profile selected no dictionary blocks; dictionary correctness and timing
are covered by separate focused production-path tests, not inferred from it.

An earlier sweep was stopped because its resource preflight had not finished
before timing started. That partial attempt is excluded, and its failure log
is retained rather than overwritten. Public graphs remain unchanged.

The Unicode stream audit also reproduced code-page conversion exceptions on
failed output creation and locked-input opening in Gzip, Bzip2, and Zstandard.
These six diagnostic paths now use the existing UTF-8 path helper. The new
creation and Windows exclusive-lock regressions passed after the repair.

The local fuzz audit found a separate harness defect: passing its script as a
Windows native argument stripped the XAR seed's XML attribute quotes. Moving
the unchanged build and smoke sequence into an LF script on the read-only
source mount removes that transport boundary. XML parsing and expected
attribute assertions now precede seed publication. The full verification below
includes the corrected sanitizer smoke; the earlier malformed seed is not
counted as equivalent coverage.

The XAR follow-up found that the parser and its earlier fixture builder both
reversed stored `length` and decoded `size`. A libarchive 3.8.8-created zlib
archive reproduced a range rejection before the fix and restored the exact
source SHA-256 afterward. The parser now uses the standard field meanings,
without accepting a guessed legacy interpretation. Stored and zlib independent
creation/extraction are permanent interoperability checks; reversed extents and
existing malformed, Unicode, overwrite, and corrupt-payload cases remain
covered. [Libarchive's implementation records both field meanings](https://github.com/libarchive/libarchive/blob/master/libarchive/archive_write_set_format_xar.c).

## Completed Local Verification

`tools/verify_changes.ps1 -IncludeUntracked -Full` passed all 22 selected
commands after the XAR extent repair. Evidence is retained in
`out/verification-xar-timing-final-batch.log`. The HIP-enabled Release test
harness passed 517 tests with zero failures. The batch also passed changed-file
hygiene, language lint, changed-function contracts, verifier regressions,
repository policy checks, automated GUI smoke, brand checks, independent
interoperability, the 36-format matrix, bounded sanitizer/fuzzer smoke, MSI
identity checks, portable packaging, and MCP child-containment regressions.

The regenerated 181-byte XAR fuzz seed was separately parsed as XML and
extracted with the production CLI. Its stored length was 18 bytes and decoded
size was 10 bytes; the restored payload was exactly `hello xar\n`. This checks
both corrected script transport and standard extent interpretation. The final
CLI SHA-256 matched the seven-block RAM sweep identity above.

These gates cover tested behavior, not every possible input, installation,
runtime, or hardware configuration. The local policy scan is not a completed
Codex Security vulnerability scan. MSI identity and portable packaging are not
install/repair/uninstall acceptance or release publication.

## Remaining Acceptance Gates

| Domain | Current Evidence | Still Required |
| --- | --- | --- |
| Standard CPU formats | Shared stream contracts, 517-test suite, 36-format routing, independent writer interoperability and XAR producer checks | Remaining adapter review and repeated software-to-software throughput/size comparisons; supported subsets are not full format coverage |
| Native CPU/GPU | Shared production decode, focused dictionary tests, finite dispatch timing, seven-block RAM sweep | Phase-level extraction bottleneck analysis, held-out workloads, repeatable before/after controls, broader supported-device validation |
| GUI and operations | Automated all-page/action smoke, settings persistence, queue and telemetry regressions | Complete visual review and remaining cancellation, metadata, source-deletion, and operation-policy review |
| Source structure | Changed-function contracts, scoped ownership fixes and clone inventory | Manual triage of remaining clone candidates and untouched production functions; an inventory is not line-by-line review |
| Hosted analysis | Fresh API inventory: 206 open records, comprising CodeQL 183, Grype 10, OSV 10 and Scorecard 3 | Evidence-backed remediation/triage, scanner dependency resolution, exact-SHA workflow completion and post-push audit |
| Release | HIP-enabled local build, deterministic MSI identity and checksummed portable package | Final end-user documents/graphs, clean-source comparison results, installer lifecycle, hosted gates and maintainer approval before publication |

The extraction review confirms synchronous host/device transfers, per-call
device allocation, and materialization in the production path. These are
candidates for tracing, not measured explanations of the complete regression.
Prior allocator experiments already regressed, so another unmeasured allocator
rewrite is not justified. Follow
[AMD's profile-first optimization workflow](https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/performance_guidelines.html)
and separate allocation, copies, kernels, synchronization, and CPU checksum
cost before choosing the next change. Windows tool availability must be
verified rather than assuming Linux profiling commands work on this host.

Reproduce with `tools/bench.ps1 -Configuration Release -SizeMiB 10240
-Profile Mixed -CompressionLevel 5 -Iterations 3 -BlockSizeKiB 8192`, using a new
`-JsonOutput` path. Repeat for Compressible and Incompressible. The required
seven-block-size sweep and broad local verifier passed for this patch set.
Final security review, remote workflow audit, cross-application measurements,
and release acceptance remain distinct open gates, not consequences of these
diagnostic numbers.
