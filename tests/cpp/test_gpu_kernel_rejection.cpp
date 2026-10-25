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
#include <utility>
#include <vector>

#if FLITZZIP_ENABLE_HIP
namespace {
using namespace flitzzip;
using namespace flitzzip::dictionary;
using namespace flitzzip::hip_detail;
constexpr std::uint32_t kWordGuard = 0xA5A5A5A5U;
constexpr std::uint64_t kWideGuard = 0xA5A5A5A5A5A5A5A5ULL;
constexpr std::byte kOutputGuard{0xA5};
constexpr std::byte kInputGuard{0xD3};
static_assert(sizeof(Match) == 8U && sizeof(PrefixDecodeSegment) == 48U && sizeof(DeviceBlock) == 32U &&
              sizeof(sparse_pattern::SparseCandidate) == 24U && sizeof(optimal::EmitCursor) == 8U);
static_assert(kDictionaryKernelThreads == 256U && kSparseKernelThreads == 256U && optimal::kLanes == 256U &&
              kSegmentBytes == 65536U && kEncodedSegmentCapacity == 65809U && optimal::kSequencesPerLaunch == 64U &&
              optimal::kTreeWords == 131072U && optimal::kResidueTreeWords == 512U);

// Purpose: Own complete initialized storage and its independent host expectation.
// Inputs: Exact allocation elements, including all outer guards. Outputs: Synchronous full-storage oracles.
template <typename T> class ResidentOracle {
  public:
    // Purpose: Acquire and initialize one bounded device allocation. Inputs: Exact initialized values and label.
    // Outputs: Owns both copies; the byte-sized HIP constructor never receives an element count.
    ResidentOracle(std::vector<T> initial, const char* label)
        : values(std::move(initial)), storage_(values.size() * sizeof(T), label), count_(values.size()) {
        upload();
    }

    // Purpose: Borrow resident storage without transferring ownership. Inputs: Live owner.
    // Outputs: Typed pointer into this exact allocation, retained through synchronous readback.
    T* device() const {
        return storage_.get();
    }

    // Purpose: Publish only initialized expected bytes. Inputs: Whole host storage of unchanged extent.
    // Outputs: Synchronously replaces every resident byte or fails before a kernel launch.
    void upload() {
        REQUIRE_EQ(values.size(), count_);
        const auto bytes = std::as_bytes(std::span(values));
        REQUIRE_EQ(hipMemcpy(storage_.get(), bytes.data(), bytes.size(), hipMemcpyHostToDevice), hipSuccess);
    }

    // Purpose: Read actual storage without consulting device metadata. Inputs: This exact resident extent.
    // Outputs: Synchronous complete copy; no queued work survives the read or owner release.
    std::vector<T> read() const {
        REQUIRE_EQ(values.size(), count_);
        std::vector<T> actual(count_);
        REQUIRE_EQ(hipMemcpy(actual.data(), storage_.get(), actual.size() * sizeof(T), hipMemcpyDeviceToHost),
                   hipSuccess);
        return actual;
    }

    // Purpose: Compare all output and immutable guard bytes. Inputs: Independent current host expectation.
    // Outputs: Requires exact object representation across the complete allocation.
    void require_same() const {
        const auto actual = read();
        REQUIRE_TRUE(std::ranges::equal(std::as_bytes(std::span(actual)), std::as_bytes(std::span(values))));
    }

    // Purpose: Check normal-path resource release. Inputs: Diagnostic label after all readbacks complete.
    // Outputs: Releases this allocation once; RAII still owns exceptional cleanup.
    void release(const char* label) {
        storage_.reset_checked(label);
    }

    std::vector<T> values;

  private:
    HipDeviceBuffer<T> storage_;
    const std::size_t count_;
};

// Purpose: Frame an independent little-endian table without using a production codec.
// Inputs: Initialized payload, contained byte offset and literal value. Outputs: Four exact bytes.
void store_literal_u32(std::vector<std::byte>& payload, std::size_t offset, std::uint32_t value) {
    REQUIRE_TRUE(offset + 4U <= payload.size());
    for (unsigned byte = 0; byte < 4U; ++byte) {
        payload[offset + byte] = std::byte{static_cast<std::uint8_t>(value >> (8U * byte))};
    }
}

// Purpose: Build two complete independent 2-bit Huffman lookup blocks.
// Inputs: None. Outputs: Guarded dense codebooks, legal [0,2] tables and E4/E4 symbol cycles.
std::vector<std::byte> tiny_huffman_payload() {
    std::vector<std::byte> payload(16468U, kInputGuard);
    constexpr std::array<std::uint8_t, 4> symbols{0x50U, 0x71U, 0x00U, 0xFFU};
    for (const auto codebook : std::array<std::size_t, 2>{32U, 8234U}) {
        for (std::size_t entry = 0; entry < 4096U; ++entry) {
            payload[codebook + entry * 2U] = std::byte{symbols[entry % 4U]};
            payload[codebook + entry * 2U + 1U] = std::byte{2U};
        }
        store_literal_u32(payload, codebook + 8192U, 0U);
        store_literal_u32(payload, codebook + 8196U, 2U);
        payload[codebook + 8200U] = std::byte{0xE4};
        payload[codebook + 8201U] = std::byte{0xE4};
    }
    return payload;
}

// Purpose: Supply contained plans even for inactive lanes and outer guards.
// Inputs: None. Outputs: Two active 8-symbol plans and four resident valid-neighbor zero-length records.
std::vector<PrefixDecodeSegment> tiny_huffman_plans() {
    std::vector<PrefixDecodeSegment> plans(6U);
    auto representation = std::as_writable_bytes(std::span(plans));
    std::fill(representation.begin(), representation.end(), std::byte{0});
    for (auto& plan : plans) {
        plan.codebook_offset = 8234U;
        plan.table_offset = 16426U;
        plan.bitstream_offset = 16434U;
        plan.output_offset = 48U;
        plan.segment_index = 0U;
        plan.decoded_len = 0U;
        plan.adaptive = 2U;
    }
    plans[1].codebook_offset = 32U;
    plans[1].table_offset = 8224U;
    plans[1].bitstream_offset = 8232U;
    plans[1].output_offset = 32U;
    plans[1].decoded_len = plans[2].decoded_len = 8U;
    return plans;
}

// Purpose: Dispatch only four bounded prefix lanes. Inputs: Two actual plans and contained inactive dummy records.
// Outputs: Completes the normal HIP launch; caller reads all storage before owners release.
void launch_tiny_huffman(ResidentOracle<std::byte>& payload, ResidentOracle<PrefixDecodeSegment>& plans,
                         ResidentOracle<std::byte>& output, ResidentOracle<std::uint32_t>& errors) {
    const auto* from = payload.device();
    const auto* descriptors = plans.device() + 1U;
    std::uint32_t count = 2U;
    auto* to = output.device();
    auto* status = errors.device() + 1U;
    void* arguments[]{&from, &descriptors, &count, &to, &status};
    REQUIRE_EQ(hipLaunchKernel(reinterpret_cast<const void*>(hip_kernel_api().materialize_prefix_segments_kernel),
                               dim3(2U), dim3(2U), arguments, 0, nullptr),
               hipSuccess);
}

// Purpose: Compare explicit LSB-first literal symbols and every output guard.
// Inputs: The number of legal plan-zero symbols before rejection; plan one always completes.
// Outputs: Exact expected bytes, independent of the production lookup or bit cursor.
void expect_tiny_huffman(ResidentOracle<std::byte>& output, std::size_t first_symbols) {
    constexpr std::array<std::byte, 8> cycle{std::byte{0x50}, std::byte{0x71}, std::byte{0}, std::byte{0xFF},
                                             std::byte{0x50}, std::byte{0x71}, std::byte{0}, std::byte{0xFF}};
    std::fill(output.values.begin(), output.values.end(), kOutputGuard);
    std::copy_n(cycle.begin(), first_symbols, output.values.begin() + 32U);
    std::copy(cycle.begin(), cycle.end(), output.values.begin() + 48U);
}

struct TinyWriterFixture {
    std::vector<std::byte> input;
    std::vector<Match> matches;
    std::vector<std::uint32_t> costs;
    std::vector<std::uint32_t> ends;
    std::vector<std::uint32_t> next;
    std::vector<std::uint64_t> tree;
    std::vector<std::uint64_t> residue;
    std::vector<std::byte> wire;
};

// Purpose: Assemble an independent legal LZ4 packet and complete bounded Neutron decision storage.
// Inputs: None. Outputs: One literal, offset-one match20, final11 literals, and a 17-byte terminal cost.
TinyWriterFixture tiny_writer_fixture() {
    constexpr Match guard{0xA5A5U, 0xA5A5U, 0xA5A5U, 0xA5A5U};
    TinyWriterFixture fixture{
        std::vector<std::byte>(96U, kInputGuard),
        std::vector<Match>(64U, guard),
        std::vector<std::uint32_t>(64U, kWordGuard),
        std::vector<std::uint32_t>(64U, kWordGuard),
        std::vector<std::uint32_t>(64U, kWordGuard),
        std::vector<std::uint64_t>(optimal::kTreeWords + 32U, kWideGuard),
        std::vector<std::uint64_t>(optimal::kResidueTreeWords + 32U, kWideGuard),
        {std::byte{0x1F}, std::byte{0x61}, std::byte{1}, std::byte{0}, std::byte{1}, std::byte{0xB0}}};
    std::fill_n(fixture.input.begin() + 32U, 32U, std::byte{0x61});
    std::fill_n(fixture.matches.begin() + 16U, 32U, Match{});
    std::fill_n(fixture.costs.begin() + 16U, 32U, 0U);
    std::fill_n(fixture.ends.begin() + 16U, 32U, 32U);
    std::fill_n(fixture.next.begin() + 16U, 32U, 32U);
    std::fill_n(fixture.tree.begin() + 16U, optimal::kTreeWords, optimal::kInfinity);
    std::fill_n(fixture.residue.begin() + 16U, optimal::kResidueTreeWords, optimal::kInfinity);
    fixture.matches[17U] = {.distance = 1U, .length = 20U};
    fixture.next[16U] = 1U;
    fixture.ends[17U] = 21U;
    fixture.tree[16U + kSegmentBytes] = std::uint64_t{17U} << 32U;
    fixture.wire.insert(fixture.wire.end(), 11U, std::byte{0x61});
    REQUIRE_EQ(fixture.wire.size(), 17U);
    std::array<std::byte, 32> decoded{};
    REQUIRE_EQ(LZ4_decompress_safe(reinterpret_cast<const char*>(fixture.wire.data()),
                                   reinterpret_cast<char*>(decoded.data()), static_cast<int>(fixture.wire.size()),
                                   static_cast<int>(decoded.size())),
               static_cast<int>(decoded.size()));
    REQUIRE_TRUE(std::ranges::all_of(decoded, [](std::byte value) { return value == std::byte{0x61}; }));
    return fixture;
}

class TinyWriterOracle {
  public:
    // Purpose: Own every State pointer and all surrounding storage. Inputs: Independent initialized fixture.
    // Outputs: Ten bounded device owners whose metadata can be verified in full after every launch.
    explicit TinyWriterOracle(const TinyWriterFixture& fixture)
        : input(fixture.input, "rejection input"), matches(fixture.matches, "rejection matches"),
          costs(fixture.costs, "rejection costs"), ends(fixture.ends, "rejection ends"),
          next(fixture.next, "rejection next"), tree(fixture.tree, "rejection tree"),
          residue(fixture.residue, "rejection residue"),
          output(std::vector<std::byte>(kEncodedSegmentCapacity + 256U, kOutputGuard), "rejection output"),
          cursors(std::vector<optimal::EmitCursor>(3U, {kWordGuard, kWordGuard}), "rejection cursors"),
          sizes(std::vector<std::uint32_t>(3U, kWordGuard), "rejection sizes") {}

