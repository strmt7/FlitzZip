# Mixed GPU Materialization

The generic HIP materializer expands raw, fill, pattern and sparse-pattern
blocks. Separate kernels decode prefix, Huffman and dictionary blocks. Previously,
the generic kernel walked those coded windows byte by byte without writing them.
It now advances each lane past the window in one step, retaining its stride.
An already passed block boundary is checked before subtraction: one lane can
cross several blocks shorter than its stride. Host framing validation, dedicated
decoders, format bytes, allocation admission and publication remain unchanged.

## Production Checks

The HIP Release build passes 12 affected tests. A new independent static/adaptive
prefix fixture combines coded windows crossing 64 KiB segments with raw, fill
and pattern blocks. Leading extents of 1, 255, 256, 257 and 65,535 bytes and several
sub-stride blocks exercise unaligned transitions. CPU/HIP bytes and device CRC
match. Existing mixed prefix/Huffman/dictionary/sparse cases, malformed frames,
pattern boundaries and empty-job contracts also pass. Changed-file lint,
function contracts and repository security policy pass.

Under the maintainer's component-only direction, that cohort replaces the blanket
native driver. Unchanged GUI, package and compatibility-adapter checks are not
repeated. The initial local lint failure was fixture formatting and was corrected
before the successful build and tests; its evidence remains separate.

## Isolated Kernel Observation

One probe binary contains the exact old/new kernel bodies and their matching
production structures, constants and lookup helpers. It uses the shared ROCm 10
compiler path and Release HIP flags, targeting the exercised `gfx1201` device.
The GPU is an RX 9070 XT; this is not qualification on other devices.

The generated fixture describes 128 MiB minus 1,357 bytes at each production
block size. Two static-prefix blocks alternate with raw and fill blocks. Prefix
frames are independently constructed valid zero-symbol streams; the production
host validator admits the full layout. Raw/fill output and untouched coded
regions are byte-checked for both variants before and after measurement.
This legal framing fixture is not representative compression-policy selection:
the production writer would normally choose fill for a constant decoded block.

Allocations and payload uploads occur outside timing. Three warmup batches per
variant precede 21 fixed alternating pairs per case. Each batch launches 81 walks,
covering at least 10 GiB of logical output; buffers are reused. HIP stream events
measure the batch, including possible host-submission gaps. Values below normalize
that interval per launch. They are not standalone kernel execution times.

| Block KiB | Before ms | After ms | Before / after | Before CV % | After CV % |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 256 | 0.508 | 0.425 | 1.196 | 0.89 | 1.98 |
| 512 | 0.497 | 0.411 | 1.207 | 0.32 | 2.82 |
| 1024 | 0.486 | 0.404 | 1.204 | 0.15 | 3.28 |
| 2048 | 0.453 | 0.381 | 1.189 | 0.23 | 3.54 |
| 4096 | 0.449 | 0.388 | 1.155 | 0.15 | 3.48 |
| 8192 | 0.452 | 0.394 | 1.146 | 0.49 | 3.34 |
| 16384 | 0.430 | 0.380 | 1.131 | 0.19 | 2.07 |

All 294 measured records and 42 warmups are retained; no outliers are removed.
Shared production statistics match independent recomputation of all 14
distributions. Every paired interval favors the candidate in this run. These
are descriptive observations with repeated buffers, cached metadata, short
intervals and within-process dependence, not confidence or superiority claims.
The probe does not decode coded payloads or measure complete archive throughput.

The first private launcher failed to observe the child's exit code after all
planned records appeared. Its series remains diagnostic. The corrected launcher
uses an owned process handle and establishes exit zero for the separate frozen
series. A private link-runtime mismatch warning was resolved before any timing.

## Whole-Operation Diagnostic

Two sequential GPU-only Mixed sweeps retain three observations at each of the
seven block sizes: 42 observations byte-check 420 GiB at level five, with zero
payload disk writes. Exact archive sizes, encode/decode worker geometry, kernel
counts, transfers, device allocation totals and block-kind counts match across
versions. The baseline is clean source; the candidate records its dirty native
source and distinct successful build receipt. Each series has stable identity.

Verify/extract means move in both directions. Every case remains inconclusive
under the controller's duration/variability/precision diagnostics. Separate
build periods also confound attribution. No whole-archive speedup follows from
the isolated result. CPU throughput, other profiles, real-corpus comparisons
and release performance qualification remain open.

Ignored evidence includes:

- `out/materialize-skip-baseline-mixed-20261003.json` and its raw journal
- `out/materialize-skip-candidate-mixed-20261003.json` and its raw journal
- `out/materialize-skip-independent-comparison-20261003.json`
- `out/materialize-skip-formatted-component-verification-20261003.json`
- `out/materialize-kernel-frozen-timings-20261003.jsonl`
- `out/materialize-kernel-frozen-statistics-20261003.json`
- `out/materialize-kernel-frozen-shared-statistics-20261003.json`
- `out/materialize-kernel-frozen-run-20261003.json`

The isolated source, executable hashes and host counter journals are retained
alongside those records. Reproduction requires the exact extracted bodies,
matching HIP flags and structures, fixture and fixed ordering above. Neither
the probe nor its private launcher is a product runtime dependency.
