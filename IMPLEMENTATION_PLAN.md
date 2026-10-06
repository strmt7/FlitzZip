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
   and executable recurrence guards without admitting source alerts through suppressions.
5. Use licensed real corpora and RAM-only transport. Separate size qualification,
   descriptive timings, controlled comparisons and broader effectiveness claims.
6. Keep development tools portable, verification proportional to changed consumers,
   and current documentation concise. Reuse evidence only when its identities match.
7. Qualify the exact portable ZIP and MSI payloads, checksums and installer lifecycle
   before requesting publication approval.

[Agent instructions](AGENTS.md) define the mandatory operating rules.
[Benchmark index](docs/benchmarks/README.md) identifies current evidence and limitations.
Completed checkpoint narratives are retained in the
[documentation archive](docs/history/development/implementation-checkpoints.md).
