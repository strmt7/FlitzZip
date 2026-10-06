# Benchmark Evidence

Start with [the Neutron public-corpus studies](../neutron-public-corpus-benchmark.md)
for the latest retained size evidence, representative workload limits and RAM-only
transport. [The native suite](../compression-level-and-benchmark-suite.md) defines
numeric-effort measurements; Neutron results remain a separate series.
[Comparison methodology](../comparative-benchmark-methodology.md) defines equal
settings, complete container bytes, correctness, uncertainty and provenance.

## Current Evidence And Limits

Fresh Neutron studies after kernel-module admission cover all eleven Canterbury
files and the complete Pythia14M checkpoint in three byte-exact HIP repetitions
each, preserving the preceding context studies' complete per-file sizes.
The complete 991-file Govdocs1 pruning study belongs to its earlier revision.
All payloads remained in RAM. Comparative timing and broad superiority remain
unqualified; archival size observations are not freshly collected measurements.

The [frozen JSON records](data) retain exact source/binary/corpus identities and
all samples. Do not rewrite old measurements when algorithms, hardware or chart
styling change. New observations require a new source-bound record.

## Figures Across All Documents

| Figure | Scope and interpretation |
| --- | --- |
| [Beta native CPU/HIP](../../resources/benchmarks/beta-native-cpu-hip.svg) | Retained synthetic Mixed level-5 study; median elapsed-time throughput and observed all-sample min–max whiskers |
| [Native CPU/HIP](../../resources/benchmarks/native-cpu-hip.svg) | Historical native study with all recorded observed ranges |
| [Application comparison](../../resources/benchmarks/application-comparison.svg) | Historical Silesia subset; complete size and median times with observed min–max ranges |
| [Compatibility effort](../../resources/benchmarks/effort-tradeoff.svg) | Historical size/time tradeoff; logarithmic time axis and capped observed ranges |
| [Native effort](../../resources/benchmarks/native-effort-tradeoff.svg) | Historical native size/time study; CPU and required-HIP results retain their measured sizes |

Whiskers describe observed sample ranges, not standard errors or confidence
intervals. The graph workflow regenerates all five figures from unchanged raw
records. Renderer contracts independently check finite geometry, interval ends,
perpendicular caps, zero-width ranges and data-to-figure consistency.

The shared SVG template is an uninstantiated source template, not a measured
figure. Mermaid diagrams in domain guides describe behavior and security flow,
not quantitative results. Design-reference images show historical UI concepts,
not the current frontend or benchmark evidence.

Completed study narratives live in [benchmark history](../history/README.md).
They remain scanned, and their relative links lead to the retained raw records.
