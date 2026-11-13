"""Independent mathematics and corruption controls for fixed GPU model data."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from tools import rans_model_oracle as oracle


def water_fill(counts: list[int], precision: int) -> list[int]:
    """Independently eliminate all below-floor symbols in simultaneous rounds."""
    active = {symbol: count for symbol, count in enumerate(counts) if count}
    mass = sum(counts)
    budget = 1 << precision
    result = [0] * 256
    while True:
        small = [symbol for symbol, count in active.items() if count * budget < mass]
        if not small:
            break
        for symbol in small:
            result[symbol] = 1
            budget -= 1
            mass -= active.pop(symbol)
    remainders = []
    assigned = 0
    for symbol, count in active.items():
        result[symbol], remainder = divmod(count * budget, mass)
        assigned += result[symbol]
        remainders.append((-remainder, symbol))
    for _, symbol in sorted(remainders)[: budget - assigned]:
        result[symbol] += 1
    return result


class RansModelOracleTests(unittest.TestCase):
    def test_checked_in_header_matches_without_ignored_prototype_files(self):
        oracle.check_fixture(oracle.DEFAULT_FIXTURE)

    def test_rational_quotas_match_independent_simultaneous_water_filling(self):
        cases = list(oracle.histograms().values())
        # Fixed bounded distributions challenge more than the checked-in goldens.
        cases += [
            [((symbol * 73 + seed * 257) ^ (symbol * seed)) % 1009 for symbol in range(256)] for seed in range(17)
        ]
        for counts in cases:
            for precision in range(8, 17):
                with self.subTest(counts=counts[:4], precision=precision):
                    actual = oracle.normalize(counts, precision)
                    self.assertEqual(actual, water_fill(counts, precision))
                    self.assertEqual(sum(actual), 1 << precision)
                    self.assertTrue(
                        all((count > 0) == (frequency > 0) for count, frequency in zip(counts, actual, strict=True))
                    )

    def test_singleton_remains_full_width_at_maximum_precision(self):
        counts = [0] * 256
        counts[255] = oracle.MAX_SOURCE_BYTES
        self.assertEqual(oracle.normalize(counts, 16), [0] * 255 + [65536])

    def test_equal_remainders_use_actual_symbol_order(self):
        counts = [0] * 256
        for symbol in (7, 111, 241):
            counts[symbol] = 9
        actual = oracle.normalize(counts, 8)
        self.assertEqual([actual[symbol] for symbol in (7, 111, 241)], [86, 85, 85])

    def test_floor_is_recomputed_after_each_eliminated_prefix(self):
        counts = [0] * 256
        for symbol, count in ((3, 1), (31, 100), (127, 10000), (199, 1000000)):
            counts[symbol] = count
        actual = oracle.normalize(counts, 8)
        self.assertEqual([actual[symbol] for symbol in (3, 31, 127, 199)], [1, 1, 3, 251])

    def test_rejects_invalid_counts_and_precisions(self):
        valid = [1] + [0] * 255
        invalid = [
            [],
            [1] * 255,
            [1] * 257,
            [0] * 256,
            [-1] + [1] * 255,
            [True] + [0] * 255,
            [1.0] + [0] * 255,
            [oracle.MAX_SOURCE_BYTES + 1] + [0] * 255,
        ]
        for counts in invalid:
            with self.subTest(counts=counts[:4]), self.assertRaises(ValueError):
                oracle.normalize(counts, 8)
        for precision in (7, 17, True, 8.0, "8"):
            with self.subTest(precision=precision), self.assertRaises(ValueError):
                oracle.normalize(valid, precision)

    def test_fixture_check_is_read_only_and_accepts_formatting(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary) / "models.hpp"
            formatted = oracle.render_fixture().replace("\n", "\r\n")
            fixture.write_bytes(formatted.encode("utf-8"))
            before = fixture.read_bytes()
            oracle.check_fixture(fixture)
            self.assertEqual(fixture.read_bytes(), before)

    def test_fixture_check_rejects_corruption_without_rewriting(self):
        original = oracle.render_fixture()
        corruptions = {
            "mass": original.replace("65536U,", "65535U,", 1),
            "tie": original.replace("86U, 85U, 85U,", "85U, 86U, 85U,", 1),
            "symbol": original.replace("{255U, 16777216U}", "{254U, 16777216U}", 1),
            "truncated": original[:-20],
            "extra": original + "inline constexpr int extra = 0;\n",
        }
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary) / "models.hpp"
            for label, changed in corruptions.items():
                with self.subTest(label=label):
                    self.assertNotEqual(changed, original)
                    fixture.write_bytes(changed.encode("utf-8"))
                    with self.assertRaisesRegex(ValueError, "differs"):
                        oracle.check_fixture(fixture)
                    self.assertEqual(fixture.read_text(encoding="utf-8"), changed)

    def test_bounded_fixture_read_and_invalid_utf8_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary) / "models.hpp"
            fixture.write_bytes(b" " * (oracle.MAX_FIXTURE_BYTES + 1))
            with self.assertRaisesRegex(ValueError, "byte budget"):
                oracle.check_fixture(fixture)
            fixture.write_bytes(bytes([255]))
            with self.assertRaises(UnicodeError):
                oracle.check_fixture(fixture)

    def test_cli_check_emit_and_failure_are_explicit(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(oracle.main([]), 0)
        self.assertIn("72 fixed", output.getvalue())
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(oracle.main(["--emit"]), 0)
        self.assertEqual(output.getvalue(), oracle.render_fixture())
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary) / "missing.hpp"
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors), self.assertRaises(SystemExit) as result:
                oracle.main(["--fixture", str(fixture)])
            self.assertEqual(result.exception.code, 1)
            self.assertIn("Model oracle check failed", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
