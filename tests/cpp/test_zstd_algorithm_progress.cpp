#include "test_util.hpp"

extern "C" {
#include "divsufsort.h"
// Purpose: Call the exact generated rank stage with bounded malformed fixtures.
// Inputs: Live count-element regions or null for empty input. Outputs: Production status.
int sz_rank_partial_suffixes(int* sorted, int* ranks, int count);
}

#include <array>
#include <limits>
#include <numeric>
#include <span>

namespace {
// Purpose: Compute suffix order independently through lexicographic comparison.
// Inputs: Complete initialized byte extent. Outputs: Owned permutation of all suffix starts.
std::vector<int> suffix_oracle(std::span<const unsigned char> input) {
    std::vector<int> expected(input.size());
    std::iota(expected.begin(), expected.end(), 0);
    std::sort(expected.begin(), expected.end(), [&](int left, int right) {
        const auto a = input.subspan(static_cast<std::size_t>(left));
        const auto b = input.subspan(static_cast<std::size_t>(right));
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
    });
    return expected;
}

// Purpose: Verify suffix and BWT consumers against an independent suffix permutation.
// Inputs: Initialized corpus, including repeated bytes and alternating/small alphabets.
// Outputs: Exact suffixes, BWT bytes and primary index; preserves both workspace/output canaries.
void require_suffix_consumers(std::span<const unsigned char> input) {
    constexpr int guard = -991;
    const auto expected = suffix_oracle(input);
    std::vector<int> suffixes(input.size() + 2, guard);
    const unsigned char empty = 0;
    const auto* source = input.empty() ? &empty : input.data();
    REQUIRE_EQ(divsufsort(source, suffixes.data() + 1, static_cast<int>(input.size()), 0), 0);
    REQUIRE_EQ(suffixes.front(), guard);
    REQUIRE_EQ(suffixes.back(), guard);
    REQUIRE_TRUE(std::ranges::equal(expected, std::span(suffixes).subspan(1, input.size())));
    std::vector<unsigned char> transformed(input.size() + 2, 0xA5);
    const auto primary =
        divbwt(source, transformed.data() + 1, nullptr, static_cast<int>(input.size()), nullptr, nullptr, 0);
    std::vector<unsigned char> bwt;
    int expectedPrimary = 0;
    if (!input.empty()) {
        bwt.push_back(input.back());
        for (std::size_t rank = 0; rank < expected.size(); ++rank) {
            const auto suffix = expected[rank];
            if (suffix == 0)
                expectedPrimary = static_cast<int>(rank + 1);
            else
                bwt.push_back(input[static_cast<std::size_t>(suffix - 1)]);
        }
    }
    REQUIRE_EQ(primary, expectedPrimary);
    REQUIRE_TRUE(std::ranges::equal(bwt, std::span(transformed).subspan(1, input.size())));
    REQUIRE_EQ(transformed.front(), 0xA5U);
    REQUIRE_EQ(transformed.back(), 0xA5U);
}
}  // namespace

// Purpose: Qualify actual suffix ordering and both production rank consumers independently.
// Inputs: Exhaustive short lengths and larger group boundaries across four byte alphabets.
// Outputs: Exact suffix/BWT agreement, including zero, one, two, repeated and randomized inputs.
TEST_CASE(zstd_suffix_progress_matches_lexicographic_oracle) {
    std::vector<std::size_t> lengths;
    for (std::size_t size = 0; size <= 128; ++size)
        lengths.push_back(size);
    for (const auto size : {255U, 512U, 1024U})
        lengths.push_back(size);
    for (const auto alphabet : {1U, 2U, 16U, 256U}) {
        for (const auto size : lengths) {
            std::vector<unsigned char> input(size);
            std::uint32_t seed = 123U;
            for (auto& byte : input) {
                seed = seed * 1664525U + 1013904223U;
                byte = static_cast<unsigned char>((seed >> 24U) % alphabet);
            }
            require_suffix_consumers(input);
        }
    }
}

// Purpose: Require explicit group progress to reject malformed indices before out-of-range access.
// Inputs: Independent known rank groups and malformed/unterminated signed group fixtures.
// Outputs: Correct ranks for controls and bounded error returns with surrounding canaries intact.
TEST_CASE(zstd_suffix_progress_rejects_malformed_groups) {
    REQUIRE_EQ(sz_rank_partial_suffixes(nullptr, nullptr, 0), 1);
    REQUIRE_EQ(sz_rank_partial_suffixes(nullptr, nullptr, -1), 0);
    {
        std::array<int, 3> sorted{0, 2, 1};
        std::array<int, 3> ranks{};
        REQUIRE_EQ(sz_rank_partial_suffixes(sorted.data(), ranks.data(), 3), 1);
        REQUIRE_TRUE((ranks == std::array<int, 3>{0, 2, 1}));
    }
    {
        std::array<int, 3> sorted{0, ~1, ~2};
        std::array<int, 3> ranks{};
        REQUIRE_EQ(sz_rank_partial_suffixes(sorted.data(), ranks.data(), 3), 1);
        REQUIRE_TRUE((ranks == std::array<int, 3>{2, 2, 2}));
    }
    for (const auto marker : {1, ~0, ~1, std::numeric_limits<int>::min()}) {
        std::array<int, 3> sorted{-997, marker, -997};
        std::array<int, 3> ranks{-998, -999, -998};
        REQUIRE_EQ(sz_rank_partial_suffixes(sorted.data() + 1, ranks.data() + 1, 1), 0);
        REQUIRE_EQ(sorted.front(), -997);
        REQUIRE_EQ(sorted.back(), -997);
        REQUIRE_EQ(ranks.front(), -998);
        REQUIRE_EQ(ranks.back(), -998);
    }
}