    // Purpose: Restore only the decision arrays a fault may change. Inputs: Independent legal fixture.
    // Outputs: Same owners receive exact legal bytes; unused State arrays remain allocated and unchanged.
    void restore(const TinyWriterFixture& fixture) {
        matches.values = fixture.matches;
        ends.values = fixture.ends;
        next.values = fixture.next;
        tree.values = fixture.tree;
        matches.upload();
        ends.upload();
        next.upload();
        tree.upload();
    }

    // Purpose: Dispatch one finite 64-sequence tile with a single 256-lane group.
    // Inputs: Exactly 32 input bytes and complete resident State storage. Outputs: Existing ABI writes owned
    // slot/status.
    void emit() {
        const auto* from = input.device() + 32U;
        std::uint32_t bytes = 32U;
        optimal::State state{.matches = matches.device() + 16U,
                             .suffix_tree = tree.device() + 16U,
                             .residue_tree = residue.device() + 16U,
                             .match_costs = costs.device() + 16U,
                             .match_ends = ends.device() + 16U,
                             .next_matches = next.device() + 16U};
        auto* to = output.device() + 128U;
        auto* cursor = cursors.device() + 1U;
        auto* status = sizes.device() + 1U;
        void* arguments[]{&from, &bytes, &state, &to, &cursor, &status};
        REQUIRE_EQ(hipLaunchKernel(reinterpret_cast<const void*>(hip_kernel_api().neutron_emit_tile), dim3(1U),
                                   dim3(kDictionaryKernelThreads), arguments, 0, nullptr),
                   hipSuccess);
    }

