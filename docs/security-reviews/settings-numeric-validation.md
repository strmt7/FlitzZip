# Settings Numeric Validation Review

## Individual Decision

### Alert 8386

`cpp/equality-on-floats` reports the comparison between `std::trunc(*value)`
and `*value` in `SettingsJson::integer`. This is an intentional exact test for
a fractional component, not an approximate comparison between independently
computed measurements.

The function first checks the scalar type and finiteness. `std::trunc` removes
the fractional part; comparing its result with the original finite value
therefore rejects a representable non-integral number. Integral values in the
Windows 32-bit `int` range are exactly representable as `double`. The range
checks precede the narrowing cast, and application bounds are applied only
afterward. Negative zero remains an integral zero. Using an epsilon would
incorrectly admit fractions close to an integer and weaken this validation.

The parser's numeric representation is Windows.Data.Json's `double`. This
decision does not claim lossless preservation of arbitrary decimal literals
beyond that API's precision or authorize another floating-point comparison.

`test_settings_json.cpp` exercises both integer endpoints, exponent notation,
negative zero, type mismatches, out-of-range values, and fractional values
immediately around integers and close to zero. Both the CPU and HIP-host CodeQL
analyses report the same operation. The ledger binds their separate categories,
exact scanner version and location, complete source, declarations, callers,
regressions and scanner configuration. Any changed binding requires another
review. Original reports remain retained.

The maintainer's standing authorization, “Fix defects; allow exact proven
false-positive dispositions,” permits this individual decision. It is a
reviewed false positive, not a production defect repair.
