# HIP Runtime Ownership Context

This caller review applies the maintainer's authorization for exact, individually
proven false positives. It renews only the affected records in the committed
ledger. Every reported file, highlighted location, rule, category and scanner
version remains unchanged. The preceding individual evidence and all preceding
context paths remain bound. No new diagnostic receives a disposition here.

## Runtime And Ownership Changes

The private runtime descriptor copies its allocation/release pair and its
copy, clear, launch and synchronization callbacks into each operation. Borrowed
callback state must outlive the owner. Default callbacks invoke the same native
HIP APIs, arguments and per-thread stream. Hardware admission, device selection,
aggregate device budgets and the private module ABI remain unchanged. Typed
kernel arguments are consumed synchronously by submission; callbacks cannot
retain their stack addresses. Successful calls perform actual HIP work.

The shared buffer owner acquires through a local `void*` output, then converts
that pointer to its element type. It rejects an incomplete allocator pair,
replacement of a live allocation and a successful nonempty null allocation.
Checked release clears ownership only after success. A failed release retains
the pointer and its reservation for explicit cleanup. Destruction retains the
existing best-effort contract; persistent driver failure is not certified safe
by these controls. Host plans using the operation's `sizeof` include the copied
descriptors; device allocation geometry is unchanged.

Ordinary host transfers retain their native completion behavior. Device-to-device
copies can remain queued; the shared comment now requires both owners through
actual completion. No speculative synchronization or CPU compression fallback is
added. Existing callers without a descriptor use the same implementation through
native defaults. Measured event behavior and counters-only completion remain
separate and unchanged.

The encoder now retains a terminal statistics-child state even when its own
later stream drain succeeds. Its drain is still attempted and its original
exception is rethrown. A terminal operation rejects even a trial whose byte
budget would otherwise prune all device work. The real-HIP regression first
reproduced the missing rejection; allocation, transfer, submission, completion
and checked-release controls also exercise retained ownership and exact readback.
These controls reject before a real operation; they never free memory and then
invent a failure. They establish the tested contracts, not arbitrary driver
recovery or a cause for the historical system freeze.

## Existing Individual Decisions

- **846:** LZ4 overlap handling and admitted regions are unchanged.
- **847:** LZ4's bounded copy extents and producer/reader checks are unchanged.
- **848:** The debug newline remains a constant literal behind its debug guard.
- **849:** Literal-format debug callers remain unchanged; callback state is not
  used as a format string.
- **850:** The source/line debug format and its arguments are unchanged.
- **851:** LZ4's fixed allocation extents and matching release family remain
  unchanged; HIP callback ownership does not cross that allocator boundary.
- **8348:** The miniz allocator branches and matching resize/release are
  unchanged. Only the bound native-selector control adds registered cases.
- **632, both recorded categories:** Lhasa's borrowed input context and reader
  destruction order are unchanged. The new fixture retains no LHA callback.
- **8422, both recorded categories:** The TAR boundary helper retains its direct
  registered caller. Adding encoder case expectations removes no registration.
- **8431, both recorded categories:** The GDI test's induction, provider and
  readback are unchanged. No new callback captures that induction variable.
- **703 and 8452–8459:** The previously reviewed CRC qualifier/reads, decoded
  extents, prefix loop bounds/packing reads and sparse qualifier are unchanged.
  Native default dispatch retains their typed entries, buffers and completion.
- **10240–10245:** The decoder qualifier/frequency recurrence and transition
  count bounds/offsets/qualifier remain unchanged. The new runtime boundary
  changes their host caller control, not those declarations or direct reads.
- **10246–10259:** Every model/stream declaration or qualifier and its individual
  reads remain unchanged as documented in the preceding classification review.
  Host failure handling changes no model mass, symbol recurrence or wire layout.

The ledger preserves each decision separately, including duplicated alert
numbers with distinct analysis categories. This document admits no rule family,
new alert, suspicious pointer expression or public archive representation.
Current native execution and complete hosted analyses remain separate acceptance
requirements. Raw findings remain retained and all scanner coverage stays active.
