#include "test_util.hpp"
#include "rans_fixtures.hpp"

#include "core/rans_block.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

// A rejection must be an archive error, not a bounds exception, allocation
// failure or unrelated test failure. Each caller supplies its actual boundary.
template <typename Action> void require_rans_rejection(Action action) {
    bool rejected = false;
    try {
        action();
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Little-endian fixture emission is independent of the production parser.
void append_rans_word(std::vector<std::byte>& bytes, std::uint32_t word) {
    for (unsigned int i = 0U; i < 4U; ++i) {
        bytes.push_back(static_cast<std::byte>((word >> (8U * i)) & 255U));
    }
}

// Legal but unused rows stress admission without changing a one-symbol stream.
// Dense rows have literal unit frequencies, not production-normalized values.
std::vector<std::byte> rans_unused_row_fixture(bool all_contexts) {
    const auto decoded = all_contexts ? 65536U : 1024U;
    const auto segments = decoded / 4096U + (decoded % 4096U != 0U ? 1U : 0U);
    std::vector<std::byte> bytes{std::byte{8}, std::byte{8}, std::byte{12},
                                 static_cast<std::byte>(all_contexts ? 255U : 1U)};
    if (all_contexts) {
        for (unsigned int context = 0U; context < 256U; ++context) {
            bytes.insert(bytes.end(), {static_cast<std::byte>(context), std::byte{0}, std::byte{0}});
        }
    } else {
        bytes.insert(bytes.end(), {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{7}, std::byte{255}});
        for (unsigned int symbol = 0U; symbol < 256U; ++symbol) {
            bytes.insert(bytes.end(), {static_cast<std::byte>(symbol), std::byte{1}, std::byte{0}});
        }
    }
    for (unsigned int i = 0U; i <= segments; ++i) {
        append_rans_word(bytes, i * 4U);
    }
    for (unsigned int i = 0U; i < segments; ++i) {
        append_rans_word(bytes, 1U << 23U);
    }
    return bytes;
}

}  // namespace

TEST_CASE(gpu_rans_reader_shared_singleton_fixture_matches_independent_wire) {
    REQUIRE_TRUE(
        std::ranges::equal(flitzzip_test::rans_singleton_payload(32U, 16U, 0U, 8U), flitzzip_test::kRansZero16));
    REQUIRE_TRUE(std::ranges::equal(flitzzip_test::rans_singleton_payload(513U, 8U, 8U, 8U, 255U),
                                    flitzzip_test::kRansSingleton8));
}

TEST_CASE(gpu_rans_reader_independent_payloads_and_segment_resets) {
    struct Fixture {
        std::span<const std::byte> payload;
        std::uint32_t decoded;
        std::uint8_t precision;
        std::uint8_t contexts;
        unsigned int pattern;
    };
    const std::array cases{Fixture{flitzzip_test::kRansZero16, 32U, 16U, 0U, 0U},
                           Fixture{flitzzip_test::kRansBinary8, 128U, 8U, 0U, 1U},
                           Fixture{flitzzip_test::kRansSegmented12, 513U, 12U, 8U, 1U},
                           Fixture{flitzzip_test::kRansSingleton8, 513U, 8U, 8U, 2U}};
    for (const auto& fixture : cases) {
        const flitzzip::GpuRansBlockView view(fixture.payload, fixture.decoded);
        REQUIRE_EQ(view.decoded_size(), fixture.decoded);
        REQUIRE_EQ(view.precision(), fixture.precision);
        REQUIRE_EQ(view.context_bits(), fixture.contexts);
        REQUIRE_EQ(view.segment_shift(), 8U);
        REQUIRE_EQ(view.segment_count(), (fixture.decoded + 255U) / 256U);
        std::vector<std::uint32_t> workspace(view.model_workspace_words() + 2U, 23U);
        std::vector<std::byte> output(fixture.decoded + 2U, std::byte{90});
        view.decode(std::span(output).subspan(1U, fixture.decoded),
                    std::span(workspace).subspan(1U, view.model_workspace_words()));
        REQUIRE_EQ(workspace.front(), 23U);
        REQUIRE_EQ(workspace.back(), 23U);
        REQUIRE_EQ(output.front(), std::byte{90});
        REQUIRE_EQ(output.back(), std::byte{90});
        for (std::size_t i = 0U; i < fixture.decoded; ++i) {
            const auto expected = fixture.pattern == 2U || (fixture.pattern == 1U && i % 2U != 0U) ? 255U : 0U;
            REQUIRE_EQ(output[i + 1U], static_cast<std::byte>(expected));
        }
        require_rans_rejection([&] { (void)view.stream(view.segment_count()); });
    }
}

TEST_CASE(gpu_rans_reader_full_width_singleton_and_complete_sparse_cdf) {
    const flitzzip::GpuRansBlockView singleton(flitzzip_test::kRansZero16, 32U);
    std::vector<std::uint32_t> words(singleton.model_workspace_words());
    singleton.prepare_models(words);
    REQUIRE_EQ(words.size(), 258U);
    REQUIRE_EQ(words[0], 1U);
    REQUIRE_EQ(words[1], 0U);
    for (std::size_t i = 2U; i < words.size(); ++i) {
        REQUIRE_EQ(words[i], 65536U);
    }
    const auto payload = rans_unused_row_fixture(false);
    const flitzzip::GpuRansBlockView view(payload, 1024U);
    words.resize(view.model_workspace_words());
    view.prepare_models(words);
    REQUIRE_EQ(words.size(), 256U + 2U * 257U);
    REQUIRE_EQ(words[1], 0U);
    REQUIRE_EQ(words[7], 256U + 257U);
    for (std::size_t symbol = 0; symbol <= 256U; ++symbol) {
        REQUIRE_EQ(words[words[7] + symbol], symbol);
    }
    std::vector<std::byte> output(1024U, std::byte{90});
    view.decode(output, words);
    REQUIRE_TRUE(std::ranges::all_of(output, [](auto byte) { return byte == std::byte{0}; }));
}

// For a singleton, frequency == 2^precision and start == slot == 0:
// the specified state remains L for every byte. Construct that wire invariant
// directly across every option, without calling a production encoder or model.
TEST_CASE(gpu_rans_reader_all_options_preserve_exact_singleton_state) {
    for (unsigned int precision = 8U; precision <= 16U; ++precision) {
        for (unsigned int context_bits = 0U; context_bits <= 8U; ++context_bits) {
            for (unsigned int shift = 8U; shift <= 12U; ++shift) {
                constexpr std::uint32_t decoded = 513U;
                const auto payload =
                    flitzzip_test::rans_singleton_payload(decoded, precision, context_bits, shift, 255U);
                const flitzzip::GpuRansBlockView view(payload, decoded);
                std::vector<std::uint32_t> workspace(view.model_workspace_words());
                std::vector<std::byte> output(decoded, std::byte{90});
                view.decode(output, workspace);
                REQUIRE_TRUE(std::ranges::all_of(output, [](auto byte) { return byte == std::byte{255}; }));
            }
        }
    }
}

TEST_CASE(gpu_rans_reader_maximum_context_inventory_has_bounded_caller_scratch) {
    const auto payload = rans_unused_row_fixture(true);
    const flitzzip::GpuRansBlockView view(payload, 65536U);
    REQUIRE_EQ(view.model_workspace_words(), 66048U);
    REQUIRE_EQ(view.segment_count(), 16U);
    std::vector<std::uint32_t> workspace(view.model_workspace_words());
    std::vector<std::byte> output(65536U, std::byte{90});
    view.decode(output, workspace);
    REQUIRE_TRUE(std::ranges::all_of(output, [](auto byte) { return byte == std::byte{0}; }));
    for (std::size_t context = 0; context < 256U; ++context) {
        REQUIRE_EQ(workspace[context], 256U + context * 257U);
    }
}

TEST_CASE(gpu_rans_reader_non_singleton_maximum_u16_frequency_is_full_width) {
    std::vector<std::byte> payload{std::byte{16},  std::byte{8},   std::byte{12},  std::byte{1}, std::byte{0},
                                   std::byte{0},   std::byte{0},   std::byte{7},   std::byte{1}, std::byte{0},
                                   std::byte{255}, std::byte{255}, std::byte{255}, std::byte{1}, std::byte{0}};
    append_rans_word(payload, 0U);
    append_rans_word(payload, 4U);
    append_rans_word(payload, 1U << 23U);
    const flitzzip::GpuRansBlockView view(payload, 32U);
    std::vector<std::uint32_t> workspace(view.model_workspace_words());
    view.prepare_models(workspace);
    const auto row = workspace[7];
    REQUIRE_EQ(workspace[row], 0U);
    for (std::size_t symbol = 1U; symbol < 256U; ++symbol) {
        REQUIRE_EQ(workspace[row + symbol], 65535U);
    }
    REQUIRE_EQ(workspace[row + 256U], 65536U);
    std::vector<std::byte> output(32U, std::byte{90});
    view.decode(output, workspace);
    REQUIRE_TRUE(std::ranges::all_of(output, [](auto byte) { return byte == std::byte{0}; }));
}

TEST_CASE(gpu_rans_reader_rejects_every_truncated_prefix_and_invalid_extent) {
    for (const auto payload : {std::span<const std::byte>(flitzzip_test::kRansZero16),
                               std::span<const std::byte>(flitzzip_test::kRansBinary8)}) {
        const auto decoded = payload.size() == flitzzip_test::kRansZero16.size() ? 32U : 128U;
        for (std::size_t size = 0; size < payload.size(); ++size) {
            require_rans_rejection([&] { (void)flitzzip::GpuRansBlockView(payload.first(size), decoded); });
        }
        for (const auto size : {0U, static_cast<std::uint32_t>(payload.size()), flitzzip::kMaxArchiveBlockBytes + 1U,
                                std::numeric_limits<std::uint32_t>::max()}) {
            require_rans_rejection([&] { (void)flitzzip::GpuRansBlockView(payload, size); });
        }
    }
}

TEST_CASE(gpu_rans_reader_rejects_noncanonical_models_and_offsets) {
    const auto reject_change = [](std::size_t offset, unsigned int value) {
        std::vector<std::byte> bytes(flitzzip_test::kRansBinary8.begin(), flitzzip_test::kRansBinary8.end());
        bytes.at(offset) = static_cast<std::byte>(value);
        require_rans_rejection([&] { (void)flitzzip::GpuRansBlockView(bytes, 128U); });
    };
    for (const auto precision : {7U, 17U, 255U}) {
        reject_change(0U, precision);
    }
    reject_change(1U, 9U);
    reject_change(2U, 7U);
    reject_change(2U, 13U);
    reject_change(3U, 1U);
    reject_change(4U, 1U);
    reject_change(9U, 0U);
    reject_change(6U, 255U);
    reject_change(7U, 0U);
    reject_change(7U, 127U);
    reject_change(7U, 129U);
    reject_change(12U, 1U);
    reject_change(16U, 3U);
    reject_change(16U, 255U);
    std::vector<std::byte> missing_initial(flitzzip_test::kRansBinary8.begin(), flitzzip_test::kRansBinary8.end());
    missing_initial[1] = std::byte{1};
    missing_initial[4] = std::byte{1};
    require_rans_rejection([&] { (void)flitzzip::GpuRansBlockView(missing_initial, 128U); });
    auto bytes = rans_unused_row_fixture(false);
    bytes[7] = std::byte{0};
    require_rans_rejection([&] { (void)flitzzip::GpuRansBlockView(bytes, 1024U); });
    bytes.assign(flitzzip_test::kRansZero16.begin(), flitzzip_test::kRansZero16.end());
    bytes.push_back(std::byte{0});
    require_rans_rejection([&] { (void)flitzzip::GpuRansBlockView(bytes, 32U); });
}

TEST_CASE(gpu_rans_reader_rejects_coded_state_and_missing_context_without_fallback) {
    std::vector<std::vector<std::byte>> corrupt;
    for (const auto state : {0U, (1U << 23U) - 1U, 1U << 31U, (1U << 23U) + 1U}) {
        std::vector<std::byte> bytes(flitzzip_test::kRansZero16.begin(), flitzzip_test::kRansZero16.end());
        bytes.resize(15U);
        append_rans_word(bytes, state);
        corrupt.push_back(std::move(bytes));
    }
    auto trailing = corrupt.front();
    trailing.resize(15U);
    trailing[11] = std::byte{5};
    append_rans_word(trailing, 1U << 23U);
    trailing.push_back(std::byte{0});
    corrupt.push_back(std::move(trailing));
    for (const auto& payload : corrupt) {
        const flitzzip::GpuRansBlockView view(payload, 32U);
        std::vector<std::uint32_t> workspace(view.model_workspace_words());
        std::vector<std::byte> output(32U);
        require_rans_rejection([&] { view.decode(output, workspace); });
        const flitzzip::GpuRansBlockView good(flitzzip_test::kRansZero16, 32U);
        good.decode(output, workspace);
        REQUIRE_TRUE(std::ranges::all_of(output, [](auto byte) { return byte == std::byte{0}; }));
    }
    std::vector<std::byte> missing(flitzzip_test::kRansBinary8.begin(), flitzzip_test::kRansBinary8.end());
    missing[1] = std::byte{8};
    const flitzzip::GpuRansBlockView view(missing, 128U);
    std::vector<std::uint32_t> workspace(view.model_workspace_words());
    std::vector<std::byte> output(128U);
    require_rans_rejection([&] { view.decode(output, workspace); });
}

TEST_CASE(gpu_rans_reader_rejects_truncated_renormalization_with_valid_framing) {
    std::vector<std::byte> bytes(flitzzip_test::kRansBinary8.begin(), flitzzip_test::kRansBinary8.end());
    bytes.pop_back();
    bytes[16] = std::byte{19};
    const flitzzip::GpuRansBlockView view(bytes, 128U);
    std::vector<std::uint32_t> workspace(view.model_workspace_words());
    std::vector<std::byte> output(128U);
    require_rans_rejection([&] { view.decode(output, workspace); });
}

TEST_CASE(gpu_rans_reader_admits_capacity_before_mutation_and_preserves_guards) {
    const flitzzip::GpuRansBlockView view(flitzzip_test::kRansZero16, 32U);
    std::vector<std::uint32_t> workspace(view.model_workspace_words() + 1U, 23U);
    std::vector<std::byte> output(32U, std::byte{90});
    require_rans_rejection([&] { view.decode(output, std::span(workspace).first(view.model_workspace_words() - 1U)); });
    require_rans_rejection([&] { view.decode(std::span(output).first(31U), workspace); });
    REQUIRE_TRUE(std::ranges::all_of(workspace, [](auto word) { return word == 23U; }));
    REQUIRE_TRUE(std::ranges::all_of(output, [](auto byte) { return byte == std::byte{90}; }));
    view.decode(output, workspace);
    REQUIRE_EQ(workspace.back(), 23U);
    REQUIRE_TRUE(std::ranges::all_of(output, [](auto byte) { return byte == std::byte{0}; }));
}

TEST_CASE(gpu_rans_reader_rejects_live_aliases_but_allows_unused_scratch_reuse) {
    std::vector<std::byte> source(32U, std::byte{90});
    std::ranges::copy(flitzzip_test::kRansZero16, source.begin());
    const flitzzip::GpuRansBlockView source_view(std::span(source).first(19U), 32U);
    std::vector<std::uint32_t> workspace(source_view.model_workspace_words(), 23U);
    const auto before = source;
    require_rans_rejection([&] { source_view.decode(source, workspace); });
    REQUIRE_EQ(source, before);
    const flitzzip::GpuRansBlockView view(flitzzip_test::kRansZero16, 32U);
    std::vector<std::uint32_t> combined(view.model_workspace_words() + 9U, 23U);
    auto bytes = std::as_writable_bytes(std::span(combined));
    const auto original = combined;
    require_rans_rejection([&] { view.decode(bytes.first(32U), combined); });
    REQUIRE_EQ(combined, original);
    std::ranges::copy(flitzzip_test::kRansZero16, bytes.begin());
    const flitzzip::GpuRansBlockView overlapped(bytes.first(19U), 32U);
    const auto payload_before = combined;
    require_rans_rejection([&] { overlapped.prepare_models(combined); });
    REQUIRE_EQ(combined, payload_before);
    view.decode(bytes.subspan(view.model_workspace_words() * 4U, 32U), combined);
    REQUIRE_EQ(combined.back(), 23U);
    REQUIRE_TRUE(std::ranges::all_of(bytes.subspan(view.model_workspace_words() * 4U, 32U),
                                     [](auto byte) { return byte == std::byte{0}; }));
}