    // Purpose: Recover using the real writer initializer, not a host-side replacement for its state transition.
    // Inputs: One owned cursor/status with outer guards. Outputs: Only active cursor and status become zero.
    void initialize() {
        auto* cursor = cursors.device() + 1U;
        auto* status = sizes.device() + 1U;
        std::uint32_t segments = 1U;
        void* arguments[]{&cursor, &status, &segments};
        REQUIRE_EQ(hipLaunchKernel(reinterpret_cast<const void*>(hip_kernel_api().neutron_initialize_writers), dim3(1U),
                                   dim3(kDictionaryKernelThreads), arguments, 0, nullptr),
                   hipSuccess);
        cursors.values[1] = {};
        sizes.values[1] = 0U;
    }

    // Purpose: Compare every immutable pointer target, output byte and cursor/status guard.
    // Inputs: Explicit independent expectations. Outputs: Complete synchronous readbacks before any release.
    void require_same() const {
        input.require_same();
        matches.require_same();
        costs.require_same();
        ends.require_same();
        next.require_same();
        tree.require_same();
        residue.require_same();
        output.require_same();
        cursors.require_same();
        sizes.require_same();
    }

    // Purpose: Check all normal resource releases after final synchronous oracles.
    // Inputs: Live owners with no queued work. Outputs: Each allocation is released exactly once.
    void release() {
        sizes.release("rejection sizes release");
        cursors.release("rejection cursors release");
        output.release("rejection output release");
        residue.release("rejection residue release");
        tree.release("rejection tree release");
        next.release("rejection next release");
        ends.release("rejection ends release");
        costs.release("rejection costs release");
        matches.release("rejection matches release");
        input.release("rejection input release");
    }

