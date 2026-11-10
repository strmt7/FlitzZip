# Neutron payload compaction review context

This review binds the existing decisions affected by the shared Neutron payload
compactor. The Neutron host writer reuses its owned payload and leaves the
candidate sequence, codec kinds, tie handling and device calls intact. The
added tests exercise compaction directly with private in-memory data. They
preserve the existing test registrations and fixture bodies. The reported
source bytes, locations, scanner versions and categories are unchanged.

## Individual decisions

### Alert 703

The HIP translation unit still declares the constant-memory CRC table, with
the same five device reads. The host compactor acts on complete, host-owned
payloads after the selected encoder has returned. It neither replaces the
CRC launch API nor edits the device translation unit. The original
[device review](codeql-hip-device-semantics.md#alert-703) describes its consumers.

### Alert 8452

The decoded CRC kernel still uses its local block end to bound the fill, raw
and periodic loops. Compaction runs in the Neutron writer. It preserves the
source CRC, recomputes dense payload offsets and keeps the existing decoder
and kernel bindings. See the [device review](codeql-hip-device-semantics.md#alert-8452).

### Alert 8348

The changed HIP source and native test do not enter the miniz fuzz allocator
graph. The header is included by the HIP host writer and the native test,
and it declares no allocator replacement or build definition. The original
[allocator review](miniz-sanitizer-context.md#alert-8348) still covers the
same miniz operation.

### Alert 632

The native tests only construct FlitzZip chunk descriptors and byte vectors.
They neither install nor retain an LHA callback. The reported stream,
adapter, reader and borrowed context retain their reviewed lifetimes.
See the [ownership review](lha-callback-lifetime.md#alert-632).

### Alert 8422

The two compaction tests register independent cases. The TAR reader test,
its direct helper call and all eight independent fixtures retain the same
source and registration. See the [reachability review](codeql-tar-reader-context.md#alert-8422).

### Alert 8431

The compaction tests access neither GDI buffers nor the reported loop. The
loop's unsigned induction bounds, distinct values and allocation assertions
retain their reviewed source. See the [induction review](gdi-rejection-loop.md#alert-8431).

The exact-source ledger includes the shared compactor and this review in the
affected contexts. Fresh hosted analysis still supplies the current results.
