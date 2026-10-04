#include "test_util.hpp"
#include "huffman_table_probe.h"
#include "zstd_errors.h"
#include "zstd.h"

#include <array>
#include <cstdint>
#include <limits>

// Purpose: Verify ranked Huffman entry semantics across all fill widths using an independent index mapping.
// Inputs: Deterministic valid rank distributions, ignored symbols and guard records beyond logical capacity.
// Outputs: Requires exact symbol/bit pairs and unchanged guard entries for table logs one through eight.
TEST_CASE(zstd_huffman_typed_fill_rank_oracle) {
    for (unsigned log = 1; log <= 8; ++log) {
        const auto capacity = std::size_t{1} << log;
        std::vector<sz_huf_x1_entry> records(capacity + 2, {0xA5, 0x5A});
        std::array<std::uint32_t, 13> ranks{};
        ranks[0] = 2;
        if (log == 1) {
            ranks[1] = 2;
        } else {
            ranks[log] = 1;
            ranks[log - 1] = 2;
        }
        const std::array<unsigned char, 5> symbols{255, 254, 7, 19, 31};
        const auto count = log == 1 ? 4U : 5U;
        REQUIRE_EQ(sz_huf_fill_x1(records.data(), records.size(), capacity, symbols.data(), count, ranks.data(),
                                  ranks.size(), log),
                   0U);
        for (std::size_t entry = 0; entry < capacity; ++entry) {
            std::size_t base = 0;
            std::size_t symbol = ranks[0];
            for (unsigned weight = 1; weight <= log; ++weight) {
                const auto width = std::size_t{1} << (weight - 1);
                const auto region = width * ranks[weight];
                if (entry < base + region) {
                    REQUIRE_EQ(records[entry].symbol, symbols[symbol + (entry - base) / width]);
                    REQUIRE_EQ(records[entry].nb_bits, log + 1 - weight);
                    break;
                }
                base += region;
                symbol += ranks[weight];
            }
        }
        for (auto guard = capacity; guard < records.size(); ++guard) {
            REQUIRE_EQ(records[guard].nb_bits, 0xA5U);
            REQUIRE_EQ(records[guard].symbol, 0x5AU);
        }
    }
}

// Purpose: Reject malformed ranked representations before publishing any entry.
// Inputs: Truncation, excess/missing ranks, overflowing counts and insufficient logical table capacity.
// Outputs: Requires errors and every original data/guard record unchanged.
TEST_CASE(zstd_huffman_typed_fill_rejects_geometry) {
    std::array<sz_huf_x1_entry, 258> records{};
    const std::array<unsigned char, 2> symbols{7, 19};
    const auto reject = [&](std::array<std::uint32_t, 13> ranks, std::size_t capacity, std::size_t count) {
        for (auto& entry : records) {
            entry = {0xA5, 0x5A};
        }
        REQUIRE_TRUE(ZSTD_isError(sz_huf_fill_x1(records.data(), records.size(), capacity, symbols.data(), count,
                                                 ranks.data(), ranks.size(), 8U)));
        for (const auto& entry : records) {
            REQUIRE_EQ(entry.nb_bits, 0xA5U);
            REQUIRE_EQ(entry.symbol, 0x5AU);
        }
    };
    std::array<std::uint32_t, 13> ranks{};
    ranks[8] = 2;
    reject(ranks, 255U, 2U);
    reject(ranks, 256U, 1U);
    ranks[0] = 3;
    reject(ranks, 256U, 2U);
    ranks[0] = 0;
    ranks[8] = 1;
    reject(ranks, 256U, 2U);
    ranks[8] = std::numeric_limits<std::uint32_t>::max();
    reject(ranks, 256U, 2U);
}