    ResidentOracle<std::byte> input;
    ResidentOracle<Match> matches;
    ResidentOracle<std::uint32_t> costs;
    ResidentOracle<std::uint32_t> ends;
    ResidentOracle<std::uint32_t> next;
    ResidentOracle<std::uint64_t> tree;
    ResidentOracle<std::uint64_t> residue;
    ResidentOracle<std::byte> output;
    ResidentOracle<optimal::EmitCursor> cursors;
    ResidentOracle<std::uint32_t> sizes;
};

// Purpose: Require sparse output to be only admitted, distinct mismatches, including a saturated subset.
// Inputs: Literal five-position oracle, exact output range/count and a complete resident copy.
// Outputs: Protects every other output word and verifies membership without assuming atomic ordering.
void require_sparse_positions(ResidentOracle<std::uint32_t>& positions, std::size_t count) {
    constexpr std::array<std::uint32_t, 5> expected{3U, 6U, 8U, 17U, 22U};
    const auto actual = positions.read();
    std::vector<std::uint32_t> written(actual.begin() + 5U, actual.begin() + 5U + count);
    std::sort(written.begin(), written.end());
    REQUIRE_TRUE(std::adjacent_find(written.begin(), written.end()) == written.end());
    for (const auto position : written) {
        REQUIRE_TRUE(std::binary_search(expected.begin(), expected.end(), position));
    }
    if (count == expected.size()) {
        REQUIRE_TRUE(std::ranges::equal(written, expected));
    }
    std::fill(positions.values.begin(), positions.values.end(), kWordGuard);
    std::copy_n(actual.begin() + 5U, count, positions.values.begin() + 5U);
    positions.require_same();
}
}  // namespace
#endif

// Purpose: Qualify bounded direct-device Huffman rejection and recovery independently of host metadata admission.
// Inputs: Two legal 8-symbol neighbors, reversed offsets and complete LUTs with width0/13 mutations.
// Outputs: Exact valid symbols/partial prefix, immutable descriptors, contained lane guards and real error clearing.
TEST_CASE(gpu_huffman_direct_lookup_rejection_preserves_neighbor_and_recovers) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::hip_device_available()) {
        return;
    }
    flitzzip::require_hip_device_ready();
    const auto valid = tiny_huffman_payload();
    ResidentOracle<std::byte> payload(valid, "lookup rejection payload");
    ResidentOracle<PrefixDecodeSegment> plans(tiny_huffman_plans(), "lookup rejection plans");
    ResidentOracle<std::byte> output(std::vector<std::byte>(96U, kOutputGuard), "lookup rejection output");
    ResidentOracle<std::uint32_t> errors(std::vector<std::uint32_t>(6U, kWordGuard), "lookup rejection errors");
    launch_tiny_huffman(payload, plans, output, errors);
    expect_tiny_huffman(output, 8U);
    errors.values[1] = errors.values[2] = 0U;
    payload.require_same();
    plans.require_same();
    output.require_same();
    errors.require_same();
    for (unsigned fault = 0; fault < 3U; ++fault) {
        payload.values = valid;
        if (fault == 0U) {
            store_literal_u32(payload.values, 8224U, 1U);
            store_literal_u32(payload.values, 8228U, 0U);
        } else {
            payload.values[32U + 2U * (fault == 1U ? 1252U : 3U) + 1U] =
                std::byte{static_cast<std::uint8_t>(fault == 1U ? 0U : 13U)};
        }
        payload.upload();
        std::fill(output.values.begin(), output.values.end(), kOutputGuard);
        output.upload();
        std::fill(errors.values.begin(), errors.values.end(), kWordGuard);
        errors.upload();
        launch_tiny_huffman(payload, plans, output, errors);
        expect_tiny_huffman(output, fault == 2U ? 7U : 0U);
        errors.values[1] = 1U;
        errors.values[2] = 0U;
        payload.require_same();
        plans.require_same();
        output.require_same();
        errors.require_same();
        payload.values = valid;
        payload.upload();
        std::fill(output.values.begin(), output.values.end(), kOutputGuard);
        output.upload();
        // The actual errors [1,0] are deliberately retained; recovery must clear them itself.
        launch_tiny_huffman(payload, plans, output, errors);
        expect_tiny_huffman(output, 8U);
        errors.values[1] = errors.values[2] = 0U;
        payload.require_same();
        plans.require_same();
        output.require_same();
        errors.require_same();
    }
    errors.release("lookup rejection errors release");
    output.release("lookup rejection output release");
    plans.release("lookup rejection plans release");
    payload.release("lookup rejection payload release");
#else
    REQUIRE_TRUE(!flitzzip::hip_device_available());
#endif
}

#if FLITZZIP_ENABLE_HIP
namespace {
// Purpose: Derive an independent nearest-prefix index without the GPU key packing or radix primitive.
// Inputs: Exactly 64 initialized bytes. Outputs: Strictly backward four-byte-equal links or the no-match sentinel.
std::array<std::uint32_t, 64> bytewise_predecessors(std::span<const std::byte, 64> input) {
    std::array<std::uint32_t, 64> previous{};
    previous.fill(0xFFFFFFFFU);
    for (std::uint32_t position = 0; position + 4U <= input.size(); ++position) {
        for (auto candidate = position; candidate > 0U;) {
            --candidate;
            if (std::equal(input.begin() + candidate, input.begin() + candidate + 4U, input.begin() + position)) {
                previous[position] = candidate;
                break;
            }
        }
    }
    return previous;
}
}  // namespace
#endif

