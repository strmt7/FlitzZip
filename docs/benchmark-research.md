# Benchmark Research And Adoption Decisions

Reviewed 1 October 2026. This research informs the
[measurement protocol](comparative-benchmark-methodology.md), not a claim
that SuperZip has already passed it. Licenses are evaluated separately in
[the permission matrix](benchmark-permissions.md). A reviewer's use of a
tool or dataset does not grant this project the same rights.

## Application Reviews

| Primary source | What was actually disclosed | What we adopt |
| --- | --- | --- |
| [Tom's Hardware, archive-tool comparison (2013)](https://www.tomshardware.com/reviews/winrar-winzip-7-zip-magicrar,3436-6.html) | Hardware/software table, mixed-file workload and exact switches; separate ZIP and proprietary-format result pages. | Disclose the machine and commands; keep common-format interoperability and native-format product tradeoffs in separate panels. Do not copy its corpus, software entitlements or old rankings. |
| [AnandTech, encoding tests (2020)](https://www.anandtech.com/show/16195/a-broadwell-retrospective-review-in-2020-is-edram-still-worth-it/7) | Repeated CLI 7-Zip built-in scores; a separate 3,477-file, 1.96 GB application workload, with a steady-state cache procedure and last-five-run average. | Separate built-in hardware scores from application timings and describe cache state. We do not adopt its 20-minute procedure blindly, copy media/game assets or select only favorable samples. |
| [PCWorld, 7-Zip review (2011)](https://www.pcworld.com/article/481550/free_utility_7_zip_compresses_files_efficiently.html) | Compressible documents/bitmap files contrasted with already-compressed images. | Include incompressible controls and explain container overhead. Historical small workloads do not establish sustained speed on current hardware. |
| [PeaZip author's application comparison](https://peazip.github.io/peazip-compression-benchmark.html) | A roughly 1.3 GB, 43-file workload, five-run averages, shared storage context, size and create/extract time, and different application defaults. | Compare achieved bytes against time rather than pretending equal numeric settings mean equal effort. This is a vendor-authored study, not an independent magazine ranking. Its software versions and corpus rights do not carry over to our runs. |

These sources support more than one measurement boundary; there is no single
universal magazine protocol. The accessible Tom's Hardware chart pages also
separate time and compressed percentage. We will publish original numeric
charts with readable units and dispersion, not reproduce those figures or
borrow their measurements.

## Scientific Work

[FCBench, VLDB 2024](https://arxiv.org/html/2312.10301v2), sections 5 and 6,
evaluates CPU/GPU lossless floating-point compressors on 33 datasets. It
reports ratio, compression/decompression throughput and end-to-end time,
including memory-copy effects. Its codec measurements repeat ten times and
exclude file I/O. Roofline analysis separates memory bandwidth from compute
limits. Adopt explicit timer boundaries and transfer accounting; do not label
its CUDA or floating-point-only results as SuperZip archive performance.
The paper's license does not establish the rights of every referenced dataset.

[PETRA III lossless compression study (2026 preprint)](https://arxiv.org/html/2608.00168v1),
sections 4 and 5, separates memory-backed and filesystem results and explores
size/throughput Pareto fronts by file category. It tests cache/NUMA contention
and displays percentile ranges. Some experiments use at least eight samples;
its cache-scaling study uses at least 64. Its underlying user data are not
publicly available. Adopt boundary disclosure, per-category tradeoffs and
contention diagnostics, not its private data, large parameter sweep or
server-specific thread policy. The paper is not evidence of a SuperZip win.

Neither source establishes that all effort levels must produce distinct
sizes, or that every GPU codec beats a CPU codec. Improvements require exact
round trips, complete archive sizes and measured end-to-end costs. A stronger
search finding no smaller representation is not an excuse to add padding,
artificial delays or a benchmark-only path.

## Tool Selection

Use several independent tools, but do not combine incompatible units:

- **Application comparisons:** 7-Zip, Bandizip Standard, PeaZip and Zstd CLI,
  limited to reviewed versions, official packages and supported formats.
  Bandizip and PeaZip still need validated application adapters; calling
  their backend directly is not an application measurement.
- **Hyperfine:** an independent whole-command timing cross-check, with
  [raw JSON, warmups and cache policy](https://github.com/sharkdp/hyperfine/blob/v1.20.0/README.md).
  Disclose shell use; its no-shell option and shell-calibrated default are
  different boundaries. Do not pool its samples with the primary harness.
- **lzbench:** [memory-only codec context](https://github.com/inikep/lzbench),
  only the individually reviewed LZ4/Zstd modules. It does not exercise
  SuperZip's archive container or HIP path.
- **7-Zip built-in benchmark:** widely used CPU/codec context, as documented
  by AnandTech above and [the benchmark author](https://www.7-cpu.com/).
  Its ratings are not application elapsed seconds or ZIP archive sizes.

WinZip is excluded under the reviewed benchmark restriction. WinRAR's trial
is not part of the free-tool plan. NanaZip's package-level review is incomplete;
PeaZip supplies the additional open-source application for this round.
No subscription, trial renewal or paid proxy is required by this plan.

## Workloads And Publication

Silesia is historically established, but age and popularity do not establish
representativeness or blanket rights for mixed component files. It is excluded
from new runs and headline claims. UCI
[HIGGS](https://archive.ics.uci.edu/dataset/280/higgs) has an explicit CC-BY-4.0
license and a large numeric workload. Attribute its creator, DOI and any
transformation. It does not replace a mixed-file or small-file corpus by itself.
Additional source/document/media workloads need constituent-level provenance;
do not borrow a magazine's payload or call synthetic repetition real data.

The final pre-release refresh must use at least five independent samples per
point and direction. Short operations remain useful diagnostics, but headline
sustained-speed results must meet the protocol's declared duration admission
target. That target is a SuperZip policy, not an attributed industry standard.
Every final time/speed plot shows labeled sample dispersion; deterministic
archive sizes may have a genuine zero-width size interval. Do not fabricate
nonzero uncertainty. Keep both RAM-only and filesystem lanes separately labeled.

Store comparator results with a content-addressed measurement identity. Reuse
requires matching methodology, corpus, executable version/hash, command settings,
measurement boundary and relevant hardware/software context, plus a valid
correctness record. Cosmetic document/chart edits do not require new timing.
Changes to the measured implementation, competitor stable release, inputs,
threading, timer or material host configuration invalidate affected records.
Permission review is refreshed independently and never inherited from a cache.

## Access And Evidence Limits

PCMag and TechPowerUp returned `Blocked by robots.txt` through the text reader.
The ordinary-browser PCMag attempt additionally returned an explicit tool
access denial. Firecrawl returned HTTP 402 for insufficient account credits.
These are distinct failures; none proves that PCMag used browser fingerprint
detection. No workaround, stealth browser or paid service was used.
FCBench's NSF mirror timed out and the VLDB PDF reader returned an internal
error; the author's arXiv v2 full text was accessible and reviewed instead.

Image search located publisher chart pages, but preview labels are not raw
data or redistribution permission. We link the primary pages and publish only
our own results. Do not claim a PCMag-specific protocol or infer a procedure
from an inaccessible article.

For browser tooling, [Playwright](https://playwright.dev/docs/intro) already
provides ordinary Windows browser automation. An additional agent framework
adds dependencies without solving an explicit access restriction.
[Browser Use](https://github.com/browser-use/browser-use) distinguishes its
free MIT library from separately charged inference/cloud services.
[SearXNG](https://docs.searxng.org/admin/searx.limiter.html) is self-hostable,
but its own documentation describes rate limits and upstream blocking; it is
not an unlimited-access guarantee. No new browser stack is installed here.
