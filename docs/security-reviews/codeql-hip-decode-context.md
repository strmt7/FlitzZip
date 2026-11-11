# CodeQL HIP decode and dispatch context

This review covers the existing CodeQL 2.27.4 `hip-host` reports below after
the host decode and CRC refactoring. The reported HIP translation unit,
coordinates and rule remain unchanged. The ledger binds this review, the
changed callers, the dispatch helper and the relevant tests to exact bytes.
It does not admit another report or replace fresh hosted analysis.

## Individual decisions

### Alert 703

The report identifies `__constant__` on `kDeviceCrc32Table` as an unused static
variable. The declaration is a HIP device-memory qualifier; the table has five
device reads in contiguous, independent-range and decoded-block CRC kernels.
The [device review](codeql-hip-device-semantics.md#alert-703) records the original
source interpretation and its limits.

The shared materialization helper retains the validated payload and descriptor
table through synchronous decoding and status validation, then checks their
release before reducing the reservation. The output buffer and its remaining
reservation survive until the synchronous consumer returns. Extraction copies
the completed output; verification calls
the same contiguous CRC kernel on that output and downloads CRC metadata only.
The decoder sequence and kernel binding are unchanged. The shared dispatch
helper completes the calling thread's stream before a consumer reads or frees
its buffers. With telemetry it retains the existing event completion path;
without telemetry it synchronizes the same stream directly.

Plain, sparse and prefix materialization and archive dictionary decoding now
use that same dispatch helper. Their kernels, launch geometry, arguments and
status validation are retained. Absent telemetry no longer creates events or
computes an unused elapsed interval in those callers. The compound reader's
independent fill, pattern and dictionary fixtures exercise both requested and
absent telemetry through the same decoded-byte and CRC assertions. This
completion change does not make a profiling result evidence of device coverage.

Composed integrity now consumes the completed inverse byte-plane window directly
on device. Its source, output and workspace reservations survive through the
inverse transform and CRC. Compound verification still decodes and admits the
bounded recovered encoding in host RAM, then uses the ordinary HIP CRC route
for its final stage. Mixed plain blocks retain their existing CRC specialization
and combine in decoded order. Independent frames test CRC equality, transfer
bounds below a decoded block, cancellation at both inverse-transform boundaries
and successful reuse; both observation modes remain exercised.

The reservation can only decrease after completed scratch buffers are freed;
it cannot grow or reacquire capacity. The independent ownership regression
checks exact live accounting at both inverse boundaries, cancellation and
immediate reuse. Additional owner tests cover rejected growth, repeated
reductions, moves, independent siblings, zeroing and exception unwinding.
These checks observe logical admission accounting, not physical VRAM usage.

These changes neither remove the device reads nor turn the qualifier into a
host variable. Removing the table would break integrity checking. Independent
bitwise CRC and guarded production-kernel tests exercise these consumers;
required-HIP archive tests cover the materialized verification route.

### Alert 8452

The local `block_end` at the reported location still bounds the immediately
following fill, literal and periodic-data loops in the decoded CRC kernel.
Each branch reads the value before advancing to the next descriptor. The
[device review](codeql-hip-device-semantics.md#alert-8452) covers the unchanged
device source.

The host CRC entry point still validates the decoded layout and routes compound
and materialized representations separately. Raw, fill and periodic descriptors
retain their direct decoded-CRC route. Dispatch still passes the admitted
payload, descriptor count, decoded extent and CRC segment geometry to the same
kernel. The helper changes completion ownership and optional instrumentation,
not the loop's bounds or reachability. The independent mixed-block CRC test
also verifies that payload and descriptor bytes remain unchanged.

The value is used and is not static. This individual false-positive decision
does not establish device instruction coverage or explain CodeQL's extraction
behavior. Raw reports and complete CPU/HIP-host analysis remain required.
