#include "test_util.hpp"

extern "C" {
#include "dictionary_merge_probe.h"
}

#include <array>
#include <limits>
#include <span>

namespace {

using Item = sz_dictionary_item;
constexpr auto sentinel_savings = std::numeric_limits<std::uint32_t>::max();

// Purpose: Observe exact merger results and every table field, including the sentinel.
// Inputs: before/expected describe complete sorted fixtures; source owns valid guarded source spans.
// Outputs: Requires the expected destination rank and byte-independent field identity.
template <std::size_t Count>
void require_merge(std::array<Item, Count> before, Item candidate, std::uint32_t skip,
                   const std::array<unsigned char, 256>& source, std::uint32_t expected_rank,
                   const std::array<Item, Count>& expected) {
    REQUIRE_EQ(sz_dictionary_merge(before.data(), before.size(), candidate, skip, source.data()), expected_rank);
    for (std::size_t index = 0; index < before.size(); ++index) {
        REQUIRE_EQ(before[index].position, expected[index].position);
        REQUIRE_EQ(before[index].length, expected[index].length);
        REQUIRE_EQ(before[index].savings, expected[index].savings);
    }
}

// Purpose: Make source spans distinguishable for overlap tests without triggering shifted-content inclusion.
// Inputs: None. Outputs: Returns a bounded source fixture whose positions 0-255 contain different bytes.
std::array<unsigned char, 256> distinct_source() {
    std::array<unsigned char, 256> source{};
    for (std::size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<unsigned char>(index);
    }
    return source;
}

}  // namespace

// Purpose: Preserve tail-overlap arithmetic, candidate skipping, stable ties and rank movement.
// Inputs: Sorted tables with overlap at the first/last entry or exactly at the new extent's end.
// Outputs: Requires explicit destination indices and complete expected sorted tables.
TEST_CASE(zstd_dictionary_tail_overlap_ranking) {
    const auto source = distinct_source();
    const std::array<Item, 4> table{{{4, 0, sentinel_savings}, {100, 8, 100}, {120, 8, 95}, {40, 12, 90}}};
    require_merge(table, {32, 16, 32}, 0, source, 1,
                  std::array<Item, 4>{{{4, 0, sentinel_savings}, {32, 20, 108}, {100, 8, 100}, {120, 8, 95}}});
    auto tied = table;
    tied[1].savings = 108;
    tied[2].savings = 108;
    require_merge(tied, {32, 16, 32}, 0, source, 3,
                  std::array<Item, 4>{{{4, 0, sentinel_savings}, {100, 8, 108}, {120, 8, 108}, {32, 20, 108}}});
    require_merge(table, {32, 16, 32}, 3, source, 0, table);
    const std::array<Item, 3> candidates{{{3, 0, sentinel_savings}, {40, 8, 100}, {48, 4, 90}}};
    require_merge(candidates, {32, 16, 32}, 1, source, 1,
                  std::array<Item, 3>{{{3, 0, sentinel_savings}, {32, 20, 124}, {40, 8, 100}}});
    const std::array<Item, 2> first{{{2, 0, sentinel_savings}, {40, 12, 90}}};
    require_merge(first, {32, 16, 32}, 0, source, 1, std::array<Item, 2>{{{2, 0, sentinel_savings}, {32, 20, 108}}});
}

// Purpose: Preserve front extension, adjacency and containment, including zero added-length ranking.
// Inputs: A sorted table and source intervals on either side of the existing item boundary.
// Outputs: Requires exact length/savings arithmetic, stable ties and untouched nonselected entries.
TEST_CASE(zstd_dictionary_front_overlap_ranking) {
    const auto source = distinct_source();
    const std::array<Item, 3> table{{{3, 0, sentinel_savings}, {100, 8, 100}, {40, 12, 90}}};
    require_merge(table, {44, 20, 20}, 0, source, 1,
                  std::array<Item, 3>{{{3, 0, sentinel_savings}, {40, 24, 104}, {100, 8, 100}}});
    require_merge(table, {44, 4, 8}, 0, source, 2, table);
    const std::array<Item, 3> adjacency{{{3, 0, sentinel_savings}, {100, 8, 99}, {40, 8, 90}}};
    require_merge(adjacency, {48, 8, 8}, 0, source, 2,
                  std::array<Item, 3>{{{3, 0, sentinel_savings}, {100, 8, 99}, {40, 16, 99}}});
    require_merge(table, {44, 20, 20}, 2, source, 0, table);
    require_merge(table, {60, 8, 8}, 0, source, 0, table);
    const std::array<Item, 1> empty{{{1, 0, sentinel_savings}}};
    require_merge(empty, {44, 20, 20}, 0, source, 0, empty);
}

// Purpose: Preserve the separate shifted-content inclusion path and its minimum-one-byte growth rule.
// Inputs: Equal-content spans without coordinate overlap; candidate lengths straddle the existing length.
// Outputs: Requires the original one-byte extension/clamping and returned candidate index.
TEST_CASE(zstd_dictionary_shifted_content_inclusion) {
    std::array<unsigned char, 256> source{};
    const std::array<Item, 2> table{{{2, 0, sentinel_savings}, {60, 12, 90}}};
    require_merge(table, {20, 16, 16}, 0, source, 1, std::array<Item, 2>{{{2, 0, sentinel_savings}, {20, 13, 94}}});
    require_merge(table, {20, 12, 12}, 0, source, 1, std::array<Item, 2>{{{2, 0, sentinel_savings}, {20, 12, 91}}});
    require_merge(table, {20, 8, 8}, 0, source, 1, std::array<Item, 2>{{{2, 0, sentinel_savings}, {20, 8, 91}}});
    source[61] = 1;
    require_merge(table, {20, 16, 16}, 0, source, 0, table);
    source.fill(0);
    require_merge(table, {20, 16, 16}, 1, source, 0, table);
}
