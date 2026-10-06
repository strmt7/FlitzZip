# SuperZip Implementation Plan

## Current Milestone

Prepare a Windows x64, HIP-enabled beta candidate with source-bound verification
and exact ZIP/MSI qualification. Publication requires the maintainer's green light.
See [beta readiness](docs/beta-readiness.md) and [release qualification](docs/release.md).

## Engineering Priorities

1. Preserve archive correctness, backward readability, safe extraction and explicit
   format capabilities. Fix production boundaries and retain independent regressions.
2. Keep Neutron star mode GPU-only and separate from numeric efforts. Optimize
   complete archive bytes across representative workloads; transfer changes to
   ordinary efforts only after demonstrating both smaller size and faster execution.
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

[Agent instructions](AGENTS.md) define the mandatory operating rules.
[Benchmark index](docs/benchmarks/README.md) identifies current evidence and limitations.
Completed checkpoint narratives remain in Git history and local review artifacts.

## Release Blockers

The maintainer requested a prompt handoff to conserve usage. The work below is
deferred, not completed or waived. The existing candidate is not approved for
publication; resume these gates before declaring the next beta ready.

1. **Close the remaining source-analysis findings.** Use the exact-commit audit,
   including reproduced dismissed reports, as the inventory. Review the PPMd,
   Gzip and Bzip2 callback lifetimes, LZ4 length/error handling and staged-header
   contract, and remaining allocation/copy and test-fixture reports. Preserve
   independent regressions and verify production repairs with the original
   detectors. The separately authorized [individual false positives](docs/security-reviews/current-source-findings.md)
   and two exact governance observations may pass only through their current
   identity checks. Record the final pushed audit result before closing this gate.
2. **Complete the GPU incident investigation.** Trace bounded kernel progress,
   asynchronous ownership, failure cleanup and runtime admission against the
   retained incident evidence. The supplied empty watchdog dumps cannot establish
   a cause. Do not label the freeze resolved from successful later GPU tests.
3. **Finish source and release qualification.** Complete the remaining parser,
   extraction, codec, driver and kernel review, plus any
   required authenticated scanner qualification. Qualify the final pushed SHA's
   workflows, audit, HIP-enabled ZIP/MSI, checksums and installer lifecycle before
   requesting release approval. Earlier successful artifacts cover their own
   source identities, not every later commit.

## Deferred Improvement Work

- **Development dependency:** the maintainer temporarily accepts the exact NLTK
  advisory for this round under the [current risk record](docs/security-reviews/dependency-risk-acceptance.md).
  Keep the upstream alert visible and reassess when the official fix or pinned
  environment changes. This accepted item does not block the current round.
- **Neutron effectiveness:** complete the current-format Govdocs1 study using
  the existing licensed corpus controller and RAM-only payload transport.
  Follow with controlled, paired CPU/GPU measurements and multiple independent
  workload classes. Preserve complete archive sizes and byte-exact readback;
  current evidence does not establish broad size superiority or a GPU speedup.
- **Compression research:** evaluate the retained primary-source research before
  another broad crawl. Test GPU-suitable dictionary search, parsing, entropy and
  lossless model-data ideas, counting every model, dictionary and format byte.
  Keep Neutron GPU-only and separate from efforts 1–9. Adopt an idea only after
  independent correctness, malformed-input and representative size measurements.
- **Repository review:** finish the unreviewed source and document ranges;
  inventory coverage is not a line-by-line review. Refactor only demonstrated
  ownership, duplication, complexity or performance problems and retain direct
  consumers. Recheck frontend Neutron admission and interaction when their code
  changes. Preserve the existing AGPL-3.0 license, third-party notices and
  action-specific benchmark permissions.
- **Benchmark presentation:** finish visual inspection of every remaining graph
  in every document, including uncertainty/error bars, units, scales, legends
  and source-data correspondence. Keep current evidence distinct from golden
  renderer fixtures; archive obsolete narratives locally without removing tests.
- **Development efficiency:** optimize measured build/workflow bottlenecks while
  retaining traced security analysis and release gates. Finish fresh-host and
  existing-installation tool qualification where evidence is missing. Keep
  canonical agent rules concise, crawler installation portable and finding
  prevention executable; do not add recurring maintenance for unchanged tools.

Reuse completed checks only when source, tools, configuration and outputs still
match. Resume from the retained local evidence and current GitHub audit rather
than repeating unchanged scans or benchmarks.
