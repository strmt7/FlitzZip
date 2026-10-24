# FlitzZip Implementation Plan

## Current Milestone

Prepare a Windows x64, HIP-enabled beta candidate with source-bound verification
and exact ZIP/MSI qualification. Publication requires the maintainer's green light.
See [beta readiness](docs/beta-readiness.md) and [release qualification](docs/release.md).

Use the [investigation skill](.agents/skills/flitzzip-test-investigation/SKILL.md)
and its research-backed oracle selection for the immediate development phase:
[meaningful coverage closure](docs/code-coverage.md#meaningful-tests). Accumulated
final qualification follows coherent development batches; this ordering does not
waive any release blocker or deferred improvement below.

## Engineering Priorities

1. Preserve archive correctness, backward readability, safe extraction and explicit
   format capabilities. Fix production boundaries and retain independent regressions.
2. Keep Neutron star mode GPU-only and separate from numeric efforts. Optimize
   complete archive bytes across representative workloads; transfer changes to
   ordinary efforts only after demonstrating both smaller size and faster execution.
   Pursue structural gains in representation, entropy modelling and GPU work
   scheduling; small overhead reductions alone do not meet the compression goal.
3. Investigate GPU failures through code, runtime and retained execution evidence.
   Keep unexplained incidents visible; a successful rerun does not establish a cause.
4. Repair actionable security findings at source. Preserve raw findings, provenance
   and executable recurrence guards. Individually proven false positives use
   the authorized exact-source review contract without suppressing scanner output.
5. Use licensed real corpora and RAM-only transport. Separate size qualification,
   descriptive timings, controlled comparisons and broader effectiveness claims.
6. Keep development tools portable, verification proportional to changed consumers,
   and current documentation concise. Reuse evidence only when its identities match.
7. Qualify the exact portable ZIP and MSI payloads, checksums and installer lifecycle
   before requesting publication approval.
8. Improve the CPU lane across supported algorithms. Measure each writer and reader
   separately at equal complete size or equal time, including startup, allocation,
   checksums and streaming. Preserve independent interoperability and avoid claiming
   that a native Zstandard improvement qualifies Deflate, Bzip2, LZMA or other codecs.

[Agent instructions](AGENTS.md) define the mandatory operating rules.
[Benchmark index](docs/benchmarks/README.md) identifies current evidence and limitations.
Completed checkpoint narratives remain in Git history and local review artifacts.

## Release Blockers

The work below remains required before release approval. Completed source-bound
checks may be reused only while their inputs remain unchanged. The existing
candidate is not approved for publication.

The product and format migration uses FlitzZip and `.flzip` throughout active
interfaces, build targets, packages and tooling. Wire signatures and the MSI
upgrade identity remain stable. Legacy names are confined to compatibility
inputs and their tests, exact historical evidence, and verified build-cache
migration. The previous release and its tag were deleted at the maintainer's
request; this does not authorize publication of a replacement.

1. **Retain current source-analysis acceptance.** Qualify the exact pushed
   revision against open and reproduced dismissed findings. The authorized
   [individual false positives](docs/security-reviews/current-source-findings.md)
   and two exact governance observations remain bound to their reviewed identities.
   Repeat the affected detectors and exact-commit audit after further production
   changes; an earlier checkpoint does not approve later source bytes.
2. **Qualify current GPU execution.** Validate required-HIP behavior, bounded kernel
   progress, asynchronous ownership and failure cleanup on the candidate binaries.
   Preserve the historical incident evidence and its limitations in
   [beta readiness](docs/beta-readiness.md#outstanding-acceptance-limits).
   The maintainer has closed the unreproduced dictionary-corruption investigation
   as nonblocking after focused qualification; neither a driver cause nor a
   production repair has been established. Keep the distinct-input and start-schedule
   regressions active. Any new byte mismatch or hang reopens the affected release gate.
3. **Finish source and release qualification.** Complete the remaining parser,
   extraction, codec, driver and kernel review, plus the required current
   source and dependency scanner audit. Qualify the final pushed SHA's
   workflows, audit, HIP-enabled ZIP/MSI, checksums and installer lifecycle before
   requesting release approval. Earlier successful artifacts cover their own
   source identities, not every later commit.

4. **Complete meaningful coverage.** The maintainer has reactivated coverage-gap
   closure. Add independent behavior, malformed-input, lifetime and recovery tests,
   repairing demonstrated production defects at their actual boundaries.
   Do not expand tests merely to increase the percentage.
   Coverage remains unfinished, not waived or passed.
   Use the native coverage artifact's uncovered-line
   and unmeasured-source inventories; the Debug measurement is the current baseline.
   Close gaps with independent behavior and failure-path tests, and qualify separate
   measurement for executable-only, tooling and device code. Do not equate a native
   CPU percentage with whole-repository coverage or declare the requested complete
   coverage achieved while those gaps remain. The [coverage handoff](docs/code-coverage.md#coverage-follow-up)
   identifies the existing measurements and their reuse limits.

## Deferred Improvement Work

- **Neutron effectiveness:** the complete version-eleven Govdocs1 study now
  covers all 991 original files through the licensed RAM-only controller at
  its recorded native source identity. The 6,060-byte aggregate gain is small.
  Follow with controlled, paired CPU/GPU measurements and multiple independent
  workload classes. Preserve complete archive sizes and byte-exact readback;
  current evidence does not establish broad size superiority or a GPU speedup.
- **Compression research:** evaluate the retained primary-source research before
  another broad crawl. Test GPU-suitable dictionary search, parsing, entropy and
  lossless model-data ideas, counting every model, dictionary and format byte.
  Keep Neutron GPU-only and separate from efforts 1–9. Adopt an idea only after
  independent correctness, malformed-input and representative size measurements.
  Current priorities are wider independent numerical contexts, batched GPU work,
  compact entropy representations and context modelling for text. Evaluate a small
  learned selector on held-out files only after collecting an exhaustive candidate
  oracle: count training/inference cost and every decoder/model byte. A classifier
  can order trials; it cannot justify dropping a competitive candidate in a
  minimum-size mode without a proven lower bound. Track native CPU, ZIP/Deflate,
  Gzip, Bzip2, Zstandard and LZMA-family results separately, including losses.
- **Repository review:** finish the unreviewed source and document ranges;
  inventory coverage is not a line-by-line review. Refactor only demonstrated
  ownership, duplication, complexity or performance problems and retain direct
  consumers. Recheck frontend Neutron admission and interaction when their code
  changes. Preserve the existing AGPL-3.0 license, third-party notices and
  action-specific benchmark permissions.
- **Benchmark presentation:** the current quantitative SVG fixture and all
  31 Mermaid diagrams across seven documents have been rendered and visually
  inspected. Repeat the affected visual and data checks when figures change,
  including uncertainty/error bars, units, scales and legends. Keep current
  measurements distinct from golden renderer fixtures; archive obsolete
  narratives locally without removing tests.
- **Development efficiency:** optimize measured build/workflow bottlenecks while
  retaining traced security analysis and release gates. Finish fresh-host and
  existing-installation tool qualification where evidence is missing. Keep
  canonical agent rules concise, crawler installation portable and finding
  prevention executable; do not add recurring maintenance for unchanged tools.

Reuse completed checks only when source, tools, configuration and outputs still
match. Resume from the retained local evidence and current GitHub audit rather
than repeating unchanged scans or benchmarks.
