---
name: flitzzip-test-investigation
description: Design meaningful FlitzZip test batches and investigate failures; use independent oracles, reproducible inputs, lifecycle assertions and source-bound coverage.
---

# FlitzZip Test Investigation

Use for a test-authoring batch, an unexplained failure or a missing behavioral test. Routine execution
of already selected checks belongs to `flitzzip-build-test`. Follow
[debugging strategy](../../../docs/debugging-strategy.md#investigation-contract)
for the investigation contract and
[meaningful tests](../../../docs/code-coverage.md#meaningful-tests) for oracle
selection. Those guides own evidence and fault-injection requirements. Their
[research and adoption decisions](../../../docs/code-coverage.md#research-and-tool-adoption)
explain which current techniques fit the existing native/Python/HIP toolchain.

## When adding a batch

- Start from retained gaps and public behavior, not a target test count. State
  each case's invariant, independent oracle and plausible defect it must reject.
  Prefer explicit boundary/transition tables to duplicated test bodies.
- Write the planned coherent cases before coverage collection. Use the existing
  runtime and fixtures; do not add a test framework merely because a source uses it.
  Generated sequences need bounded inputs, replay data and a reduced failing case.
- Challenge uncertain assertions with a targeted old-source or isolated fault
  control. Never derive the expected result by calling the same production logic.
- Run the positive exact cohort once through the canonical bounded executor.
  Its optional `-ContinueAfterAssertionFailure` mode retains independent assertion
  failures; process/registry failures and lost backend readiness still stop it.
  The batch must remain failed if any case fails. Do not retry into a pass.
- Measure after the batch, preserving scope and source/tool identities. Add tests
  for remaining behavior gaps; group exposed problems by the first violated
  contract for repair. Repair a broken observation mechanism before interpreting
  its measurement. Keep deferred gates explicit; this is not release acceptance.

## Work from the boundary

1. Select one coherent behavior from the report or retained gap inventory.
   Identify the public consumer, expected invariant and actual failure. Match
   source, binary, input and configuration before reusing a reproduction.
2. Trace the first violated contract, including ownership before acquisition,
   publication, cancellation and final completion. Distinguish a product defect
   from a test-driver or measurement defect; retain unresolved alternatives.
3. Choose an oracle before editing. Pair a legal success control with rejection
   at the affected boundary. A writer/reader round trip alone can share a bug;
   use independent bytes, another compatible reader or a separately derived
   invariant. Generated cases need a reproducible seed and a reduced failing input.
4. Inject failure at a real external boundary when ordinary inputs cannot reach
   it. Keep control per instance, preserve the real successful operation, and
   verify resource release, unchanged caller data and successful reuse. Do not
   exhaust the host, patch global imports or change production policy for a test.
5. Repair the production mechanism and inspect affected siblings. Establish that
   the assertion detects the defect, using the original failing source or a
   narrowly isolated negative control when practical. Record a limitation when
   neither is feasible; execution alone is not proof of an effective assertion.
6. Apply the [learning loop](../../../docs/engineering-learning-loop.md) and run
   the planner-selected direct consumers. Collect coverage after a coherent batch;
   final qualification still covers the accumulated changes.

## Choose the next step

| Observation | Decision |
| --- | --- |
| Reproducible parser or size failure | Minimize the bytes and exercise adjacent valid/invalid bounds through the public reader |
| Intermittent GUI or worker failure | Observe ownership, message order and completion events before changing waits or retries |
| Cleanup or allocation failure | Verify the relevant runtime/exception contract and all retained owners, then test recovery |
| Zero-hit line | Determine whether the behavior is absent, the collector is incomplete or the path is genuinely unreachable |
| GPU failure | Use `flitzzip-security` for lifetime/progress analysis, then qualify real HIP execution; host counters do not measure device instructions |
| Proposed speed or size gain | Use `flitzzip-performance`; correctness and measured performance are separate conclusions |

Keep reproduction details and evolving results in local evidence, not this skill.
Do not silently exclude code, add test-only product branches or change an API
solely to satisfy a coverage counter. Report the proven cause, affected contract,
tests actually run, remaining uncertainty and next required qualification.
