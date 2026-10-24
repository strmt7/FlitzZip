#include "test_util.hpp"

#include "gpu/dictionary_matcher.hpp"
#include "gpu/hip_device.hpp"
#include "lz4.h"

#if FLITZZIP_ENABLE_HIP
#include "gpu/hip_kernel_api.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#if FLITZZIP_ENABLE_HIP
namespace {
struct IndependentWriterState {
    std::vector<std::byte> input;
    std::vector<flitzzip::dictionary::Match> matches;
    std::vector<std::uint32_t> next;
    std::vector<std::uint32_t> ends;
    std::vector<std::uint64_t> tree;
    std::vector<std::byte> wire;
};

// Purpose: Describe an independently legal 65-match LZ4 parse that crosses the real writer's 64-sequence tile.
// Inputs: None. Outputs: Guarded immutable input/decisions, exact packet and terminal cost; no production parser.
IndependentWriterState independent_incremental_writer_state() {
    using namespace flitzzip::dictionary;
    constexpr Match guard{0xA5A5U, 0xA5A5U, 0xA5A5U, 0xA5A5U};
    IndependentWriterState state{std::vector<std::byte>(334U, std::byte{0xD6}),
                                 std::vector<Match>(302U, guard),
                                 std::vector<std::uint32_t>(302U, 0xA5A5A5A5U),
                                 std::vector<std::uint32_t>(302U, 0xA5A5A5A5U),
                                 std::vector<std::uint64_t>(optimal::kTreeWords + 32U, optimal::kInfinity),
                                 {}};
    std::fill_n(state.input.begin() + 32U, 270U, std::byte{0x61});
    std::fill_n(state.matches.begin() + 16U, 270U, Match{});
    std::fill_n(state.next.begin() + 16U, 270U, 270U);
    std::fill_n(state.ends.begin() + 16U, 270U, 270U);
    std::fill_n(state.tree.begin(), 16U, 0xA5A5A5A5A5A5A5A5ULL);
    std::fill_n(state.tree.end() - 16U, 16U, 0xA5A5A5A5A5A5A5A5ULL);
    state.tree[16U + kSegmentBytes] = std::uint64_t{206U} << 32U;
    for (std::uint32_t sequence = 0; sequence < 65U; ++sequence) {
        const auto match = 1U + sequence * 4U;
        state.next[16U + (sequence == 0U ? 0U : match)] = match;
        state.ends[16U + match] = match + 4U;
        state.matches[16U + match] = {.distance = 1U, .length = 4U};
    }
    state.wire = {std::byte{0x10}, std::byte{0x61}, std::byte{1}, std::byte{0}};
    for (unsigned sequence = 1U; sequence < 65U; ++sequence) {
        state.wire.insert(state.wire.end(), {std::byte{0}, std::byte{1}, std::byte{0}});
    }
    state.wire.push_back(std::byte{0x90});
    state.wire.insert(state.wire.end(), 9U, std::byte{0x61});
    REQUIRE_EQ(state.wire.size(), 206U);
    std::vector<std::byte> independent(270U);
    REQUIRE_EQ(LZ4_decompress_safe(reinterpret_cast<const char*>(state.wire.data()),
                                   reinterpret_cast<char*>(independent.data()), static_cast<int>(state.wire.size()),
                                   static_cast<int>(independent.size())),
               static_cast<int>(independent.size()));
    REQUIRE_TRUE(std::equal(independent.begin(), independent.end(), state.input.begin() + 32U));
    return state;
}

// Purpose: Read a live owned device allocation independently of writer metadata. Inputs: Exact resident byte extent.
// Outputs: Requires every immutable byte or expected output byte to agree; synchronizes before owners can release.
void require_writer_device_bytes(const void* device, std::span<const std::byte> expected) {
    std::vector<std::byte> actual(expected.size());
    REQUIRE_EQ(hipMemcpy(actual.data(), device, actual.size(), hipMemcpyDeviceToHost), hipSuccess);
    REQUIRE_TRUE(std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()));
}

// Purpose: Upload only complete initialized fixture storage. Inputs: Owned destination and immutable exact bytes.
// Outputs: Completes the copy or fails; no owner changes and no queued work outlives the caller.
void upload_writer_device_bytes(void* device, std::span<const std::byte> bytes) {
    REQUIRE_EQ(hipMemcpy(device, bytes.data(), bytes.size(), hipMemcpyHostToDevice), hipSuccess);
}
}  // namespace
#endif

