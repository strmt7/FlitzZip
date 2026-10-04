#include "test_util.hpp"
#include "fault_allocator.h"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace {

constexpr std::array<unsigned, 3> kVersions{5, 6, 7};
constexpr unsigned char kCanary = 0xCC;

// Purpose: Compare canonical sequence copying against a byte-at-a-time LZ history oracle.
// Inputs: version is shipped; dictionary/prefix/literal extents and offset describe valid live history.
// Outputs: Requires exact output bytes and unchanged storage outside the advertised output capacity.
void require_sequence_readback(unsigned version, std::size_t dictionary_bytes, std::size_t prefix,
                               std::size_t literal_length, std::size_t match_length, std::size_t offset) {
    std::array<unsigned char, 80> storage{};
    std::array<unsigned char, 16> dictionary{};
    std::array<unsigned char, 24> literals{};
    storage.fill(kCanary);
    auto* const output = storage.data() + 8;
    for (std::size_t i = 0; i < dictionary.size(); ++i) {
        dictionary[i] = static_cast<unsigned char>('a' + i);
    }
    for (std::size_t i = 0; i < prefix; ++i) {
        output[i] = static_cast<unsigned char>('A' + i);
    }
    for (std::size_t i = 0; i < literals.size(); ++i) {
        literals[i] = static_cast<unsigned char>('0' + i % 10);
    }
    std::vector<unsigned char> history(dictionary.begin(), dictionary.begin() + dictionary_bytes);
    history.insert(history.end(), output, output + prefix);
    history.insert(history.end(), literals.begin(), literals.begin() + literal_length);
    for (std::size_t i = 0; i < match_length; ++i) {
        const auto next = history[history.size() - offset];
        history.push_back(next);
    }
    const auto capacity = prefix + literal_length + std::max<std::size_t>(8, match_length);
    const auto result = sz_legacy_sequence(version, output, capacity, prefix, literals.data(), literal_length,
                                           dictionary_bytes == 0 ? nullptr : dictionary.data(), dictionary_bytes,
                                           literal_length, match_length, offset);
    REQUIRE_EQ(sz_legacy_result_is_error(result), 0);
    REQUIRE_EQ(result, literal_length + match_length);
    REQUIRE_TRUE(std::equal(history.begin() + dictionary_bytes, history.end(), output));
    REQUIRE_TRUE(std::all_of(storage.begin(), storage.begin() + 8, [](auto byte) { return byte == kCanary; }));
    REQUIRE_TRUE(std::all_of(storage.begin() + 8 + capacity, storage.end(), [](auto byte) { return byte == kCanary; }));
}

}  // namespace

// Purpose: Preserve short-distance, dictionary-only, crossing and prefix sequence semantics.
// Inputs: Three shipped helpers, history sizes 0-16, literal lengths 0/1/4 and match lengths 3/8/17/31.
// Outputs: Compares every valid offset against an independent recurrence oracle and capacity canaries.
TEST_CASE(zstd_legacy_sequence_bounded_history_readback) {
    for (const auto version : kVersions) {
        for (std::size_t dictionary = 0; dictionary <= 16; ++dictionary) {
            for (const std::size_t prefix : {0U, 1U, 8U, 16U}) {
                for (const std::size_t literals : {0U, 1U, 4U}) {
                    for (std::size_t offset = 1; offset <= dictionary + prefix + literals; ++offset) {
                        for (const std::size_t match : {3U, 8U, 17U, 31U}) {
                            require_sequence_readback(version, dictionary, prefix, literals, match, offset);
                        }
                    }
                }
            }
        }
    }
}

// Purpose: Reject length overflow, impossible history and invalid literal extents before output mutation.
// Inputs: Canonical helpers receive each boundary separately, including SIZE_MAX and nullable empty buffers.
// Outputs: Requires native error identity and unchanged output for every rejected sequence.
TEST_CASE(zstd_legacy_sequence_rejects_malformed_extents) {
    constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
    struct Case {
        std::size_t capacity;
        std::size_t literal_bytes;
        std::size_t literal_length;
        std::size_t match_length;
        std::size_t offset;
    };
    constexpr std::array cases{Case{64, 8, maximum, 1, 1}, Case{64, 8, 1, maximum, 1}, Case{64, 8, 1, 3, maximum},
                               Case{64, 8, 9, 3, 1},       Case{64, 8, 1, 3, 0},       Case{64, 8, 1, 3, 2},
                               Case{7, 8, 1, 3, 1}};
    const std::array<unsigned char, 16> literals{'A'};
    for (const auto version : kVersions) {
        for (const auto& item : cases) {
            std::array<unsigned char, 64> output{};
            output.fill(kCanary);
            const auto before = output;
            const auto result =
                sz_legacy_sequence(version, output.data(), item.capacity, 0, literals.data(), item.literal_bytes,
                                   nullptr, 0, item.literal_length, item.match_length, item.offset);
            REQUIRE_EQ(sz_legacy_result_is_error(result), 1);
            REQUIRE_EQ(output, before);
        }
        REQUIRE_EQ(sz_legacy_result_is_error(
                       sz_legacy_sequence(version, nullptr, 0, 0, nullptr, 0, nullptr, 0, maximum, 3, 1)),
                   1);
    }
}

// Purpose: Prevent encoded errors from becoming v0.7 history endpoints.
// Inputs: A malformed block goes through the actual exported block function in canonical fault-test objects.
// Outputs: Requires native error propagation, preserved history and complete context release.
TEST_CASE(zstd_legacy_v07_block_error_preserves_history) {
    REQUIRE_TRUE(sz_fault_reset(0));
    REQUIRE_EQ(sz_legacy_v07_block_error_history(), 1);
    REQUIRE_EQ(sz_fault_live_allocations(), 0U);
    REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
}