// Purpose: Check the earliest mismatch in every byte of the matcher's eight-byte comparison against scalar bytes.
// Inputs: Eight 64-byte mirrored fixtures with one changed byte and independently derived nearest-prefix links.
// Outputs: Exact lengths/distances, bounded effort, immutable inputs and whole-allocation output guards.
TEST_CASE(gpu_dictionary_word_mismatch_matches_independent_bytewise_oracle) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::hip_device_available()) {
        return;
    }
    flitzzip::require_hip_device_ready();
    constexpr Match guard{0xA5A5U, 0xA5A5U, 0xA5A5U, 0xA5A5U};
    for (std::uint32_t mismatch = 0; mismatch < 8U; ++mismatch) {
        std::array<std::byte, 64> source{};
        for (std::size_t index = 0; index < source.size(); ++index) {
            source[index] = std::byte{static_cast<std::uint8_t>(0x20U + index % 32U)};
        }
        source[36U + mismatch] ^= std::byte{0x80};
        const auto previous = bytewise_predecessors(source);
        std::vector<std::byte> input_values(96U, kInputGuard);
        std::copy(source.begin(), source.end(), input_values.begin() + 16U);
        std::vector<std::uint32_t> link_values(kDictionaryKernelThreads + 32U, kWordGuard);
        std::copy(previous.begin(), previous.end(), link_values.begin() + 16U);
        ResidentOracle<std::byte> input(std::move(input_values), "word mismatch source");
        ResidentOracle<std::uint32_t> links(std::move(link_values), "word mismatch predecessors");
        ResidentOracle<Match> matches(std::vector<Match>(kDictionaryKernelThreads + 32U, guard),
                                      "word mismatch output");
        const auto* from = input.device() + 16U;
        const auto* predecessors = links.device() + 16U;
        auto* to = matches.device() + 16U;
        std::uint32_t size = 64U;
        Effort effort{1U, 64U};
        void* arguments[]{&from, &size, &predecessors, &effort, &to};
        REQUIRE_EQ(hipLaunchKernel(reinterpret_cast<const void*>(hip_kernel_api().search_dictionary_matches), dim3(1U),
                                   dim3(kDictionaryKernelThreads), arguments, 0, nullptr),
                   hipSuccess);
        const auto actual = matches.read();
        for (std::size_t index = 0; index < actual.size(); ++index) {
            if (index < 16U || index >= 80U) {
                REQUIRE_EQ(actual[index], guard);
            }
        }
        for (std::uint32_t position = 0; position < size; ++position) {
            const auto& observed = actual[16U + position];
            if (previous[position] == 0xFFFFFFFFU) {
                REQUIRE_EQ(observed, Match{});
                continue;
            }
            const auto candidate = previous[position];
            std::uint32_t length = 0U;
            while (position + length < size && source[candidate + length] == source[position + length]) {
                ++length;
            }
            REQUIRE_EQ(observed.distance, position - candidate);
            REQUIRE_EQ(observed.length, length);
            REQUIRE_EQ(observed.candidates_examined, 1U);
            REQUIRE_TRUE(observed.bytes_compared <= 28U);
        }
        const Match expected{32U, static_cast<std::uint16_t>(4U + mismatch), 1U, 8U};
        REQUIRE_EQ(actual[48U], expected);
        input.require_same();
        links.require_same();
        matches.release("word mismatch output release");
        links.release("word mismatch predecessor release");
        input.release("word mismatch source release");
    }
#else
    REQUIRE_TRUE(!flitzzip::hip_device_available());
#endif
}