// Purpose: Verify a bounded writer publishes continuation state, completes once and rejects a stopped cursor safely.
// Inputs: Independent 270-byte input/65-match parse, one real 64-sequence tile per launch and a cursor at exact end.
// Outputs: Exact partial/final packet, immutable source/decisions, guarded states, no-op completion and valid reuse.
TEST_CASE(gpu_neutron_incremental_writer_independent_wire_and_cursor_recovery) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    using namespace flitzzip;
    using namespace flitzzip::dictionary;
    using namespace flitzzip::hip_detail;
    const auto fixture = independent_incremental_writer_state();
    HipDeviceBuffer<std::byte> input(fixture.input.size(), "writer oracle input");
    HipDeviceBuffer<Match> matches(fixture.matches.size() * sizeof(Match), "writer oracle matches");
    HipDeviceBuffer<std::uint32_t> next(fixture.next.size() * sizeof(std::uint32_t), "writer oracle next");
    HipDeviceBuffer<std::uint32_t> ends(fixture.ends.size() * sizeof(std::uint32_t), "writer oracle ends");
    HipDeviceBuffer<std::uint64_t> tree(fixture.tree.size() * sizeof(std::uint64_t), "writer oracle cost");
    HipDeviceBuffer<std::byte> output(kEncodedSegmentCapacity + 256U, "writer oracle output");
    HipDeviceBuffer<optimal::EmitCursor> cursors(3U * sizeof(optimal::EmitCursor), "writer oracle cursors");
    HipDeviceBuffer<std::uint32_t> sizes(3U * sizeof(std::uint32_t), "writer oracle sizes");
    upload_writer_device_bytes(input.get(), fixture.input);
    upload_writer_device_bytes(matches.get(), std::as_bytes(std::span(fixture.matches)));
    upload_writer_device_bytes(next.get(), std::as_bytes(std::span(fixture.next)));
    upload_writer_device_bytes(ends.get(), std::as_bytes(std::span(fixture.ends)));
    upload_writer_device_bytes(tree.get(), std::as_bytes(std::span(fixture.tree)));
    optimal::State state{.matches = matches.get() + 16U,
                         .suffix_tree = tree.get() + 16U,
                         .match_ends = ends.get() + 16U,
                         .next_matches = next.get() + 16U};
    struct Stage {
        bool reset;
        std::uint32_t start;
        std::uint32_t prefix;
        std::uint32_t position;
        std::uint32_t written;
        std::uint32_t size;
    };
    constexpr std::array stages{Stage{true, 0U, 193U, 257U, 193U, 0U},    Stage{false, 0U, 206U, 257U, 193U, 206U},
                                Stage{false, 0U, 206U, 257U, 193U, 206U}, Stage{true, 270U, 0U, 270U, 0U, 0xFFFFFFFFU},
                                Stage{true, 0U, 193U, 257U, 193U, 0U},    Stage{false, 0U, 206U, 257U, 193U, 206U}};
    for (const auto& stage : stages) {
        if (stage.reset) {
            REQUIRE_EQ(hipMemset(output.get(), 0xA5, kEncodedSegmentCapacity + 256U), hipSuccess);
            const std::array<optimal::EmitCursor, 3> initial{
                {{0xA5A5A5A5U, 0xA5A5A5A5U}, {stage.start, 0U}, {0xA5A5A5A5U, 0xA5A5A5A5U}}};
            const std::array<std::uint32_t, 3> status{0xA5A5A5A5U, 0U, 0xA5A5A5A5U};
            upload_writer_device_bytes(cursors.get(), std::as_bytes(std::span(initial)));
            upload_writer_device_bytes(sizes.get(), std::as_bytes(std::span(status)));
        }
        const auto* from = input.get() + 32U;
        std::uint32_t length = 270U;
        auto* to = output.get() + 128U;
        auto* cursor = cursors.get() + 1U;
        auto* status = sizes.get() + 1U;
        void* arguments[]{&from, &length, &state, &to, &cursor, &status};
        REQUIRE_EQ(hipLaunchKernel(reinterpret_cast<const void*>(hip_kernel_api().neutron_emit_tile), dim3(1),
                                   dim3(kDictionaryKernelThreads), arguments, 0, nullptr),
                   hipSuccess);
        std::vector<std::byte> expected(kEncodedSegmentCapacity + 256U, std::byte{0xA5});
        std::copy_n(fixture.wire.begin(), stage.prefix, expected.begin() + 128U);
        require_writer_device_bytes(output.get(), expected);
        const std::array<optimal::EmitCursor, 3> expected_cursors{
            {{0xA5A5A5A5U, 0xA5A5A5A5U}, {stage.position, stage.written}, {0xA5A5A5A5U, 0xA5A5A5A5U}}};
        const std::array<std::uint32_t, 3> expected_sizes{0xA5A5A5A5U, stage.size, 0xA5A5A5A5U};
        require_writer_device_bytes(cursors.get(), std::as_bytes(std::span(expected_cursors)));
        require_writer_device_bytes(sizes.get(), std::as_bytes(std::span(expected_sizes)));
        require_writer_device_bytes(input.get(), fixture.input);
        require_writer_device_bytes(matches.get(), std::as_bytes(std::span(fixture.matches)));
        require_writer_device_bytes(next.get(), std::as_bytes(std::span(fixture.next)));
        require_writer_device_bytes(ends.get(), std::as_bytes(std::span(fixture.ends)));
        require_writer_device_bytes(tree.get(), std::as_bytes(std::span(fixture.tree)));
    }
    sizes.reset_checked("writer oracle sizes release");
    cursors.reset_checked("writer oracle cursors release");
    output.reset_checked("writer oracle output release");
    tree.reset_checked("writer oracle cost release");
    ends.reset_checked("writer oracle ends release");
    next.reset_checked("writer oracle next release");
    matches.reset_checked("writer oracle matches release");
    input.reset_checked("writer oracle input release");
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}
