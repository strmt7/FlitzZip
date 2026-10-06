# Repository Figure Review, 6 October 2026

The review covered every tracked quantitative figure and every Mermaid block
across the documentation, including historical reports and design references.
It did not collect replacement benchmark measurements.

## Quantitative Figures

All five figures indexed in [benchmark evidence](../../benchmarks/README.md)
were regenerated from their retained JSON records and inspected as rasterized
SVGs. Labels, units, axes, point/bar locations, complete sample ranges and
whisker caps were checked. Historical dates and unfavorable measurements remain.

The shared renderer now draws exact finite intervals with perpendicular caps.
Native studies retain observed ranges in older supported schemas as well as
newer ones. Comparison whiskers sit on their bars; tradeoff whiskers have visible
ends. Zero-width intervals are not artificially expanded. Forty affected graph
contracts passed, including independent geometry and data-to-figure checks.
Observed ranges are not labeled as confidence intervals or standard errors.

## Behavior Diagrams And Design References

The public GitHub renderings of all 31 Mermaid diagrams were inspected across
the format matrix, native specification, debugging strategy, benchmark suite,
refactoring guide, product audit and block-size validation guide. Captures cover
the top, middle and bottom regions of tall diagrams. Their source blocks were
compared byte-for-byte with the reviewed published revision and match the
current documents. No Mermaid syntax or overlapping-label defect was observed.

The five images under `resources/design` were inspected separately. They are
historical UI concepts and contain illustrative monitor traces, not empirical
benchmark plots or current frontend acceptance evidence. The uninstantiated
chart template is source material rather than a measured figure. Canonical
branding and the design-reference images were not altered.

Raw captures and review inventories stay in ignored local artifacts. Current
guides link to the canonical figure index; historical records retain their
original observation scope and updated relative paths.
