#include "test_util.hpp"
#include "fault_allocator.h"
#include "zstd_legacy_fixture.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <vector>

namespace {

constexpr std::array<unsigned, 3> kVersions{5, 6, 7};
constexpr unsigned char kCanary = 0xCC;

struct PreparedOwner {
    void* context;
    unsigned version;
    // Purpose: Release prepared production state on every assertion path.
    // Inputs: Original context/version. Outputs: Matching complete owner destruction.
    ~PreparedOwner() {
        sz_legacy_free_owned(context, version);
    }
};

struct DestinationAllocator {
    std::size_t allocations{};
    std::size_t releases{};
};

// Purpose: Acquire tracked destination storage with observable allocator identity.
// Inputs: Live callback state and requested bytes. Outputs: Tracked storage or null, with an acquisition count.
void* allocate_destination(void* opaque, std::size_t bytes) {
    auto& state = *static_cast<DestinationAllocator*>(opaque);
    auto* result = sz_fault_malloc(bytes);
    if (result != nullptr)
        ++state.allocations;
    return result;
}

// Purpose: Observe matching destruction under the destination's original callback identity.
// Inputs: Callback state and live owned storage. Outputs: Tracked release and count.
void release_destination(void* opaque, void* address) {
    ++static_cast<DestinationAllocator*>(opaque)->releases;
    sz_fault_free(address);
}

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

// Purpose: Recover exact literals and dictionary bytes after cloning and expiry of every original source.
// Inputs: Every shipped decoder, fixed independent byte oracles and a distinct v07 destination allocator.
// Outputs: Requires byte identity, surrounding canaries and matching complete destruction.
TEST_CASE(zstd_legacy_prepared_history_and_literals_outlive_sources) {
    for (const auto version : kVersions) {
        DestinationAllocator state;
        const sz_legacy_allocator allocator =
            version == 7 ? sz_legacy_allocator{allocate_destination, release_destination, &state}
                         : sz_legacy_allocator{};
        REQUIRE_TRUE(sz_fault_reset(0));
        {
            std::array<unsigned char, 8> dictionary{'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
            std::array<unsigned char, 4> literals{0x83, 'A', 'B', 'C'};
            PreparedOwner owner{sz_legacy_prepare_owned(version, dictionary.data(), dictionary.size(), literals.data(),
                                                        literals.size(), allocator),
                                version};
            REQUIRE_TRUE(owner.context != nullptr);
            dictionary.fill('x');
            literals.fill('y');
            std::array<unsigned char, 24> output{};
            output.fill(kCanary);
            REQUIRE_EQ(sz_legacy_read_owned(version, owner.context, output.data() + 4, 16, 0, 0, 12, 8), 12U);
            constexpr std::array<unsigned char, 12> dictionary_expected{'a', 'b', 'c', 'd', 'e', 'f',
                                                                        'g', 'h', 'a', 'b', 'c', 'd'};
            REQUIRE_TRUE(std::equal(dictionary_expected.begin(), dictionary_expected.end(), output.begin() + 4));
            REQUIRE_EQ(output[3], kCanary);
            REQUIRE_EQ(output[20], kCanary);
            output.fill(kCanary);
            REQUIRE_EQ(sz_legacy_read_owned(version, owner.context, output.data() + 4, 16, 0, 3, 3, 1), 6U);
            constexpr std::array<unsigned char, 6> literals_expected{'A', 'B', 'C', 'C', 'C', 'C'};
            REQUIRE_TRUE(std::equal(literals_expected.begin(), literals_expected.end(), output.begin() + 4));
            REQUIRE_EQ(output[3], kCanary);
            REQUIRE_EQ(output[20], kCanary);
        }
        REQUIRE_EQ(sz_fault_live_allocations(), 0U);
        REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
        REQUIRE_EQ(state.allocations, state.releases);
        if (version == 7)
            REQUIRE_EQ(state.allocations, 4U);
    }
}

// Purpose: Reject every prepared ownership acquisition failure with complete matching rollback.
// Inputs: Three raw decoder records per context, dictionary capture and independent history clone.
// Outputs: Requires null on every injected failure and complete ownership after a successful retry.
TEST_CASE(zstd_legacy_prepared_owner_allocation_failures) {
    const std::array<unsigned char, 8> dictionary{'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
    const std::array<unsigned char, 4> literals{0x83, 'A', 'B', 'C'};
    for (const auto version : kVersions) {
        for (std::size_t failure = 1; failure <= 9; ++failure) {
            REQUIRE_TRUE(sz_fault_reset(failure));
            {
                PreparedOwner owner{sz_legacy_prepare_owned(version, dictionary.data(), dictionary.size(),
                                                            literals.data(), literals.size(), {}),
                                    version};
                REQUIRE_EQ(owner.context == nullptr, failure <= 8);
            }
            REQUIRE_EQ(sz_fault_live_allocations(), 0U);
            REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
        }
    }
}

// Purpose: Return prepared-clone allocation failure before writing any frame output, then permit an explicit retry.
// Inputs: Exact independent legacy golden frames, valid dictionaries and the actual clone acquisition boundary.
// Outputs: Requires untouched output on failure and byte-exact golden-frame recovery after successful retry.
TEST_CASE(zstd_legacy_prepared_clone_failure_blocks_frame_output) {
    const std::array<unsigned char, 8> dictionary{'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
    const std::array<unsigned char, 4> literals{0x83, 'A', 'B', 'C'};
    for (const auto version : kVersions) {
        REQUIRE_TRUE(sz_fault_reset(0));
        {
            PreparedOwner owner{sz_legacy_prepare_owned(version, dictionary.data(), dictionary.size(), literals.data(),
                                                        literals.size(), {}),
                                version};
            REQUIRE_TRUE(owner.context != nullptr);
            const auto extent = superzip_test::kLegacyFrames[version - 4U];
            const auto frame = std::span(superzip_test::kLegacyCompressed).subspan(extent.offset, extent.size);
            std::array<unsigned char, 1024> output{};
            output.fill(kCanary);
            const auto before = output;
            REQUIRE_TRUE(sz_fault_fail_after(5));
            const auto failed =
                sz_legacy_clone_decode(version, owner.context, output.data(), output.size(), dictionary.data(),
                                       dictionary.size(), frame.data(), frame.size());
            REQUIRE_EQ(sz_legacy_result_is_error(failed), 1);
            REQUIRE_EQ(output, before);
            REQUIRE_TRUE(sz_fault_fail_after(0));
            const auto result =
                sz_legacy_clone_decode(version, owner.context, output.data(), output.size(), dictionary.data(),
                                       dictionary.size(), frame.data(), frame.size());
            REQUIRE_EQ(sz_legacy_result_is_error(result), 0);
            REQUIRE_EQ(result, superzip_test::kLegacyExpected.size());
            REQUIRE_TRUE(std::equal(superzip_test::kLegacyExpected.begin(), superzip_test::kLegacyExpected.end(),
                                    output.begin()));
        }
        REQUIRE_EQ(sz_fault_live_allocations(), 0U);
        REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
    }
}
