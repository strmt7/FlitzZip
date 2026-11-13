"""Reproduce the fixed GPU model goldens using exact rational arithmetic.

This is an offline test oracle, never an application encoder or CPU fallback.
Checking is read-only. --emit writes a candidate header to stdout for review;
it never changes the checked-in expectations to accommodate a product failure.
"""

from __future__ import annotations

import argparse
import re
from fractions import Fraction
from pathlib import Path

MAX_SOURCE_BYTES = 16 * 1024 * 1024
MAX_FIXTURE_BYTES = 256 * 1024
DEFAULT_FIXTURE = Path(__file__).resolve().parents[1] / "tests/cpp/rans_model_fixtures.hpp"


def normalize(counts: list[int], precision: int) -> list[int]:
    """Return constrained rational quotas with a one-count floor and symbol ties.

    Input is exactly 256 nonnegative integer counts of total mass 1..16 MiB,
    with precision 8..16. No floating arithmetic or product code is used.
    """
    if type(precision) is not int or not 8 <= precision <= 16:
        raise ValueError("Unsupported model precision")
    if len(counts) != 256 or any(type(count) is not int or count < 0 for count in counts):
        raise ValueError("Invalid model histogram")
    mass = sum(counts)
    if not 1 <= mass <= MAX_SOURCE_BYTES:
        raise ValueError("Invalid histogram mass")
    ordered = sorted((count, symbol) for symbol, count in enumerate(counts) if count)
    total = 1 << precision
    fixed = 0
    result = [0] * 256
    # Ascending counts make the floor prefix exact. Removing a below-floor
    # count lowers the remaining scale, so later rows need the revised quota.
    for count, symbol in ordered:
        if Fraction((total - fixed) * count, mass) >= 1:
            break
        result[symbol] = 1
        fixed += 1
        mass -= count
    shares = [(symbol, Fraction((total - fixed) * count, mass)) for count, symbol in ordered[fixed:]]
    for symbol, quota in shares:
        result[symbol] = quota.numerator // quota.denominator
    remaining = total - sum(result)
    ranked = sorted(shares, key=lambda pair: (-(pair[1] - int(pair[1])), pair[0]))
    for symbol, _ in ranked[:remaining]:
        result[symbol] += 1
    return result


def histograms() -> dict[str, list[int]]:
    """Define the original fixed cases without local files or private state."""
    maximum = MAX_SOURCE_BYTES
    sparse = {
        "singleton_high_symbol": {255: maximum},
        "full_alphabet_equal": dict.fromkeys(range(256), 1),
        "full_alphabet_floor": {**dict.fromkeys(range(255), 1), 255: maximum - 255},
        "binary_floor": {13: 1, 247: maximum - 1},
        "ascending_symbol_remainder_ties": {7: 9, 111: 9, 241: 9},
        "successive_floor_decisions": {3: 1, 31: 100, 127: 10000, 199: 1000000},
        "exact_one_frequency_threshold": {1: 1, 255: 255},
        "sparse_mixed_counts": {i * 13: ((i * 73 + 257) ^ (i * 17)) % 1009 + 1 for i in range(17)},
    }
    return {name: [values.get(symbol, 0) for symbol in range(256)] for name, values in sparse.items()}


def render_fixture() -> str:
    """Emit deterministic C++ data; formatting is independent of clang-format."""
    lines = [
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "#include <span>",
        "",
        "namespace flitzzip_test {",
        "",
        "// Fixed oracle data: rational constrained quotas, one-frequency floor,",
        "// largest remainders and ascending-symbol tie order. Each frequency",
        "// span contains nine consecutive distributions for precisions 8..16,",
        "// in the positive-symbol order of counts. No product normalizer is used.",
        "struct RansGoldenCount { std::uint32_t symbol, count; };",
        "struct RansGoldenModel {",
        "    std::span<const RansGoldenCount> counts;",
        "    std::span<const std::uint32_t> frequencies;",
        "};",
        "",
    ]
    names = []
    for name, counts in histograms().items():
        positive = [(symbol, count) for symbol, count in enumerate(counts) if count]
        base = "kRansGolden_" + name
        names.append(base)
        lines.append(f"inline constexpr std::array<RansGoldenCount, {len(positive)}> {base}_counts{{{{")
        lines.extend(f"    {{{symbol}U, {count}U}}," for symbol, count in positive)
        lines.append("}};")
        frequencies = [normalize(counts, precision) for precision in range(8, 17)]
        values = [distribution[symbol] for distribution in frequencies for symbol, _ in positive]
        lines.append(f"inline constexpr std::array<std::uint32_t, {len(values)}> {base}_frequencies{{")
        for begin in range(0, len(values), 16):
            lines.append("    " + ", ".join(f"{value}U" for value in values[begin : begin + 16]) + ",")
        lines.extend(["};", ""])
    lines.append(f"inline constexpr std::array<RansGoldenModel, {len(names)}> kRansGoldenModels{{{{")
    lines.extend(f"    {{{name}_counts, {name}_frequencies}}," for name in names)
    lines.extend(["}};", "", "}  // namespace flitzzip_test", ""])
    return "\n".join(lines)


def check_fixture(path: Path) -> None:
    """Check the complete fixed header, allowing only whitespace formatting.

    This source-data comparison is not a C++ parser or proof of GPU execution.
    A bounded read rejects oversized input before decoding or normalization.
    """
    with path.open("rb") as stream:
        raw = stream.read(MAX_FIXTURE_BYTES + 1)
    if len(raw) > MAX_FIXTURE_BYTES:
        raise ValueError("Model fixture exceeds its byte budget")
    actual = raw.decode("utf-8")
    expected = render_fixture()
    if re.sub(r"\s+", "", actual) != re.sub(r"\s+", "", expected):
        raise ValueError("Model fixture differs from the independent rational oracle")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, default=DEFAULT_FIXTURE, help="Header to check; never overwritten")
    parser.add_argument("--emit", action="store_true", help="Emit a candidate header to stdout instead of checking")
    args = parser.parse_args(argv)
    try:
        if args.emit:
            print(render_fixture(), end="")
        else:
            check_fixture(args.fixture)
            print("Verified 72 fixed model distributions; no product code or GPU execution used.")
    except (OSError, UnicodeError, ValueError) as error:
        parser.exit(1, f"Model oracle check failed: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
