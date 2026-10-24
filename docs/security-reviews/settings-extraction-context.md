# Settings numeric validation after extraction-path refactoring

## Individual decision

### Alert 8386

The maintainer's standing authorization, “Fix defects; allow exact proven
false-positive dispositions,” applies to this individual review of CodeQL
2.27.3 `cpp/equality-on-floats` in `SettingsJson::integer`.

The [numeric validation rationale](settings-numeric-validation.md) remains
valid: exact truncation comparison rejects finite fractional values before a
range-checked narrowing conversion. An approximate comparison would admit
invalid fractions. The complete parser, declaration and numeric regression
test files are unchanged from the previous reviewed context.

The current extraction controller passes typed settings into owned jobs. Path
selection, worker lifetime and diagnostic refactoring do not change this pure
numeric admission boundary. `integer()` first checks the stored scalar's type,
finiteness, exact integrality and representability as `int`, then narrows and
clamps it. Exact equality is intentional for a stored numeric value; this is
not an approximate physical measurement or an arbitrary-precision JSON claim.

The required-HIP Release build and all seven CTest targets passed for these
native source bytes, including the settings regressions for fractional values,
integer endpoints, exponent notation, negative zero and invalid scalar types.
The individual ledger binds this reviewed caller change, the unchanged
numeric source and tests, scanner configuration, exact report location and
both explicit CodeQL analysis categories. Further changes expire the review;
the original report remains retained. This is a false-positive review, not a
production defect repair or a claim of complete coverage.