// Purpose: Verify real multi-slot prefix sums and a contained surplus group using independently valid LZ4 packets.
// Inputs: Literal packets of 17, 2 and 11 bytes; the host's nonzero encoded-size contract stays respected.
// Outputs: Exact 30-byte concatenation, independently decoded contents and complete immutable/storage guards.
TEST_CASE(gpu_dictionary_compaction_preserves_three_legal_packets_and_surplus_guard) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::hip_device_available()) {
        return;
    }
    flitzzip::require_hip_device_ready();
    std::array<std::vector<std::byte>, 3> plain{
        std::vector<std::byte>(15U), {std::byte{0xD7}}, std::vector<std::byte>(10U)};
    for (std::size_t index = 0; index < plain[0].size(); ++index) {
        plain[0][index] = std::byte{static_cast<std::uint8_t>(0x40U + index)};
    }
    for (std::size_t index = 0; index < plain[2].size(); ++index) {
        plain[2][index] = std::byte{static_cast<std::uint8_t>(0x90U + index)};
    }
    std::array<std::vector<std::byte>, 3> packets{
        {{std::byte{0xF0}, std::byte{0}}, {std::byte{0x10}}, {std::byte{0xA0}}}};
    std::vector<std::byte> expected;
    std::vector<std::byte> slots(4U * kEncodedSegmentCapacity + 32U, kInputGuard);
    for (std::size_t slot = 0; slot < packets.size(); ++slot) {
        packets[slot].insert(packets[slot].end(), plain[slot].begin(), plain[slot].end());
        std::vector<std::byte> decoded(plain[slot].size());
        REQUIRE_EQ(LZ4_decompress_safe(reinterpret_cast<const char*>(packets[slot].data()),
                                       reinterpret_cast<char*>(decoded.data()), static_cast<int>(packets[slot].size()),
                                       static_cast<int>(decoded.size())),
                   static_cast<int>(plain[slot].size()));
        REQUIRE_EQ(decoded, plain[slot]);
        std::copy(packets[slot].begin(), packets[slot].end(), slots.begin() + 16U + slot * kEncodedSegmentCapacity);
        expected.insert(expected.end(), packets[slot].begin(), packets[slot].end());
    }
    REQUIRE_EQ(expected.size(), 30U);
    slots[16U + 3U * kEncodedSegmentCapacity] = std::byte{0};
    ResidentOracle<std::byte> encoded(std::move(slots), "compaction literal packets");
    ResidentOracle<std::uint32_t> sizes({kWordGuard, 17U, 2U, 11U, 1U, kWordGuard}, "compaction packet sizes");
    ResidentOracle<std::byte> packed(std::vector<std::byte>(62U, kOutputGuard), "compaction guarded output");
    const auto* from = encoded.device() + 16U;
    const auto* lengths = sizes.device() + 1U;
    std::uint32_t count = 3U;
    auto* to = packed.device() + 16U;
    void* arguments[]{&from, &lengths, &count, &to};
    REQUIRE_EQ(hipLaunchKernel(reinterpret_cast<const void*>(hip_kernel_api().compact_dictionary_segments), dim3(4U),
                               dim3(kDictionaryKernelThreads), arguments, 0, nullptr),
               hipSuccess);
    std::copy(expected.begin(), expected.end(), packed.values.begin() + 16U);
    packed.require_same();
    sizes.require_same();
    encoded.require_same();
    packed.release("compaction output release");
    sizes.release("compaction sizes release");
    encoded.release("compaction packets release");
#else
    REQUIRE_TRUE(!flitzzip::hip_device_available());
#endif
}

// Purpose: Exercise real writer invariant failures, sticky no-op states and initialized successful reuse.
// Inputs: Eight early bounded faults, one late terminal-price fault and an independent 17-byte legal LZ4 packet.
// Outputs: Exact sentinel/prefix/cursor behavior and whole-state guards; no transactional-publication claim.
TEST_CASE(gpu_neutron_writer_bounded_rejection_preserves_state_and_recovers) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::hip_device_available()) {
        return;
    }
    flitzzip::require_hip_device_ready();
    const auto fixture = tiny_writer_fixture();
    TinyWriterOracle oracle(fixture);
    oracle.cursors.values[1] = {};
    oracle.sizes.values[1] = 0U;
    oracle.cursors.upload();
    oracle.sizes.upload();
    oracle.emit();
    std::copy(fixture.wire.begin(), fixture.wire.end(), oracle.output.values.begin() + 128U);
    oracle.sizes.values[1] = 17U;
    oracle.require_same();
    for (unsigned fault = 0; fault < 9U; ++fault) {
        oracle.restore(fixture);
        oracle.cursors.values[1] = {};
        oracle.sizes.values[1] = 0U;
        if (fault == 0U) {
            oracle.cursors.values[1].written = kEncodedSegmentCapacity + 1U;
        } else if (fault == 1U) {
            oracle.cursors.values[1].position = 4U;
            oracle.next.values[20U] = 3U;
        } else if (fault == 2U) {
            oracle.next.values[16U] = 33U;
        } else if (fault == 3U || fault == 4U) {
            oracle.ends.values[17U] = fault == 3U ? 0U : 33U;
        } else if (fault == 5U || fault == 6U) {
            oracle.matches.values[17U].distance = static_cast<std::uint16_t>(fault == 5U ? 0U : 2U);
        } else if (fault == 7U) {
            oracle.cursors.values[1].written = kEncodedSegmentCapacity - 1U;
        } else {
            oracle.tree.values[16U + kSegmentBytes] = std::uint64_t{18U} << 32U;
        }
        oracle.next.upload();
        oracle.ends.upload();
        oracle.matches.upload();
        oracle.tree.upload();
        oracle.cursors.upload();
        oracle.sizes.upload();
        std::fill(oracle.output.values.begin(), oracle.output.values.end(), kOutputGuard);
        oracle.output.upload();
        oracle.emit();
        if (fault == 8U) {
            std::copy(fixture.wire.begin(), fixture.wire.end(), oracle.output.values.begin() + 128U);
        }
        oracle.sizes.values[1] = 0xFFFFFFFFU;
        oracle.require_same();
        oracle.emit();
        oracle.require_same();
        oracle.restore(fixture);
        oracle.initialize();
        oracle.require_same();
        std::fill(oracle.output.values.begin(), oracle.output.values.end(), kOutputGuard);
        oracle.output.upload();
        oracle.emit();
        std::copy(fixture.wire.begin(), fixture.wire.end(), oracle.output.values.begin() + 128U);
        oracle.sizes.values[1] = 17U;
        oracle.require_same();
        oracle.emit();
        oracle.require_same();
    }
    oracle.release();
