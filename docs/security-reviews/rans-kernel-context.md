# Private rANS Kernel Context Review

This review applies the maintainer's authorization for individually proven
false positives. The listed reported files, highlighted locations, rules and
scanner versions remain unchanged. Only their changed build or module context
is renewed; this review admits no new finding or public archive representation.
Raw reports and the preceding individual evidence remain retained.

The kernel module now uses private ABI version 2, with its new export, exact
layout and complete device-entry inventory changed together. Existing component
binders and entrypoints remain present. The additional translation unit owns
only a bounded rANS segment decoder and its binder. It declares no allocator,
LHA callback, TAR helper or GDI state. Existing compiler definitions, source
membership and test registration remain unchanged; one separate test source is
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

## Individual Decisions

### Alert 8348

The miniz allocation still selects the Windows process heap in ordinary Windows
builds. Its reported `malloc` is confined to the explicit fuzz allocator branch,
with matching resize and release. Adding the HIP object changes none of those
definitions or allocation callers. The
[allocator review](miniz-sanitizer-context.md#alert-8348) remains applicable.

### Alert 632

The opaque Lhasa context remains borrowed. `LhaReaderSession` owns its input
and frees the C reader and stream before input member destruction, including
reader-construction failure. Test owners retain the same destruction order.
Neither the new kernel binder nor its test retains an LHA callback or context.
The [lifetime review](lha-callback-lifetime.md#alert-632) remains applicable.

### Alert 8422

`require_tar_writer_path_boundary` retains its direct call in the registered
TAR boundary case. That source and the registration macro are unchanged and
remain members of the same executable target. Adding a separate registered
test source does not remove the call. The preceding
[reachability review](neutron-compaction-context.md#alert-8422) still applies.

### Alert 8431

The local unsigned induction variable still starts at zero, increments once
per completed iteration and checks eight different colors and allocation
counts. Callbacks do not capture it. The test source, provider and registration
are unchanged. The [loop review](gdi-rejection-loop.md#alert-8431) still applies.

### Alert 703

`__constant__` still qualifies the device CRC table; its five device reads
remain in the unchanged source. The codec binder and all CRC entries remain
in the version-2 table and complete device-support inventory. The preceding
[device review](gpu-observation-context.md#alert-703) still applies.

### Alert 8452

`block_end` remains the bound of the fill, raw and periodic decoded-CRC loops.
The reported source, arguments and caller extents are unchanged. Its typed
kernel remains bound and admitted. See the
[device review](gpu-observation-context.md#alert-8452).

### Alert 8453

Static-prefix `word_count` still bounds the following scratch-word loop.
Its source, dimensions and binder remain unchanged in ABI version 2. See the
[device review](gpu-observation-context.md#alert-8453).

### Alert 8454

Static-prefix `word_base` still participates in the following `atomicOr`
destination. Neither the packing expression nor its extent changes. See the
[device review](gpu-observation-context.md#alert-8454).

### Alert 8455

`__global__` still qualifies the sparse-position count kernel. Its sparse
component binder and typed device-entry admission remain present; no sparse
source or launch changes. See the
[device review](gpu-observation-context.md#alert-8455).

### Alert 8456

Adaptive-prefix `word_count` still bounds the scratch-word loop. The adaptive
component binder and all four entropy-length specializations remain present.
See the [device review](gpu-observation-context.md#alert-8456).

### Alert 8457

Adaptive-prefix `word_base` still forms the `atomicOr` destination, with the
same packing geometry and allocation contract. See the
[device review](gpu-observation-context.md#alert-8457).

### Alert 8458

Adaptive-prefix `code` still supplies the bits shifted and masked in the packing
loop. Neither its table nor this read changes. See the
[device review](gpu-observation-context.md#alert-8458).

### Alert 8459

Adaptive-prefix `width` still bounds the inner packing loop and determines
the bits emitted. The source and component binding are unchanged. See the
[device review](gpu-observation-context.md#alert-8459).
