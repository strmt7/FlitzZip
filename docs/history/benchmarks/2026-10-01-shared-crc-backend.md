# Shared CRC Backend And Dictionary Admission Review

This is a development checkpoint, not a release benchmark or a claim of
complete repository acceptance. Measurements used uncommitted development
source on `main`. Public comparison graphs are intentionally unchanged until clean-source
results and the remaining acceptance gates are available.

## Root Causes And Changes

The native CPU checksum previously advanced one byte at a time through a
256-entry IEEE CRC table. Native compression, verification, CPU extraction,
and other readers using the shared checksum paid that cost. The existing,
pinned LZMA SDK already supplies an optimized IEEE CRC backend, so the shared
wrapper now uses `CrcUpdate` without adding a dependency or changing the
polynomial, finalized seed convention, serialized checksum, or wire format.

The SDK's table generation mutates global state. Native checksums and the 7z
reader now enter the same thread-safe, one-time initialization guard rather
than maintaining independent initialization paths. Empty input preserves the
caller seed without passing a null range into the SDK. Standalone sanitizer
targets link the checksum implementation and its transitive SDK objects.
The SDK defaults to twelve lookup tables on the existing little-endian x64
build; its architecture dispatch is not an x86 CRC32C substitution.
[Upstream implementation](https://github.com/ip7z/7zip/blob/main/C/7zCrc.c).

An independent archive smoke exposed a second, older problem. GPU effort 6
selected a smaller entropy result and thereby crossed an admission cutoff:
efforts below 7 skipped dictionary search when entropy had already halved the
input size. Dictionary compression would have saved substantially more. The
cutoff is removed; bounded source-repetition and periodic-distance screens
remain. Candidate publication still compares complete encoded sizes,
including segment tables, and retains the existing representation on ties.
No padding, artificial delay, CPU codec substitution, or benchmark-only
encoder branch was introduced.

## Controls And Method

The CRC-only comparison used three paired rounds for each of Mixed,
Compressible, and Incompressible in both CPU and GPU modes: 36 runs total.
Both build order and CPU/GPU order alternate across rounds. Each run processes
10 GiB entirely through the production memory benchmark at effort 5, 8 MiB
blocks, 32 requested workers, and 32 in-flight chunks. The recorded 250 ms
pause is outside child phase timers. All runs require reconstruction CRC
validation, `memory_only=true`, zero archive disk writes, and real HIP telemetry
for GPU. The RAM benchmark does not compare every restored byte with a retained
10 GiB source; separate bounded archive fixtures check complete byte equality.

The reference platform was Ryzen 9 9950X, Radeon RX 9070 XT (`gfx1201`), and
loaded HIP runtime file version `10.0.3679.0`. This is one platform, not
multi-device or installation acceptance. Build identities are:

| Build | CLI SHA-256 |
| --- | --- |
| Original byte-table control | `c92338e5177f491c4cbed5c85bdd6c3423632bbaca659707fffefa4db4610a89` |
| Shared SDK CRC, before admission repair | `c1724aa54534ba1001a7aeaea285b0232d4d45f10ce11a916c0d0f561616d6c4` |
| Shared SDK CRC plus admission repair | `12d42ef5104d5b07d25b7e9495fc7b5d532926d6ddb7f6fd49ba46166581b81d` |

The ignored diagnostic driver imports the existing `tools/bench.ps1` functions;
it does not implement a different codec or bypass benchmark validation.
Evidence remains in
`out/benchmarks/crc-sdk-paired-2026-09-30T220319-7069728Z-n36.json`.
These dirty-build controls identify the experiment, but are not eligible for
published cross-application rankings.

## CRC-Only Phase Results

Values below are median seconds across three runs. Paired extraction speedup
is control time divided by candidate time for each matched round, not a ratio
selected from unrelated fastest results. Archive size was identical between
builds in every matched case.

| Profile | Mode | Archive Bytes | Compress Before / After | Verify Before / After | Extract Before / After | Paired Extraction Speedup |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Mixed | CPU | 4,603,345,552 | 5.16402 / 4.75094 | 1.39083 / 0.962595 | 1.34757 / 0.934342 | 1.37-1.50x |
| Mixed | GPU | 4,527,858,919 | 3.37182 / 3.37810 | 0.750868 / 0.711265 | 2.02763 / 1.50695 | 1.32-1.41x |
| Compressible | CPU | 1,074,642,523 | 2.25297 / 1.86959 | 1.34057 / 0.969229 | 1.31777 / 0.973719 | 1.34-1.40x |
| Compressible | GPU | 1,073,857,115 | 1.81081 / 1.88409 | 0.169360 / 0.164559 | 1.39903 / 0.956235 | 1.46-1.52x |
| Incompressible | CPU | 10,737,441,371 | 3.72853 / 3.36879 | 1.37090 / 0.991227 | 1.20495 / 0.821427 | 1.45-1.47x |
| Incompressible | GPU | 10,737,441,371 | 2.67263 / 2.49014 | 0.693959 / 0.743141 | 2.17684 / 1.71202 | 1.26-1.29x |

This is a repeatable local extraction improvement, not an order-of-magnitude
GPU claim. GPU extraction still trails CPU on all three profiles. GPU
Compressible compression was 4.0% slower by median, and GPU Incompressible
verification was 7.1% slower. Those phases are retained rather than removed
from the report; the experiment does not establish that the checksum change
caused every timing difference. Compatibility formats with an independent
checksum implementation, including miniz-backed ZIP/Gzip, are not inferred
to have the same speedup.

The seven-block regression sweep in
`out/benchmarks/crc-sdk-seven-blocks-serial.json` covered 256, 512, 1,024,
2,048, 4,096, 8,192, and 16,384 KiB. All fourteen CPU/GPU cases preserved
the prior encoded sizes, passed reconstruction CRC checks, and recorded zero
archive disk writes. It was one pair per block size, CPU first, so it is not an independent
repeated speedup study.

## Host Context And Measurement Limits

The paired CRC experiment recorded 37 sparse observations during workloads.
Total CPU peaked at 70.58%, including SuperZip; available RAM stayed at or
above 26,460 MiB. The busiest summed competing physical GPU engine peaked at
34.74%; recorded disk busy time peaked at 9.59%. During-workload paging peaked
at 31.80 pages/s. One after-workload sample reached 1,080.55 pages/s with
31,538 MiB still available. It is not evidence of sustained paging exhaustion,
but remains in the raw record. Samples do not establish exclusive hardware,
thermal stability, or absence of short interference. Process I/O counters
were unavailable; the production benchmark separately asserted zero archive
disk writes. There was no common monotonic phase slowdown across rounds.

The admission comparison's supplemental Windows counter query initially
failed with an invalid performance-counter sample. Six completed runs remain
intact. The observer now inspects individual status values, records unavailable
samples and query errors, and still rejects missing required CPU/RAM/disk
measurements. A resumed record preserves the first six cases rather than
rerunning them. This is a diagnostic collector correction, not a relaxed
production correctness or scanner gate.

The resulting twelve-case admission comparison is retained in
`out/benchmarks/dictionary-admission-resumed-2026-09-30T223546-2061961Z-n12.json`.
All three profile/mode pairs retained the control archive sizes; these
generated profiles selected no dictionary blocks. One pair per case is a
regression diagnostic, not a speedup claim. A separate fourteen-case sweep in
`out/benchmarks/dictionary-admission-seven-blocks-serial.json` covered every
production block size with the repaired build, reconstruction CRC validation,
identical control sizes, finite HIP timing, and zero archive disk writes.

## HIP Trace Interpretation

The offline analyzer `tools/analyze_hip_trace.py` validates bounded level-four
HIP logs, pairs calls per thread, rejects incomplete or reversed records,
and reports thread-union API occupancy without double-counting nested calls.
It removes process/thread identities, addresses, and source paths from the
summary. Eight regressions cover nesting, transfers, bounds, malformed logs,
UTF-8 failures, CLI privacy, and time-window selection.

The instrumented 10 GiB Mixed trace contained 7,120 complete calls and no
failed returns. Selected extraction windows contained 1,640 calls across
four workers: 4,559,334,028 H2D bytes and 10,740,039,680 D2H bytes, including
metadata. Summed copy occupancy was 2,807,357 worker-microseconds,
event synchronization 1,274,767, frees 353,483, and allocations 13,167.
Worker sums are not phase wall time. Windows are interleaved with compression
and verification; their gapped span must not be labeled extraction duration.
Tracing itself changes timing and is disabled during throughput comparisons.

The trace supports further transfer/synchronization investigation, not another
unmeasured allocator rewrite. AMD documents that pageable host buffers can
make nominally asynchronous copies synchronous; bounded pinned staging is a
candidate requiring its own correctness, lifetime, RAM-budget, portability,
and paired performance evidence before adoption.
[HIP memory API](https://rocm.docs.amd.com/projects/HIP/en/latest/doxygen/html/group___memory.html),
[performance guidance](https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/performance_guidelines.html).

### Transfer Hypothesis Follow-Up

Two standalone, byte-checked probes tested the transfer hypothesis without
modifying the production allocator. Each case used four independent streams,
128 MiB regions, twenty cycles per worker, and exactly 10 GiB in each copy
direction. Every cycle compared all recovered bytes, not only the final
result. Mode order reversed on the second round. These are warm-buffer HIP
API diagnostics, not compression, corpus, or application benchmarks.

The reusable-buffer probe compared pageable memory, registration once, pinned
allocation, and registration per cycle. Eight cases passed. D2H worker sums
were 1.66209/1.74284 seconds for pageable buffers, 0.47194/0.51899 for
once-registered buffers, and 0.51846/0.50764 for pinned allocations. Faster
copies alone did not account for registration or ownership costs.

The second probe allocated and released the output for every cycle, matching
the existing owned decoder's lifetime more closely. Its six cases passed:

| Output Strategy | Round 1 Wall Seconds | Round 2 Wall Seconds |
| --- | ---: | ---: |
| Pageable per cycle | 1.898747 | 1.658013 |
| Registered per cycle | 1.406681 | 1.391103 |
| Pinned allocation per cycle | 0.998297 | 0.944583 |

Wall values include setup, transfers, full byte comparison, allocation and
cleanup. The pinned candidate was 1.76-1.90x faster in this diagnostic, not
in SuperZip extraction. Registration per output cost 2.06723/2.07712 summed
worker-seconds; pinned output allocation/release cost 0.55600/0.35448. This
supports evaluating owned pinned output rather than simply registering every
existing span. It does not justify assuming persistent pools are free or
changing all transfer paths at once.

Evidence is retained in `out/research/host-transfer-20261001-005946.json` and
`out/research/host-transfer-20261001-010553.json`; the original reused-buffer
driver is preserved separately. Maximum pinned/registered budget was 1 GiB
and device buffers totaled 512 MiB. Preflight and before/after observations
had at least 27,969 MiB available RAM, total CPU at most 11.58%, and competing
GPU at most 0.79%, with no invalid counter samples. These are not continuous
during-copy observations or proof of exclusive hardware.

No pinned production allocation is shipped by this batch. The next candidate
must preserve the shared production/RAM `decode_owned_chunk` path, explicit
deallocation ownership, fail-before-publication behavior, and CPU/required-HIP
semantics. It needs a host-relative aggregate pinned-RAM cap, failure fallback
for allocation only, move/lifetime/concurrency regressions, and matched
three-profile/seven-block measurements. A raw transfer win is not enough to
skip those gates. The subsequent [bounded pinned-output checkpoint](2026-10-01-pinned-output-pool.md)
records the production candidate, unsuccessful intermediate results, shared
checksum-worker fix, and separate acceptance evidence. This section retains the
earlier CRC-only batch's historical status.

## Effort-Level Reproduction

The bounded 2 MiB plus 13-byte fixture combines zeroes, a repeated phrase,
a repeated 4 KiB low-alphabet motif, and pseudorandom tail bytes. The CRC-only
change first produced byte-identical archives at all nine efforts in both
modes, with 36 exact extractions. After the separate admission repair, all
eighteen candidate archives passed read-back verification and exact extraction;
CPU archives remained byte-identical to the control.

| GPU Effort | Before Bytes | After Bytes |
| ---: | ---: | ---: |
| 1 | 592,707 | 592,707 |
| 2 | 583,362 | 583,362 |
| 3 | 579,474 | 579,474 |
| 4 | 574,554 | 574,554 |
| 5 | 571,626 | 571,626 |
| 6 | 981,163 | 568,642 |
| 7 | 567,154 | 567,154 |
| 8 | 566,618 | 566,618 |
| 9 | 566,370 | 566,370 |

The new production-path regression failed before the fix and passes afterward.
It checks all nine GPU-native dictionary choices, non-growing complete payload
sizes, CPU/HIP reconstruction, and actual kernel launches. These are
fixture-specific results; dictionary compression is not guaranteed to improve
every input. The same archive smoke records native CPU sizes of 526,720 bytes
at effort 2 and 526,849 at efforts 3/4. The CPU Zstandard selector does not
retain lower-effort candidates; that observed 129-byte growth is not fixed by
the GPU admission repair. Equal or larger outputs must not be hidden by
rounded ratios or manufactured effort differences.

## Acceptance Status

The combined batch passed `tools/verify_changes.ps1 -IncludeUntracked -Full`:
all 22 selected commands, including 521 C++ tests, eight offline trace tests,
changed-function/language checks, policy and verifier regressions, automated
all-page GUI smoke, independent interoperability, all 36 registered format
routes, fourteen bounded sanitizer/fuzzer targets, MSI identity, portable
packaging, and MCP child-containment checks. Evidence is retained in
`out/verification-shared-crc-dictionary-final-batch.log`. The eight compact page
screenshots, effort menu, and overwrite modal were also inspected; this does
not establish full visual acceptance for every recorded state or display.

The refreshed hosted audit retrieved 1,678 incident records: 206 open, 859
fixed, and 613 dismissed. Open records comprise CodeQL 183, Grype 10, OSV 10,
and Scorecard 3. It correctly failed on 204 unapproved open records. Closure
history is not remediation evidence. PyPI metadata still lists Semgrep
1.178.0 as latest and its requirement is `pyjwt[crypto]~=2.13.0`; latest PyJWT
is 2.15.1. The current development manifest retains the same constraint, so
adopting development source does not by itself resolve this dependency gate.
[Semgrep distribution](https://pypi.org/project/semgrep/),
[development manifest](https://github.com/semgrep/semgrep/blob/develop/cli/pyproject.toml).

Remaining gates include complete visual/operation review, standard-format
competitor measurements, held-out GPU workloads,
hosted finding triage and exact-SHA workflow completion, installer lifecycle,
final user documents/graphs, and maintainer approval before release. Neither
this report nor a local policy scan proves zero vulnerabilities or that every
repository function has been fully audited.
