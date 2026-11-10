#include "core/archive_block_version.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using flitzzip::BlockDescriptor;
using flitzzip::BlockKind;

void require_version_rejection(const BlockDescriptor& block, std::span<const std::byte> payload,
                               std::string_view message = {}) {
    bool rejected = false;
    try {
        (void)flitzzip::inspect_encoded_block_version(block, payload);
    } catch (const flitzzip::ArchiveError& error) {
        if (!message.empty()) {
            REQUIRE_EQ(std::string_view(error.what()), message);
        }
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

struct CompositionFixture {
    BlockKind kind;
    std::uint32_t decoded;
    std::uint32_t minimum;
    std::vector<std::byte> bytes;
    std::size_t header;
};

// Independent fixed headers: fill then a pattern, a whole-plane pattern, and
// a two-plane fill/raw frame. The last frame's raw plane has exactly 64 bytes.
std::array<CompositionFixture, 3> composition_version_fixtures() {
    std::vector<std::byte> contexts{
        std::byte{2},  std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{65},
        std::byte{0},  std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{64}, std::byte{0}, std::byte{0}, std::byte{0},
    };
    contexts.resize(80U, std::byte{0x7F});
    return {{
        {BlockKind::GpuCompound,
         4096U,
         9U,
         {std::byte{1}, std::byte{3}, std::byte{65}, std::byte{0}, std::byte{64}, std::byte{0}, std::byte{0},
          std::byte{0}},
         8U},
        {BlockKind::GpuBytePlane,
         4096U,
         10U,
         {std::byte{2}, std::byte{3}, std::byte{0}, std::byte{0}, std::byte{65}, std::byte{66}},
         4U},
        {BlockKind::GpuBytePlaneContexts, 128U, 11U, std::move(contexts), 16U},
    }};
}

}  // namespace

TEST_CASE(flzip_encoded_version_uses_independent_kind_matrix_and_owner_offsets) {
    constexpr std::array<std::uint32_t, 14> minima{1U, 1U, 1U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U};
    const std::vector<std::byte> owner(300U, std::byte{0xA5});
    for (std::uint32_t kind = 0; kind <= 255U; ++kind) {
        if (kind >= 11U && kind <= 13U) {
            continue;  // Composition headers have separate independent fixtures.
        }
        for (const auto length : {2U, 256U, 257U}) {
            const BlockDescriptor block{.kind = static_cast<BlockKind>(kind),
                                        .uncompressed_len = 4096U,
                                        .encoded_offset = 9U,
                                        .encoded_len = length};
            if (kind >= minima.size()) {
                require_version_rejection(block, owner, "archive block has unknown encoding kind");
            } else {
                const auto inspected = flitzzip::inspect_encoded_block_version(block, owner);
                const auto expected = kind == 3U && length > 256U ? 4U : minima[kind];
                REQUIRE_EQ(inspected.minimum_version, expected);
                REQUIRE_EQ(inspected.payload.data(), owner.data() + 9U);
                REQUIRE_EQ(inspected.payload.size(), length);
            }
        }
    }
}

TEST_CASE(flzip_encoded_version_rejects_out_of_window_extents_and_handles_empty_fill) {
    const std::array<std::byte, 4> owner{};
    for (const auto offset : std::array<std::uint64_t, 2>{5U, std::numeric_limits<std::uint64_t>::max()}) {
        require_version_rejection({.kind = BlockKind::Raw, .encoded_offset = offset, .encoded_len = 1U}, owner,
                                  "archive block version inspection exceeds its payload window");
    }
    for (const auto length : {1U, std::numeric_limits<std::uint32_t>::max()}) {
        require_version_rejection({.kind = BlockKind::Raw, .encoded_offset = 4U, .encoded_len = length}, owner,
                                  "archive block version inspection exceeds its payload window");
    }
    for (const auto bytes : {std::span<const std::byte>{}, std::span<const std::byte>(owner)}) {
        const auto inspected =
            flitzzip::inspect_encoded_block_version({.kind = BlockKind::Fill, .encoded_offset = bytes.size()}, bytes);
        REQUIRE_EQ(inspected.minimum_version, 1U);
        REQUIRE_TRUE(inspected.payload.empty());
    }
}

TEST_CASE(flzip_encoded_version_reads_actual_composition_headers_and_borrowed_views) {
    for (const auto& fixture : composition_version_fixtures()) {
        std::vector<std::byte> owner(9U, std::byte{0xA5});
        owner.insert(owner.end(), fixture.bytes.begin(), fixture.bytes.end());
        owner.resize(owner.size() + 7U, std::byte{0x5A});
        const BlockDescriptor block{.kind = fixture.kind,
                                    .uncompressed_len = fixture.decoded,
                                    .encoded_offset = 9U,
                                    .encoded_len = static_cast<std::uint32_t>(fixture.bytes.size())};
        const auto before = owner;
        const auto inspected = flitzzip::inspect_encoded_block_version(block, owner);
        REQUIRE_EQ(inspected.minimum_version, fixture.minimum);
        REQUIRE_EQ(inspected.payload.data(), owner.data() + 9U);
        REQUIRE_TRUE(
            std::equal(inspected.payload.begin(), inspected.payload.end(), fixture.bytes.begin(), fixture.bytes.end()));
        REQUIRE_EQ(owner, before);
    }
}

TEST_CASE(flzip_encoded_version_refuses_truncated_and_noncanonical_composition_headers) {
    for (const auto& fixture : composition_version_fixtures()) {
        const auto refused_prefixes =
            fixture.kind == BlockKind::GpuBytePlaneContexts ? fixture.bytes.size() : fixture.header;
        for (std::size_t length = 0; length < refused_prefixes; ++length) {
            require_version_rejection({.kind = fixture.kind,
                                       .uncompressed_len = fixture.decoded,
                                       .encoded_len = static_cast<std::uint32_t>(length)},
                                      std::span(fixture.bytes).first(length));
        }
        const BlockDescriptor block{.kind = fixture.kind,
                                    .uncompressed_len = fixture.decoded,
                                    .encoded_len = static_cast<std::uint32_t>(fixture.bytes.size())};
        auto altered = fixture.bytes;
        altered[3] = std::byte{1};
        require_version_rejection(block, altered);
        altered = fixture.bytes;
        const auto stage = fixture.kind == BlockKind::GpuCompound    ? 0U
                           : fixture.kind == BlockKind::GpuBytePlane ? 1U
                                                                     : 4U;
        for (const auto forbidden : {std::byte{14}, std::byte{11}, std::byte{12}, std::byte{13}, std::byte{255}}) {
            altered[stage] = forbidden;
            require_version_rejection(block, altered);
        }
    }
}