#else
    REQUIRE_TRUE(!flitzzip::hip_device_available());
#endif
}

// Purpose: Require sparse gathering to respect admitted capacity and recover to the full independent mismatch set.
// Inputs: Two 33-byte period3 candidates, five mismatches and patch counts0/2 then0/5; two X tiles per Y group.
// The first tile is active; the second is outside both extents. All source and metadata pointers remain resident.
// Outputs: Exact counts/cursors, a distinct bounded subset then exact set, whole input/metadata and output canaries.
TEST_CASE(gpu_sparse_gather_saturation_preserves_ranges_and_recovers) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::hip_device_available()) {
        return;
    }
    flitzzip::require_hip_device_ready();
    using flitzzip::sparse_pattern::SparseCandidate;
    std::vector<std::byte> source(100U, kInputGuard);
    for (std::size_t i = 0; i < 33U; ++i) {
        source[35U + i] = std::byte{static_cast<std::uint8_t>(0x41U + i % 3U)};
    }
    for (const auto mismatch : std::array<std::uint32_t, 5>{3U, 6U, 8U, 17U, 22U}) {
        source[35U + mismatch] = std::byte{0x7F};
    }
    const SparseCandidate guard{kWordGuard, kWordGuard, kWordGuard, kWordGuard, kWordGuard, kWordGuard};
    std::vector<SparseCandidate> descriptors(4U, guard);
    descriptors[1] = {.source_offset = 3U,
                      .input_bytes = 33U,
                      .period = 3U,
                      .max_patches = 5U,
                      .positions_offset = 0U,
                      .patch_count = 0U};
    descriptors[2] = descriptors[1];
    descriptors[2].positions_offset = 1U;
    descriptors[2].patch_count = 2U;
    ResidentOracle<std::byte> input(std::move(source), "sparse saturation input");
    ResidentOracle<SparseCandidate> candidates(std::move(descriptors), "sparse saturation candidates");
    ResidentOracle<std::uint32_t> counts({kWordGuard, 0U, 0U, kWordGuard}, "sparse saturation counts");
    ResidentOracle<std::uint32_t> cursors({kWordGuard, 0U, 0U, kWordGuard}, "sparse saturation cursors");
    ResidentOracle<std::uint32_t> positions(std::vector<std::uint32_t>(15U, kWordGuard), "sparse saturation positions");
    const auto* from = input.device() + 32U;
    const auto* plans = candidates.device() + 1U;
    auto* count = counts.device() + 1U;
    void* count_arguments[]{&from, &plans, &count};
    REQUIRE_EQ(hipLaunchKernel(reinterpret_cast<const void*>(hip_kernel_api().count_sparse_positions_kernel),
                               dim3(2U, 2U), dim3(kSparseKernelThreads), count_arguments, 0, nullptr),
               hipSuccess);
    counts.values[1] = counts.values[2] = 5U;
    input.require_same();
    candidates.require_same();
    counts.require_same();
    cursors.require_same();
    positions.require_same();
    for (const auto capacity : std::array<std::uint32_t, 2>{2U, 5U}) {
        candidates.values[2].patch_count = capacity;
        candidates.upload();
        cursors.values[1] = cursors.values[2] = 0U;
        cursors.upload();
        std::fill(positions.values.begin(), positions.values.end(), kWordGuard);
        positions.upload();
        auto* cursor = cursors.device() + 1U;
        auto* to = positions.device() + 4U;
        void* gather_arguments[]{&from, &plans, &cursor, &to};
        REQUIRE_EQ(hipLaunchKernel(reinterpret_cast<const void*>(hip_kernel_api().gather_sparse_positions_kernel),
                                   dim3(2U, 2U), dim3(kSparseKernelThreads), gather_arguments, 0, nullptr),
                   hipSuccess);
        cursors.values[2] = 5U;
        input.require_same();
        candidates.require_same();
        counts.require_same();
        cursors.require_same();
        require_sparse_positions(positions, capacity);
    }
    positions.release("sparse saturation positions release");
    cursors.release("sparse saturation cursors release");
    counts.release("sparse saturation counts release");
    candidates.release("sparse saturation candidates release");
    input.release("sparse saturation input release");
#else
    REQUIRE_TRUE(!flitzzip::hip_device_available());
#endif
}

