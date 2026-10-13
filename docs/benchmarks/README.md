# Benchmark Evidence

The [Neutron public-corpus report](../neutron-public-corpus-benchmark.md) contains
current size evidence and its qualification limits. The
[native suite](../compression-level-and-benchmark-suite.md) defines numeric-level
CPU/HIP measurements. [Comparison methodology](../comparative-benchmark-methodology.md)
defines equal settings, complete container bytes, correctness and uncertainty.

## Published Records

| Record | Scope |
| --- | --- |
| [Neutron Canterbury](data/neutron-canterbury.json) | Eleven complete original files, three actual HIP repetitions, 1 MiB blocks |
| [Neutron Pythia14M](data/neutron-pythia14m.json) | One complete trained checkpoint, three actual HIP repetitions, 1 MiB blocks |
| [Neutron Govdocs1](data/neutron-govdocs1.json) | All 991 original thread0 files, one complete actual HIP pass, 1 MiB blocks |
| [Canterbury standard formats](data/canterbury-standard-reference.json) | Fixed independent ZIP, gzip, bzip2 and XZ size references with RAM readback |
| [Pythia14M standard formats](data/pythia-standard-reference.json) | Fixed complete-checkpoint size references with RAM readback |

Neutron records preserve source, binary, corpus and tool identities, every
observation and actual completion time. Payloads remain in RAM. Comparative
timing remains unqualified. Standard-format records retain their original
measurement identities; they are comparisons, not new runs of current source.

The Govdocs1 record covers the version-eleven context implementation and binds
its comparison to the earlier complete pruning study. Nine files improve in
size and none regresses; the aggregate reduction is only 6,060 bytes.
Superseded studies, abandoned runs and chronological narratives are recoverable
from Git history and the local material archive, rather than published as
current evidence. Follow the operating guide's
[evidence lifecycle](../agent-operating-guide.md#engineering-quality-baseline)
when replacing a record; do not change dates or measurements to imply freshness.

## Graphs And Regression Fixtures

Current guides use the measured size tables above. Superseded quantitative
graphs have been retired from product documentation. Mermaid diagrams describe
behavior and security flow; design-reference images describe UI concepts.

The [fixture directory](data/fixtures) retains historical record
formats required by renderer and scanner-policy regression tests, including
paired and GPU-only effort records with a nonmonotonic size observation. The
[expected native SVG](../../resources/benchmarks/fixtures/native-v1.svg) is a
golden test output, not a current performance chart. Their original dates,
source identities, actual tool-version strings and measurements remain intact.
Product display labels and passive schema keys use the current names. The native
receipt fixture remains byte-identical because its digests cover the original
recipe and artifact names; it is read through the explicit historical schema.
These passive fixtures stay
within normal repository scanning and never authorize another corpus run.

The graph workflow still runs every renderer's validation tests, including
byte-identical legacy regeneration, finite geometry, observed range endpoints,
perpendicular caps, zero-width ranges and data-to-figure consistency. Whiskers
denote the stated observed range; they are not standard errors or confidence
intervals. The shared SVG template is not a measured figure.
