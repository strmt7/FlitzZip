# GPU Observation Caller Review

The maintainer authorizes fixing defects and admitting only individually proven
false positives. This review renews the listed existing reports after the GPU
observation changes. Every reported source file, highlighted token, coordinate,
rule and scanner version was compared with its original report and is unchanged.
The source ledger binds the complete files, changed caller closure, this evidence
and the preceding individual rationales. It admits no other report.

Observation mode is immutable in operation telemetry and inherited by isolated
HIP attempts. Dictionary producer metadata must agree with the caller before
its work is recorded. A mismatch raises an archive contract error and cannot be
hidden by optional CPU retry. Measured mode remains the default.

Counters-only factories own no timing events. The same typed kernels, arguments,
dimensions and buffers are submitted; per-thread stream completion replaces the
optional event wait before the caller reads or releases storage. Measured event
and query failures propagate. Completed work is counted even when its subsequent
query fails. The bounded Neutron stage owner retains its failed-reuse prohibition.
Measured completion records its known launches together with the duration or an
explicit unavailable sample. The count-only primitive rejects nonempty measured
work before mutation; this contract error cannot be hidden by optional GPU retry.
No kernel, copy extent, device allocation geometry, format representation or HIP-only
policy changed. No rocPRIM-internal launch count is invented.

Fixed provider/completion controls, independent LZ4 readback, native CPU/HIP
readback and cancellation/recovery cases exercise these boundaries. Their actual
execution and fresh hosted analyses remain qualification requirements; this
static review is not a declaration that either passed. Raw reports remain retained.

## Device Reports

### Alert 703

`__constant__` still qualifies `kDeviceCrc32Table` at the unchanged device
declaration; it is not a variable name. The five device CRC reads remain present.
The [decode review](codeql-hip-decode-context.md#alert-703) records its consumers.
Optional host observation changes no CRC input, bound, dispatch or buffer lifetime.

### Alert 8452

`block_end` still bounds the fill, raw and periodic decoded-CRC loops at the
reported site. Each branch reads it before moving to the next descriptor. The
[decode review](codeql-hip-decode-context.md#alert-8452) remains applicable to
the unchanged device source; observation changes no decoded extent or loop.

### Alert 8453

Static-prefix `word_count` remains the bound of the immediately following
scratch-word loop. Host event selection changes neither the segment plan nor
this read. See the [device review](codeql-hip-device-semantics.md#alert-8453).

### Alert 8454

Static-prefix `word_base` is still read by the immediately following `atomicOr`
destination expression. No packing address or allocation geometry changed.
See the [device review](codeql-hip-device-semantics.md#alert-8454).

### Alert 8455

`__global__` remains the function qualifier of `count_sparse_positions_kernel`.
Its binding and typed launch remain present. Count and gather complete on the
same stream before their buffers are consumed. See the
[device review](codeql-hip-device-semantics.md#alert-8455).

### Alert 8456

Adaptive-prefix `word_count` remains the explicit scratch-word loop bound.
Optional timestamps change no symbol width, segment span or scratch extent.
See the [device review](codeql-hip-device-semantics.md#alert-8456).

### Alert 8457

Adaptive-prefix `word_base` remains part of the `atomicOr` destination. The
address expression and its validated segment geometry are unchanged. See the
[device review](codeql-hip-device-semantics.md#alert-8457).

### Alert 8458

Adaptive-prefix `code` still supplies the bits shifted and masked by the
immediately following packing loop. Profiling does not alter the code table.
See the [device review](codeql-hip-device-semantics.md#alert-8458).

### Alert 8459

Adaptive-prefix `width` still bounds that packing loop. The read determines how
many encoded bits are emitted, independently of observation. See the
[device review](codeql-hip-device-semantics.md#alert-8459).

## LZ4 Reports

The imported raw-block implementation remains unchanged. The dictionary caller
now passes observation mode and checks returned mode identity; it retains the
same payload framing, source lengths, workspace admission and exact-length CPU
reader. No frame, ring-buffer or deprecated unsafe decoder was introduced.

### Alert 846

The `LZ4_memmove` fallback remains required for overlap within admitted regions.
Observation mode changes no copy extent or aliasing relationship. See the
[individual copy review](lz4-current-validation.md#alert-846).

### Alert 847

The standard `LZ4_memcpy` fallback still does not select its extent. Bounded
producer and reader checks remain in place; the observation change adds no copy.
See the [individual primitive review](lz4-current-validation.md#alert-847).

### Alert 848

The debug newline output remains a constant literal behind `LZ4_DEBUG >= 2`.
Neither its format nor build admission changed. See the
[individual debug review](lz4-current-validation.md#alert-848).

### Alert 849

The variadic debug formatter and its literal-format callers are unchanged.
Observation metadata is not used as a format string. See the
[individual format review](lz4-current-validation.md#alert-849).

### Alert 850

The source/line debug output still uses its constant format with `__FILE__` and
`__LINE__`; no input-selected format is introduced. See the
[individual debug review](lz4-current-validation.md#alert-850).

### Alert 851

The allocation macro, fixed stream/state extents and matching free family are
unchanged. Observation objects do not cross this allocator boundary. See the
[individual allocator review](lz4-current-validation.md#alert-851).
