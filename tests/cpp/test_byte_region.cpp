#include "test_util.hpp"

#include "core/byte_region.hpp"

#include <array>
#include <cstddef>
#include <span>

TEST_CASE(byte_region_alias_admission_matches_independent_byte_membership) {
    std::array<std::byte, 32> owner{};
    struct Interval {
        std::size_t begin;
        std::size_t count;
    };
    constexpr std::array intervals{Interval{0U, 0U},  Interval{0U, 1U},   Interval{1U, 0U},  Interval{1U, 1U},
                                   Interval{0U, 16U}, Interval{16U, 16U}, Interval{15U, 2U}, Interval{31U, 1U},
                                   Interval{32U, 0U}, Interval{8U, 8U},   Interval{7U, 9U},  Interval{0U, 32U}};
    for (const auto a : intervals) {
        for (const auto b : intervals) {
            bool shares_byte = false;
            for (std::size_t index = 0U; index < owner.size(); ++index) {
                shares_byte |=
                    index >= a.begin && index < a.begin + a.count && index >= b.begin && index < b.begin + b.count;
            }
            const auto first = std::span<const std::byte>(owner).subspan(a.begin, a.count);
            const auto second = std::span<const std::byte>(owner).subspan(b.begin, b.count);
            REQUIRE_EQ(flitzzip::byte_regions_overlap(first, second), shares_byte);
        }
    }
    std::array<std::byte, 32> other{};
    REQUIRE_TRUE(!flitzzip::byte_regions_overlap(owner, other));
    REQUIRE_TRUE(!flitzzip::byte_regions_overlap(other, owner));
    REQUIRE_TRUE(!flitzzip::byte_regions_overlap({}, owner));
    REQUIRE_TRUE(!flitzzip::byte_regions_overlap(owner, {}));
}