// Purpose: Qualify the materializer retained by the exact private v1 ABI and capability-completeness table.
// Inputs: Independent raw/fill/nonzero-pattern windows, an unsupported decoder window, two contained output refusals
// and one resident surplus block; seven 64-lane groups, at most one loop iteration per lane and zero barriers.
// Outputs: Exact valid bytes and unchanged gaps/guards; repaired output extents reuse the same resident owners.
TEST_CASE(gpu_retained_materializer_independent_windows_and_bounded_refusal) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::hip_device_available()) {
        return;
    }
    flitzzip::require_hip_device_ready();
    constexpr std::array<std::byte, 11> raw{std::byte{0},    std::byte{0xF1}, std::byte{0x44}, std::byte{0x7E},
                                            std::byte{0x80}, std::byte{0x2A}, std::byte{0x19}, std::byte{0xD0},
                                            std::byte{3},    std::byte{0xB7}, std::byte{0x55}};
    constexpr std::array<std::byte, 3> motif{std::byte{0x18}, std::byte{0xC3}, std::byte{0x5E}};
    constexpr std::array<std::byte, 17> repeated{
        std::byte{0x18}, std::byte{0xC3}, std::byte{0x5E}, std::byte{0x18}, std::byte{0xC3}, std::byte{0x5E},
        std::byte{0x18}, std::byte{0xC3}, std::byte{0x5E}, std::byte{0x18}, std::byte{0xC3}, std::byte{0x5E},
        std::byte{0x18}, std::byte{0xC3}, std::byte{0x5E}, std::byte{0x18}, std::byte{0xC3}};
    std::vector<std::byte> source(112U, kInputGuard);
    std::copy(raw.begin(), raw.end(), source.begin() + 37U);
    std::copy(motif.begin(), motif.end(), source.begin() + 63U);
    std::vector<DeviceBlock> metadata(9U);
    auto representation = std::as_writable_bytes(std::span(metadata));
    std::fill(representation.begin(), representation.end(), std::byte{0});
    for (auto& block : metadata) {
        block.kind = static_cast<std::uint8_t>(flitzzip::BlockKind::Deflate);
        block.fill_value = 0xECU;
        block.reserved = 0xA5A5U;
        block.encoded_offset = 37U;
        block.encoded_len = 3U;
    }
    metadata[1].kind = static_cast<std::uint8_t>(flitzzip::BlockKind::Raw);
    metadata[1].uncompressed_len = metadata[1].encoded_len = 11U;
    metadata[1].output_offset = 5U;
    metadata[2].kind = static_cast<std::uint8_t>(flitzzip::BlockKind::Fill);
    metadata[2].fill_value = 0x6DU;
    metadata[2].uncompressed_len = 9U;
    metadata[2].encoded_len = 0U;
    metadata[2].output_offset = 23U;
    metadata[3].kind = static_cast<std::uint8_t>(flitzzip::BlockKind::Pattern);
    metadata[3].uncompressed_len = 17U;
    metadata[3].encoded_offset = 63U;
    metadata[3].output_offset = 41U;
    metadata[4].uncompressed_len = 7U;
    metadata[4].output_offset = 66U;
    metadata[5].kind = metadata[6].kind = static_cast<std::uint8_t>(flitzzip::BlockKind::Raw);
    metadata[5].uncompressed_len = metadata[5].encoded_len = 1U;
    metadata[5].output_offset = 97U;
    metadata[6].uncompressed_len = metadata[6].encoded_len = 4U;
    metadata[6].output_offset = 94U;
    metadata[7].kind = static_cast<std::uint8_t>(flitzzip::BlockKind::Fill);
    metadata[7].uncompressed_len = 5U;
    metadata[7].encoded_len = 0U;
    metadata[7].output_offset = 82U;
    ResidentOracle<std::byte> payload(std::move(source), "retained materializer payload");
    ResidentOracle<DeviceBlock> blocks(std::move(metadata), "retained materializer blocks");
    ResidentOracle<std::byte> output(std::vector<std::byte>(160U, kOutputGuard), "retained materializer output");
    for (const auto repaired : std::array<bool, 2>{false, true}) {
        if (repaired) {
            blocks.values[5].output_offset = 76U;
            blocks.values[6].output_offset = 87U;
            blocks.upload();
        }
        std::fill(output.values.begin(), output.values.end(), kOutputGuard);
        output.upload();
        const auto* from = payload.device();
        const auto* plans = blocks.device() + 1U;
        std::uint32_t count = 6U;
        auto* to = output.device() + 32U;
        std::size_t extent = 96U;
        void* arguments[]{&from, &plans, &count, &to, &extent};
        REQUIRE_EQ(hipLaunchKernel(reinterpret_cast<const void*>(hip_kernel_api().materialize_blocks_kernel), dim3(7U),
                                   dim3(64U), arguments, 0, nullptr),
                   hipSuccess);
        std::copy(raw.begin(), raw.end(), output.values.begin() + 37U);
        std::fill_n(output.values.begin() + 55U, 9U, std::byte{0x6D});
        std::copy(repeated.begin(), repeated.end(), output.values.begin() + 73U);
        if (repaired) {
            output.values[108U] = raw[0];
            std::copy_n(raw.begin(), 4U, output.values.begin() + 119U);
        }
        payload.require_same();
        blocks.require_same();
        output.require_same();
    }
    output.release("retained materializer output release");
    blocks.release("retained materializer blocks release");
    payload.release("retained materializer payload release");
#else
    REQUIRE_TRUE(!flitzzip::hip_device_available());
#endif
}
