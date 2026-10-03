# Zstandard Condition Cleanup — 3 October 2026

This batch addresses source clarity and redundant cleanup in the pinned
Zstandard 1.5.7 build. It does not claim to repair a demonstrated dangling
pointer, buffer overflow or archive corruption. The upstream archive remains
unchanged; production and test builds apply identity-checked transformations
to extracted source through `cmake/PatchZstdLegacy.cmake`.

## Reviewed Source Changes

| Open CodeQL reports | Change and semantic evidence |
| --- | --- |
| 1527–1530, `cpp/guarded-free` | Remove four null guards around standard `free` in COVER map destruction, sample scratch cleanup, best-state destruction and dictionary replacement. Standard `free(NULL)` is valid. Pointer resets, synchronization and allocation-error handling remain intact. Guards that also protect other operations are preserved. |
| 1495–1498, `cpp/incorrect-not-operator-usage` | Use logical OR for six construction-failure predicates in `ZSTDMT_createCCtx_advanced_internal`. All allocations and serial initialization still execute before this condition. Its operands only read local fields and the completed initializer result, so short-circuit evaluation changes no side effects or failure cleanup. |
| 1499–1500, `cpp/incorrect-not-operator-usage` | Express legacy v0.5/v0.4 repeat-offset update as `offsetCode != 0 || litLength == 0`. The original bitwise test is true for exactly the same unsigned-value combinations. Sequence decoding, entropy-state updates and offset history are unchanged. |

Changed generated functions receive Purpose/Inputs/Outputs contracts.
Complete original and resulting file hashes bind each transformation. An
incremental v0.5 build admits only the exact earlier constructor-repair hash
and must produce the same result as a fresh extraction. Unknown prior edits,
partial writes and output-identity mismatches still fail closed.

The changes remove redundant predicates and express the intended logic.
No compression throughput, ratio or generated-machine-code improvement has
been measured for this batch. The earlier GPU entropy-selection allocation
improvement remains documented separately.

## Verification And Limits

The normal HIP-enabled Release build passes. The affected dependency targets
cover production-DLL guarded dictionary/frame capacities, all nine legacy
allocation-failure cases, multi-inclusion header contracts, and reproducible
patch application. All 15 native Zstandard and TAR.ZST direct-consumer cases
pass, including all effort levels, byte-exact roundtrip, overwrite refusal,
bad-stream rejection and exact serialized-size parity.

Patch contracts cover eight transformed files: fresh extraction, incremental
upgrade, byte-identical repeated application, source drift, interrupted input
and output writes, and unchanged provenance-archive identity.

Initial CMake lint rejected overlong transformation strings and a test variable
name; the corrected lint passes. The first incremental fixture wrote CRLF
bytes on Windows instead of the production patch's LF bytes. Its normalized
hash matched the earlier patch exactly. The fixture now uses explicit LF
generation, and its complete patch contract passes without relaxing the hash.
The earlier passing bounds, legacy-failure and header checks were retained;
only the repaired patch mechanism and previously unrun consumers were run.
A local helper invocation also failed because PowerShell expanded variables
inside Python command text; a saved PowerShell script corrected that invocation.

The generic planner still requests the full native driver and package/installer
checks for this shared CMake path. Those gates are deferred for this intermediate
batch under the maintainer's explicit component-only direction, not reported as
passed. Final accumulated beta qualification remains required. No production,
test or configuration path was excluded, no rule disabled, and no alert manually
dismissed. Automatic closure of the ten reports requires the new hosted scan;
the other findings remain unresolved.

Private evidence under `out/` records the HIP build, passing component checks,
original failures, repaired patch contract, lint and policy checks. It is not
packaged or used to imply final acceptance.
