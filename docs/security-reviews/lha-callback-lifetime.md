# LHA Callback Context Lifetime Review

## Individual Decision

### Alert 632

CodeQL's `cpp/stack-address-escape` reports the assignment of the opaque
`handle` parameter to `LHAInputStream::handle` in `lha_input_stream_new`.
The input stream deliberately borrows this context. Its public API requires
the caller to keep the context alive until `lha_input_stream_free` returns.
Storing a stack address does not violate that contract when the stream is
destroyed first.

Every compiled caller follows that lifetime:

- `LhaReaderSession` owns its `std::ifstream` with `std::unique_ptr`. It frees
  the Lhasa reader and stream in its destructor body, before member destruction
  closes the C++ stream. Reader-allocation failure frees the C stream before
  constructor unwinding destroys the input owner. Stream-allocation failure
  retains no context. The session cannot be copied or implicitly moved.
- `LhaMemoryReader` borrows a `LhaMemoryInput` declared earlier in the same test
  scope. Its reader and stream are `unique_ptr` members, destroyed in that
  order before the surrounding input leaves scope, including assertion
  exceptions. Every instance is local; no stream or context escapes the test.
- `lha_stream_owns_callback_configuration` declares its input before the
  stream owner and explicitly resets that owner while the input remains live.
  It verifies that the original close callback runs exactly once and updates
  the still-live context. Exception unwinding preserves the same destruction
  order.

Reads, skips and close callbacks run synchronously. The Lhasa reader and basic
reader borrow the stream; they do not retain work beyond their destruction.
No compiled caller releases its context while the stream can still invoke a
callback. The regression suite also exercises fragmented reads, injected input
errors, failed skips and clean termination with these real callback contexts.

This is separate from the callback-table lifetime defect reported as alert
8396. That defect was repaired: the stream now owns a complete callback-table
snapshot. The regression mutates the original table and verifies that the
stream retains its original skip and close behavior. The opaque context cannot
be copied generically because its type and size belong to its caller.

The maintainer's standing authorization, “Fix defects; allow exact proven
false-positive dispositions,” permits this individual decision. The committed
ledger binds both observed CPU and HIP-host categories, scanner version,
location, complete source, public contract, all callers, lifetime consumers,
tests, build configuration and this review. Changed bindings require another
review. Raw reports remain retained and scanned; this decision is not a
production repair or approval of any other stack-address report.
