# Off-Screen GDI Retry-Loop Review

## Individual Decision

### Alert 8431

CodeQL 2.27.3 reports `cpp/constant-comparison` at the retry-loop condition in
`tests/cpp/test_gdi_back_buffer.cpp`. Both the CPU and HIP-host analyses report
that `attempt < 8U` is always true because `attempt <= 0`.

The induction variable is a local unsigned 64-bit integer initialized to zero,
incremented once by the loop expression, and never assigned elsewhere or passed
by reference. The paint callbacks do not capture or modify it. Every completed
iteration therefore advances toward eight; a failed assertion throws out of the
test. Neither path can repeat indefinitely with the variable held at zero.

The test checks eight distinct pixel colors and successive lifetime allocation
counts after real Win32 presentation failures. Each cycle also verifies that
the original destination pixels survive and that the process's GDI resource count
returns to its baseline. The final valid frame requires the ninth allocation and
successful readback, followed by explicit release. A single iteration cannot
satisfy these final assertions. The complete HIP-enabled Release registry passed
this regression during qualification of the preceding source checkpoint, and
the reviewed test, back-buffer implementation and test macros are unchanged.

This is an individually proven false positive under the maintainer's existing
authorization. The reason for CodeQL's incorrect inferred range has not been
established. The decision is limited to this report, its two observed categories
and the exact source, scanner version, location and context in the committed
review ledger. It does not admit other constant comparisons or classify every
GDI failure as harmless. The test and scanner coverage remain intact; changed
identities expire the decision, and final hosted acceptance remains required.
