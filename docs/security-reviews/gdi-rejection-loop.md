# Off-Screen GDI Retry-Loop Review

## Individual Decision

Fresh CPU and HIP-host analyses of commit
`0597068a6407375687e5c364de4b43dec9d3d21e` both report the current condition
at line 177, columns 37 through 49. The changed surface provider, failure tests,
registration and caller context were individually re-reviewed before renewing
the current exact-source ledger entries. Earlier identities remain historical;
they do not admit a reproduced finding.

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
all 1,045 cases at the reviewed checkpoint, including this regression and the
new independently validated native-failure and live-owner rejection controls.

This is an individually proven false positive under the maintainer's existing
authorization. The reason for CodeQL's incorrect inferred range has not been
established. The decision is limited to this report, its two observed categories
and the exact source, scanner version, location and context in the committed
review ledger. It does not admit other constant comparisons or classify every
GDI failure as harmless. The test and scanner coverage remain intact; changed
identities expire the decision, and final hosted acceptance remains required.
