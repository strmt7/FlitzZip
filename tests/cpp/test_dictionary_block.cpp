#include "core/dictionary_block.hpp"
#include "test_util.hpp"

#include <array>
#include <string>

namespace {

// Purpose: Build dictionary framing independently of the production scanner and encoder.
// Inputs: Three cumulative offsets and a small uninterpreted encoded-body length.
// Outputs: Returns serialized framing bytes; these fixtures test metadata, not LZ4 decompression.
std::vector<std::byte> dictionary_framing_fixture(std::array<std::uint32_t, 3> offsets, std::size_t body_size = 7U) {
    std::vector<std::byte> payload;
    for (const auto offset : offsets) {
        for (unsigned byte = 0U; byte < 4U; ++byte) {
            payload.push_back(static_cast<std::byte>((offset >> (8U * byte)) & 0xFFU));
        }
    }
    payload.resize(payload.size() + body_size, std::byte{0x41});
    return payload;
}

// Purpose: Require malformed framing to fail through both allocation-free and collecting scanner modes.
// Inputs: Serialized fixture bytes, exact decoded size and a mode selecting private span collection.
// Outputs: Returns the ArchiveError message; fails the test if framing is accepted or another error escapes.
std::string dictionary_framing_error(std::span<const std::byte> payload, std::uint32_t decoded_size, bool collect) {
    try {
        if (collect) {
            static_cast<void>(superzip::parse_dictionary_segments(payload, decoded_size));
        } else {
            superzip::scan_dictionary_segments(payload, decoded_size);
        }
    } catch (const superzip::ArchiveError& error) {
        return error.what();
    }
    throw std::runtime_error("Malformed dictionary framing was accepted");
}

}  // namespace

// Purpose: Preserve exact parser extents and append semantics across a full segment and short coded tail.
// Inputs: Independently serialized two-segment framing and an existing scratch sentinel.
// Outputs: Both APIs accept identical extents; collection preserves the sentinel and advertised capacity.
TEST_CASE(dictionary_block_scan_preserves_extents_and_existing_scratch) {
    const auto payload = dictionary_framing_fixture({0U, 3U, 7U});
    constexpr auto decoded_size = superzip::kGpuDictionarySegmentBytes + 11U;
    superzip::scan_dictionary_segments(payload, decoded_size);
    const auto spans = superzip::parse_dictionary_segments(payload, decoded_size);
    REQUIRE_EQ(spans.size(), 2U);
    REQUIRE_EQ(spans[0].encoded_offset, 12U);
    REQUIRE_EQ(spans[0].encoded_size, 3U);
    REQUIRE_EQ(spans[0].decoded_offset, 0U);
    REQUIRE_EQ(spans[0].decoded_size, superzip::kGpuDictionarySegmentBytes);
    REQUIRE_EQ(spans[1].encoded_offset, 15U);
    REQUIRE_EQ(spans[1].encoded_size, 4U);
    REQUIRE_EQ(spans[1].decoded_offset, superzip::kGpuDictionarySegmentBytes);
    REQUIRE_EQ(spans[1].decoded_size, 11U);
    std::vector<superzip::DictionarySegmentSpan> scratch{{123U, 4U, 456U, 5U}};
    scratch.reserve(3U);
    const auto* storage = scratch.data();
    superzip::scan_dictionary_segments(payload, decoded_size, &scratch);
    REQUIRE_TRUE(scratch.data() == storage);
    REQUIRE_EQ(scratch.size(), 3U);
    REQUIRE_EQ(scratch.front().encoded_offset, 123U);
    REQUIRE_EQ(scratch.front().decoded_offset, 456U);
    for (std::size_t index = 0U; index < spans.size(); ++index) {
        REQUIRE_EQ(scratch[index + 1U].encoded_offset, spans[index].encoded_offset);
        REQUIRE_EQ(scratch[index + 1U].encoded_size, spans[index].encoded_size);
        REQUIRE_EQ(scratch[index + 1U].decoded_offset, spans[index].decoded_offset);
        REQUIRE_EQ(scratch[index + 1U].decoded_size, spans[index].decoded_size);
    }
}

// Purpose: Keep complete framing rejection identical in collecting and allocation-free validation.
// Inputs: Zero/oversized decoded extents, incomplete tables, invalid offsets, oversized tails and trailing bytes.
// Outputs: Both modes reject every fixture with the independently expected error category.
TEST_CASE(dictionary_block_scan_rejects_malformed_framing_in_both_modes) {
    constexpr auto size = superzip::kGpuDictionarySegmentBytes + 11U;
    struct Fixture {
        std::vector<std::byte> payload;
        std::uint32_t decoded_size;
        std::string error;
    };
    const std::array cases{
        Fixture{{}, size, "metadata is invalid"},
        Fixture{dictionary_framing_fixture({0U, 3U, 7U}), 0U, "decoded size is invalid"},
        Fixture{dictionary_framing_fixture({0U, 3U, 7U}), superzip::kMaxArchiveBlockBytes + 1U,
                "decoded size is invalid"},
        Fixture{dictionary_framing_fixture({0U, 3U, 7U}, 0U), size, "metadata is invalid"},
        Fixture{dictionary_framing_fixture({1U, 3U, 7U}), size, "table must start at zero"},
        Fixture{dictionary_framing_fixture({0U, 0U, 7U}), size, "segment extent is invalid"},
        Fixture{dictionary_framing_fixture({0U, 3U, 2U}), size, "segment extent is invalid"},
        Fixture{dictionary_framing_fixture({0U, 3U, 8U}), size, "segment extent is invalid"},
        Fixture{dictionary_framing_fixture({0U, 3U, 6U}), size, "payload has trailing bytes"},
        Fixture{dictionary_framing_fixture({0U, 3U, 32U}, 32U), size, "segment extent is invalid"},
    };
    for (const auto& fixture : cases) {
        const auto expected = "GPU dictionary block " + fixture.error;
        REQUIRE_EQ(dictionary_framing_error(fixture.payload, fixture.decoded_size, false), expected);
        REQUIRE_EQ(dictionary_framing_error(fixture.payload, fixture.decoded_size, true), expected);
    }
}
