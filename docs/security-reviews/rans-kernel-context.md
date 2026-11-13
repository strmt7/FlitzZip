# Private rANS Kernel Context Review

This review applies the maintainer's authorization for individually proven
false positives. The listed reported files, highlighted locations, rules and
scanner versions remain unchanged. Existing decisions renew their changed build
and module context; two additional HIP-host parser reports are reviewed
individually below. No public archive representation is admitted.
Raw reports and the preceding individual evidence remain retained.

For these decisions, superseded review narratives are referenced through the
[immutable predecessor ledger](https://github.com/strmt7/FlitzZip/blob/2ee2e910d5cf6924e0f09f6f865b8a4c459fba8f/.github/scanner-false-positive-reviews.csv).
The current binding retains every executable source, caller, test, build and
scanner path, together with this current evidence. Historical narrative edits
cannot change the referenced predecessor's evidence or current source identity.

The kernel module now uses private ABI version 2, with its new export, exact
layout and complete device-entry inventory changed together. Existing component
binders and entrypoints remain present. The additional translation unit owns
only a bounded rANS segment decoder and its binder. It declares no allocator,
LHA callback, TAR helper or GDI state. Existing compiler definitions, source
membership and test registration remain unchanged; independent test sources are
added. The new tests use admitted immutable models and independently specified
decoded bytes. Qualification of that new path remains separate from these
static source and caller decisions.

The canonical HIP object command now rejects undefined, empty or relative
output paths before command generation. Configure-only controls exercise that
actual function. Its existing arguments, dependency paths and compiler launcher
are preserved; the new object's assigned output is included in the module.

The miniz and LHA reviews bind the changed build function and configure
fixtures, alongside their existing source and caller closure. The isolated
rANS implementation, POD and device tests neither allocate through miniz nor
retain Lhasa contexts, so they are not added as callers of those two reports.
Their existing context paths and scanner coverage are retained in full.

## Host Operation And Shared View

The host reader borrows exact resident input/output windows and uses the
canonical view's model workspace. It owns one charged model/plan/status
allocation through synchronous batch completion and status admission, before
checked normal release. Checkpoints see completed work; exceptions propagate
after an attempted drain. The caller retains the output through the existing
device CRC consumer. Neither host decoding nor a CPU fallback is introduced.

The address-only alias guard is shared by the portable view and HIP host
admission. Its body is identical to the preceding view-local implementation;
byte-membership cases independently exercise empty, adjacent, nested and
unrelated regions. The view's new payload accessor borrows immutable bytes and
changes no allocation, callback ownership, wire layout or decoding arithmetic.
Shared singleton fixtures emit the mathematical fixed-state identity, without
using a product encoder. Fixed independent byte arrays remain comparison oracles.

The LHA and miniz sources and direct callers are unchanged. The new isolated
host reader and tests do not register their callbacks or replace their allocators.
The TAR helper retains its same direct registered call, and the GDI loop retains
its same induction, providers and readbacks. Their earlier context paths remain
bound; unrelated new translation units are not substituted for those callers.
All listed device kernel bodies, component bindings and existing entrypoints
also remain unchanged. The additional CRC test consumes a live decoded window
through the existing synchronous CRC function; it does not alter the table,
decoded-block bounds, prefix packing or sparse kernel attributes below.

The changed native selector now includes both device suites when the shared
view or fixture changes, and includes the independent byte-region suite for
that shared helper. Exact-name controls retain every CPU reader case and require
HIP admission for device consumers. CMake and unknown native inputs still select
the broad driver. This strengthens qualification routing; it changes no allocator,
callback, registration macro, kernel body, scanner configuration or individual
decision below. The originally retained selector failure remains a failed record.

## Individual Decisions

### Alert 8348

The miniz allocation still selects the Windows process heap in ordinary Windows
builds. Its reported `malloc` is confined to the explicit fuzz allocator branch,
with matching resize and release. Adding the HIP object changes none of those
definitions or allocation callers. The
[allocator review](https://github.com/strmt7/FlitzZip/blob/2ee2e910d5cf6924e0f09f6f865b8a4c459fba8f/docs/security-reviews/miniz-sanitizer-context.md#alert-8348) remains applicable.

### Alert 632

The opaque Lhasa context remains borrowed. `LhaReaderSession` owns its input
and frees the C reader and stream before input member destruction, including
reader-construction failure. Test owners retain the same destruction order.
Neither the new kernel binder nor its test retains an LHA callback or context.
The [lifetime review](https://github.com/strmt7/FlitzZip/blob/2ee2e910d5cf6924e0f09f6f865b8a4c459fba8f/docs/security-reviews/lha-callback-lifetime.md#alert-632) remains applicable.

### Alert 8422

`require_tar_writer_path_boundary` retains its direct call in the registered
TAR boundary case. That source and the registration macro are unchanged and
remain members of the same executable target. Adding a separate registered
test source does not remove the call. The preceding
[reachability review](https://github.com/strmt7/FlitzZip/blob/2ee2e910d5cf6924e0f09f6f865b8a4c459fba8f/docs/security-reviews/neutron-compaction-context.md#alert-8422) still applies.

### Alert 8431

The local unsigned induction variable still starts at zero, increments once
per completed iteration and checks eight different colors and allocation
counts. Callbacks do not capture it. The test source, provider and registration
are unchanged. The [loop review](https://github.com/strmt7/FlitzZip/blob/2ee2e910d5cf6924e0f09f6f865b8a4c459fba8f/docs/security-reviews/gdi-rejection-loop.md#alert-8431) still applies.

### Alert 703

`__constant__` still qualifies the device CRC table; its five device reads
remain in the unchanged source. The codec binder and all CRC entries remain
in the version-2 table and complete device-support inventory. The preceding
[device review](https://github.com/strmt7/FlitzZip/blob/2ee2e910d5cf6924e0f09f6f865b8a4c459fba8f/docs/security-reviews/gpu-observation-context.md#alert-703) still applies.

### Alert 8452

`block_end` remains the bound of the fill, raw and periodic decoded-CRC loops.
The reported source, arguments and caller extents are unchanged. Its typed
kernel remains bound and admitted. See the
[device review](https://github.com/strmt7/FlitzZip/blob/2ee2e910d5cf6924e0f09f6f865b8a4c459fba8f/docs/security-reviews/gpu-observation-context.md#alert-8452).

### Alert 8453

Static-prefix `word_count` still bounds the following scratch-word loop.
Its source, dimensions and binder remain unchanged in ABI version 2. See the
[device review](https://github.com/strmt7/FlitzZip/blob/2ee2e910d5cf6924e0f09f6f865b8a4c459fba8f/docs/security-reviews/gpu-observation-context.md#alert-8453).

### Alert 8454

Static-prefix `word_base` still participates in the following `atomicOr`
destination. Neither the packing expression nor its extent changes. See the
[device review](https://github.com/strmt7/FlitzZip/blob/2ee2e910d5cf6924e0f09f6f865b8a4c459fba8f/docs/security-reviews/gpu-observation-context.md#alert-8454).

### Alert 8455

`__global__` still qualifies the sparse-position count kernel. Its sparse
component binder and typed device-entry admission remain present; no sparse
source or launch changes. See the
[device review](https://github.com/strmt7/FlitzZip/blob/2ee2e910d5cf6924e0f09f6f865b8a4c459fba8f/docs/security-reviews/gpu-observation-context.md#alert-8455).

### Alert 8456

Adaptive-prefix `word_count` still bounds the scratch-word loop. The adaptive
component binder and all four entropy-length specializations remain present.
See the [device review](https://github.com/strmt7/FlitzZip/blob/2ee2e910d5cf6924e0f09f6f865b8a4c459fba8f/docs/security-reviews/gpu-observation-context.md#alert-8456).

### Alert 8457

Adaptive-prefix `word_base` still forms the `atomicOr` destination, with the
same packing geometry and allocation contract. See the
[device review](https://github.com/strmt7/FlitzZip/blob/2ee2e910d5cf6924e0f09f6f865b8a4c459fba8f/docs/security-reviews/gpu-observation-context.md#alert-8457).

### Alert 8458

Adaptive-prefix `code` still supplies the bits shifted and masked in the packing
loop. Neither its table nor this read changes. See the
[device review](https://github.com/strmt7/FlitzZip/blob/2ee2e910d5cf6924e0f09f6f865b8a4c459fba8f/docs/security-reviews/gpu-observation-context.md#alert-8458).

### Alert 8459

Adaptive-prefix `width` still bounds the inner packing loop and determines
the bits emitted. The source and component binding are unchanged. See the
[device review](https://github.com/strmt7/FlitzZip/blob/2ee2e910d5cf6924e0f09f6f865b8a4c459fba8f/docs/security-reviews/gpu-observation-context.md#alert-8459).

### Alert 10240

CodeQL 2.27.4's HIP-host analysis reports `__device__` as an unread static
variable at the declaration of `valid_rans_plan`. It is the HIP function
qualifier, not an object declaration. The function returns the metadata-admission
predicate and is called by `decode_rans_segments` before segment decoding.
The typed entry remains bound in the checksum-admitted module and complete
device-entry inventory. Guarded device cases exercise accepted plans, unsupported
precision/context widths, excessive segment work and uncontained extents.
Removing this qualifier or admission function would damage the device boundary.
This decision binds only that exact reported qualifier, location and producer.

### Alert 10241

CodeQL 2.27.4's HIP-host analysis reports local `frequency` as an unread static
variable. It is a local constant, and the immediately following state update
multiplies by it. The selected CDF interval is positive and bounded by the
full-width model mass. Both the portable reader and HIP fixtures require that
same frequency-dependent rANS recurrence; independent binary and segmented
payloads, stream consumption and terminal-state controls exercise it. Removing
or replacing the read would change decoding. This decision binds only the exact
reported local declaration, source bytes, location and producer.
