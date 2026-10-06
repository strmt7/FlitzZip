---
name: superzip-performance
description: Improve SuperZip compression and execution efficiency using mode-specific RAM-only benchmarks, licensed workloads, complete archive sizes and actual AMD HIP telemetry.
---

# SuperZip Performance

Read the affected contracts before changing codec policy, scheduling, telemetry
or benchmark claims:

- [Compression levels and benchmark suite](../../../docs/compression-level-and-benchmark-suite.md)
- [Block-size measurement and validation](../../../docs/performance-block-size-validation.md)
- [GPU codec and UI architecture](../../../docs/gpu-accelerated-ui-and-codec-research.md)
- [Native format](../../../docs/native-suzip-format.md) when encoding or decoding changes
- [Neutron corpus protocol](../../../docs/neutron-public-corpus-benchmark.md) for Neutron work

The operating guide owns shared-host admission and execution limits. Use the
change-aware verifier for correctness checks before benchmarking.

## Choose the measurement

| Question | Existing controller and interpretation |
| --- | --- |
| Standard numeric effort CPU/HIP comparison | `tools/bench.ps1`; documented paired protocol, equal effort and applicable block-size matrix |
| Neutron size or kernel improvement | `tools.neutron_corpus_benchmark`; licensed real corpora and required-HIP RAM transport |
| Compatibility-format comparison | Existing archive-comparison tooling and its permission/independent-reader contracts |
| Runtime, transfer or device fault | Corresponding diagnostic or focused correctness test; a diagnostic is not an application benchmark |

## Improve and verify

1. State the bottleneck or size hypothesis and freeze source, workload, settings
   and baseline identity. Use representative inputs and a holdout workload when
   selecting data-dependent strategies.
2. Inspect production host work, allocation, transfers, kernels, synchronization
   and final framing. Count complete archive bytes, including tables and any
   model or dictionary state.
3. Change the canonical production path. Preserve compatibility, failure behavior
   and independent byte-exact readback. GPU changes need bounded progress and
   asynchronous-lifetime controls before device execution.
4. Run selected correctness consumers, then the affected existing benchmark
   protocol. Preserve RAM-only transport and its emitted proof fields. Sample
   competing host load when drawing timing conclusions.
5. Compare repeated observations on the same basis. Retain losses, non-monotonic
   effort pairs and inconclusive timings. Separate size, correctness, throughput
   and hardware-coverage claims.
6. Refresh affected documentation and generated figures from retained records.
   Inspect every changed figure visually and preserve uncertainty correctly.

Neutron remains a distinct GPU-only mode prioritizing minimum complete archive
size. Its measurements and tuning must not enter ordinary effort scores or
caches. Moving an improvement into numeric efforts requires evidence of both
smaller output and faster execution under the maintainer's existing contract.
Standard paired CPU/HIP recipes therefore do not automatically apply to a
Neutron-only change.

Use intermediate checkpoints while tuning and the operating guide's final
verification and hosted-audit procedure before reporting release readiness.
