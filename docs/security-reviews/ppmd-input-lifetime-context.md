# PPMd input lifetime

## Reviewed source

### Alert 8347

CodeQL `cpp/stack-address-escape` reports the input-stream assignment in
`third_party/lzma_sdk/C/7zDec.c:159`. Both CPU and HIP-host producer categories
use the same reviewed source and synchronous decoder contract.

`SzDecodePpmd` allocates one `CPpmdWithInput` owner containing both the PPMd
decoder and its callback bridge. The borrowed `ILookInStream` remains alive in
the calling adapter throughout decoding. `ReadByte` consumes only that bounded
borrowed stream; the bridge is not returned, transferred to asynchronous work
or published globally. Range decoding and its error checks finish before
`Ppmd7_Free` releases the model region and the matching allocator releases the
common decoder/bridge owner. Allocation refusal returns before any callback can
observe an incomplete owner.

The free-list redistribution in `Ppmd7_GlueFreeBlocks` advances through each
saved `Next` link. Expressing this traversal with `while` preserves its original
initial node, advance-before-mutation ordering and termination condition. It
does not acquire or publish a stream, change callback ownership, or extend a
decoder lifetime. The source assignment and reported location remain unchanged.

The exact-source ledger binds the complete decoder, callback and model sources,
adapter, tests, scanner configuration and this review. PPMd regression consumers
cover allocation refusal, callback lifetime, I/O failure, truncation and empty
output. Their current execution and fresh analysis remain required separately;
this review does not claim a vulnerability repair or admit another finding.
