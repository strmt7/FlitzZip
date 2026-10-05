# Neutron Star Mode Validation

## Measured Improvement Rounds

These bounded research fixtures establish correctness and specific size
improvements. They do not establish representative performance, a GPU speedup,
or best-in-class compression. Neutron remains a separate native mode; numeric
efforts retain their existing algorithms and policies.

Round one introduced minimum-byte parsing over the admitted dictionary match
graph. Independent CPU transition enumeration and independent LZ4/HIP readback
covered short literal runs, overlaps, extension costs and segment bounds.

Round two integrated the stronger trial into native archive selection, retained
every existing GPU candidate and used complete framed block costs. Its native
input identity was
`5a7ece06c0f25bc1e93fe487952f2da8e00047c869a5015d2f528382b932b9a6`.

Round three expanded only Neutron's match length to 65,535 bytes and covered all
extension-cost plateaus, including those beyond the 256 GPU lanes. Its initial
native input identity was
`dc8ef0f3a6fb87077f66e527a1f4a285e81a966ba67e207d38b7fc178892ab34`.
The focused suite passed thirteen contracts, including a closed-form minimum
for a complete uniform 64 KiB segment, the independent small-input oracle,
complete archive CPU/HIP extraction, observer serialization, cancellation and
destination preservation.

The standalone 1 MiB periodic primitive fell from 5,344 bytes in round two to
5,280 in round three; the ordinary standalone level-nine primitive used 5,712.
These are primitive payload sizes. Existing product periodic candidates can
already win, so these differences do not imply the same product archive saving.

## Complete Archive Comparison

The native corpus benchmark authenticated a preloaded source snapshot and
performed bytewise readback. Every observation reported `memory_only=true`,
`disk_write_bytes=0` and complete validated input coverage. All lanes used
256 KiB archive blocks, one worker, one encode slot and one decode slot.
Neutron and ordinary GPU lanes reported actual HIP execution. The CPU lane was
an independent ordinary level-nine reference, not a Neutron fallback.

| Fixture | Input bytes | CPU 9 archive | GPU 9 archive | Neutron round two | Neutron round three |
| --- | ---: | ---: | ---: | ---: | ---: |
| Isolated short repeats | 600 | 709 | 709 | 692 | 692 |
| Long repeat of a 30,000-byte prefix | 65,536 | 30,132 | 30,387 | 30,387 | 30,384 |
| Random control | 65,536 | 65,645 | 65,645 | 65,645 | 65,645 |

The source identities, in table order, are
`31dae87609124153d6ceb074448f0a023a3c6902d1ee4b44eec1fa0b4584d901`,
`d28c2d3aa3d8342587533568ee050b5d445629778a79b560c27bcdfabd5222ff`
and `96b62d606cbcc5c4b851e31a942d961dfb219971fbb70c768c29c43039d389be`.
Data came from deterministic xorshift32 fixtures, not private user files. Each
byte applies shifts 13 left, 17 right and 5 left with 32-bit masking, then takes
the high byte. Seeds are 713947, 71937 and 714239 respectively. The first fixture
copies bytes 4..19 to 230..245 and 32..51 to 512..531. The second repeats its
30,000 generated prefix bytes and truncates at 65,536. The third is unmodified.
Timings are single diagnostic observations with startup and
hardware scheduling variation; no timing superiority is inferred. In
particular, the CPU reference remains smaller on the long-repeat fixture.

The isolated-repeat test demonstrates a complete archive improvement missed
by ordinary dictionary sampling. The long-repeat test demonstrates a further
incremental improvement from longer matches. The random control demonstrates
unchanged size for this input. These examples are not a broad corpus study.

## Qualification Boundary

The final incremental HIP build binds native inputs
`2dcac6ebdc40ccad6a7e1b4b2491c88cbb9ac5698023c89a373593837bb97f24`
to successful receipt
`985384d9ef9351b9a11fe803a36d62427bb4357a5982791082a505350bd9caf8`.
It follows formatting changes to two GUI helpers. Its CLI remains byte-identical
to the measured round-three CLI:
`70b9f4fce55a5f7d87908bfabe7eb9aaa863a8c712eeb06652415a5e77aec113`.

The selected eighteen local checks completed: changed-file hygiene, source
preflight, language lint, HIP Release build, the native driver, function
contracts, repository security checks, GUI smoke, independent compatibility
and format consumers, short fuzzing, MSI identity, packaging, isolated
sanitizers, rewrite policy, native selection, precompiled headers and benchmark
reporting. The seven CTest suites passed; after repairing the planning test's
field-order assumption, its remaining corpus/controller and version consumers
completed without repeating those suites. All-page and compact screenshots,
including the separate Neutron row, were manually reviewed. The isolated MSVC
AddressSanitizer lane passed six tests and bound to the same native inputs; it
is dependency validation rather than a HIP device sanitizer.

Completed checks are reused only for unchanged
inputs and their recorded scope. Final hosted results must be bound to the
pushed commit. Publication and unmeasured ratio or performance claims remain
outside this validation record.
