# Neutron Stability Investigation

## Incident And Evidence Boundary

The maintainer reported a system-wide freeze requiring a forced reboot on
5 October 2026. The cause is **not established**. Neutron benchmarks and GPU
execution initially stopped; no claim of a completed stability repair is made.
After the source and lifecycle review recorded below, bounded device execution
resumed. The later [public-corpus study](../../neutron-public-corpus-benchmark.md)
completed all 991 Govdocs1 files and seven Canterbury block-setting runs with
byte-exact HIP readback. These successful finite runs do not identify the
incident's cause or establish universal safety.

The bounded read-only Windows event review found black-screen diagnostic
reports around the restart. It did not identify a SuperZip kernel, device
address, failing HIP call or reproducible input. The queried display-timeout,
hardware-error and resource-exhaustion providers supplied no corresponding
failure record. Missing telemetry cannot exclude a GPU or application defect.
Raw machine identities, private paths and unrelated application reports are
not published here.

The retained command history narrows the timeline, in local time:

- The repository-file GPU loop was stopped at approximately 08:00; a follow-up
  at 08:01 found none of its four known process IDs still running.
- The final recorded automated execution before the restart was the changed-file
  CPU scanner at 08:43. It completed in approximately three seconds.
- Windows records the latest boot at 08:45. This does not establish when the
  freeze began, or whether an earlier GPU operation had a delayed consequence.

Windows denied access to the `LiveKernelReports/WATCHDOG` directory. No ACL,
privilege or driver setting was changed to bypass that denial. The bounded
accessible application-log query returned no `LiveKernelEvent` in its selected
pre-restart interval. The absent dump and failure record limit causal analysis.

Earlier repository-file iteration is not a canonical corpus benchmark. Its
repeated whole-build identity checks created avoidable reads and its timing
study did not complete. None of those observations proves the freeze's cause.
Public-corpus timing has not been run for the current source changes.

## Source Review And Concrete Change

The review traced Neutron's predecessor search, match-graph classification,
descending parse, incremental writer and host dispatch/completion path.
Barrier participation, segment-local indexing, tree extents, predecessor
progress, byte-comparison accounting and workspace ownership were inspected.
No reproducer for the system freeze has been established by that review.
This is not device race detection or a substitute for hardware qualification.

One lifecycle concern is concrete: `run_neutron_stage` previously created and
destroyed a fresh pair of HIP timing events at every small dispatch. A full
active segment alone requires 2,048 parse dispatches, in addition to search,
classification and writing. That causes repeated driver-object churn. It is
an engineering defect to remove, **not a confirmed cause of the incident**.

The revised path owns one pair in each Neutron operation. The production
`NeutronStageClock` performs dispatch, successful synchronization/timestamp
collection, then reuse, in that order. Dispatch or completion exceptions leave
that clock unusable for another stage. Independent workers retain independent
owners; no global event pool or shared mutable handles are introduced. The
HIP adapter still waits with `finish_dictionary_stage`; it does not submit an
unsynchronized queue of stages. Compression decisions, wire bytes, kernel
algorithms and ordinary numeric compression levels are unchanged by this
lifecycle change.

AMD documents that recording over a still-recording event is undefined, while
a completed event can have its state replaced. The
[event API](https://rocm.docs.amd.com/projects/HIP/en/docs-10.0.0/reference/hip_runtime_api/modules/event_management.html)
and the pinned SDK's dispatch declarations are the applicable contracts.
Windows
[timeout detection and recovery](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/timeout-detection-and-recovery)
is an operating-system recovery mechanism, not evidence that these application
kernels are correct. No TDR settings or host driver settings were changed.

## Host Verification And Remaining Gates

The production clock and its canonical C++ tests were compiled with native
MSVC C++20, `/W4 /WX`, using the existing test runner. This isolated executable
links neither SuperZip's HIP adapter nor a HIP runtime. All four tests passed:
stable ownership and completion ordering, separate operation state, failures
during both dispatch and completion followed by rejected reuse, and missing,
invalid or overflowing timings remaining unavailable. These tests use explicit
host-only backend doubles; they do not execute GPU kernels.

Two isolated incorrect-header controls were also rejected: permitting reuse
after failed completion, and replacing unavailable timing with zero. Canonical
source was unchanged during those controls.

The pinned HIP compiler subsequently passed a `-fsyntax-only` integration check
for all six configured release targets: `gfx1100`, `gfx1101`, `gfx1102`,
`gfx1151`, `gfx1200` and `gfx1201`. It produced no executable or object and ran
no device kernels. The driver's unused-linker-option warnings are expected in
syntax-only mode; they are not source compilation failures. This checks the
typed HIP adapter, not device execution, races, scheduling or driver stability.

The same source is in the ordinary native suite. A separate optional CMake
target supports quick host-only recurrence checks without building the app:

```powershell
cmake --build build --config Release --target superzip_neutron_stage_clock_contracts
build/Release/superzip_neutron_stage_clock_contracts.exe
```

Device execution resumed in stages after host contracts, source review and
the ordinary HIP-enabled build passed. The first 256-byte RAM-only probe
validated every source byte with real HIP dispatch. The exact tiny-input GPU
oracle and staged cancellation/recovery regressions then passed, followed by
segment/workspace cases through 1 MiB. No freeze was observed in those runs;
that observation does not establish the incident's cause or universal safety.

The normal suite subsequently passed 605 main C++ tests and all seven CTest
suites. Its direct CLI consumers passed fourteen CPU/required-HIP corpus
cases, source and binary-stdin rejection controls, allocation-free planning
for a 175 MiB source, and twelve controller correctness observations. Corpus
payloads remained in RAM. These are correctness fixtures, not the requested
canonical public-corpus performance study. That study was pending at this
checkpoint and subsequently completed as linked above; paired repeated timing
qualification remains separate.

The sanitizer gate exposed a separate test-lifetime defect: a composite
migration fixture took 123.79 seconds against its inherited 120-second outer
deadline. The repaired shared aggregate deadline is 600 seconds; individual
archive extraction and rejection children retain explicit 20/30-second bounds.
The policy's mutation controls reject missing bounds. All six ASan CTests
passed, including the instrumentation negative control. No sanitizer, coverage
case or assertion was removed.

Retained evidence includes `out/neutron-native-unit-tests-verifier.log`,
`out/neutron-first-device-probe.json`, the staged device regression logs,
`out/neutron-check-zstd-sanitizer-repaired.log`, and
`out/neutron-patch-deadline-policy-tests.log`. The all-page GUI smoke passed,
with regular and compact page screenshots and the Neutron selection reviewed.
The incremental HIP build retained identical qualified product binaries;
package smoke and license verification passed after the GUI closed.
Publication and benchmark timing qualification remain separate. The freeze investigation
remains open until its cause and relevant repair are evidenced; the event
lifecycle improvement alone does not close it.
