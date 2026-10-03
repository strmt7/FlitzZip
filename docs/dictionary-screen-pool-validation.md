# Operation-Owned Dictionary Screening Scratch

The canonical dictionary candidate filter now uses a C++20 polymorphic allocator
pool owned by one function invocation. Its set borrows twelve-byte source views;
the set is destroyed before its pool. Segment transitions recycle nodes rather
than repeatedly allocating them. The initial bucket reservation is bounded by
the maximum sampled keys in one independent 64 KiB segment.

Sampling positions, source-key equality, segment boundaries, the eight-repeat
threshold and the separate periodic-distance fallback are preserved. The change
does not alter dictionary search, HIP kernels, effort policy or archive framing.
There is no global cache, shared pool, new runtime dependency or retained scratch
after a return or exception.

## Isolated Timing Evidence

The baseline source is `db3e8d59c780971a53f181bfb37872e5eafb5f50`.
A Release C++20 probe compiled with MSVC 19.51.36260.0 compares its exact filter
with a reservation-only control and the pooled candidate. After ignoring comments
and whitespace, the production function matches that measured candidate.

All seven production block sizes, 256 through 16384 KiB, are exercised with
deterministically generated random bytes, uniform bytes and repeated sampled
anchors. These are screening fixtures, not representative real-world corpora.
All variants agree on their Boolean results. Payloads remain in RAM and there
are no product payload writes; the journal stores measurement metadata only.

Calibration is retained separately. Its iteration count doubles until the
slowest variant reaches 20 ms or the calibration cap. The confirmation count
is then frozen to target 20 ms for the fastest calibrated variant. Each of the
21 cases has seven fixed confirmation rounds, rotating the order of all three
variants. All 272 calibration rows and 147 confirmation rows are retained;
each confirmation row contains three timings. No observations are trimmed,
discarded as outliers or extended because a result is favorable.

Baseline mean time divided by pooled mean time ranges from 1.55 to 6.54 across
the 21 cases. The reservation-only control regresses on random inputs and was
rejected. Timed processes use normal priority. This is a single-host isolated
filter result, not an archive throughput, GPU speed, compression-ratio or
competitor claim. Whole-operation qualification and representative corpus
comparisons remain outstanding.

## Allocation And Failure Evidence

A separate, untimed upstream allocation counter compares the pooled candidate
with a direct-allocation PMR set proxy for the baseline container. The proxy
does not intercept the original standard allocator; its byte counts must not
be presented as measured original-container heap usage.

The resource study covers 18 sizes, including empty input, twelve-byte key
boundaries, 4096-byte admission boundaries, 64 KiB segment boundaries and all
seven production block sizes, with three shapes each. All 54 cases agree on
results. Every candidate upstream allocation point is separately failed with
`std::bad_alloc`; selected first/middle/last baseline-proxy failures are also
checked. All 512 injected failures clean up, and all 108 normal variant rows
report zero live upstream bytes after return.

For random production-sized inputs, measured upstream requests are:

| Block KiB | Baseline proxy allocation calls | Pooled calls | Baseline proxy peak bytes | Pooled peak bytes |
| --- | --- | --- | --- | --- |
| 256 | 4099 | 14 | 98336 | 263672 |
| 512 | 4099 | 13 | 81952 | 132544 |
| 1024 | 4099 | 12 | 73760 | 66952 |
| 2048 | 4099 | 11 | 69664 | 34128 |
| 4096 | 4099 | 10 | 67616 | 17688 |
| 8192 | 4099 | 9 | 66592 | 9440 |
| 16384 | 4099 | 8 | 66080 | 5288 |

The default standard-library pool trades additional peak scratch at the two
smallest random block sizes for fewer allocator calls. The maximum observed
candidate request peak is 263672 bytes per invocation; concurrent invocations
multiply scratch use. These are upstream requested-byte counts, excluding heap
metadata, and are not a portable allocator-layout guarantee or a process memory
measurement. Pool growth and rounding depend on the standard library. Source
sampling and simultaneously live keys remain bounded independently of input
size; every invocation releases its pool.

## Retained Evidence

Private evidence remains under ignored `out/`:
`dictionary-screen-summary-20261003.json`,
`dictionary-screen-confirmation-journal-20261003.jsonl`,
`dictionary-screen-resource-journal-20261003.jsonl` and
`dictionary-screen-resource-proof-20261003.json`.
The timing journal SHA-256 is
`5702ca934fced1d73c0e56057af176a92bec43449e8f1f4046e79762b944f864`;
the allocation journal SHA-256 is
`cbe22b57aee05b70f74a58c385ae473931099c96041929c9f23c713824bae822`.
These local studies do not replace required-HIP production integration tests,
hosted qualification or the held release's broader acceptance gates.
