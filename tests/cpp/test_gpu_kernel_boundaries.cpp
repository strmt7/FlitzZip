#include "test_util.hpp"

#include "gpu/hip_device.hpp"
#include "lz4.h"

#if FLITZZIP_ENABLE_HIP
#include "gpu/hip_crc.hpp"
#include "gpu/hip_kernel_api.hpp"
#endif

#include <array>
#include <algorithm>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#if FLITZZIP_ENABLE_HIP
namespace {

// Purpose: Supply a bitwise IEEE CRC oracle independent of production tables and device reduction geometry.
// Inputs: A bounded byte span. Outputs: Its finalized CRC; the test also checks the standard numeric vector.
std::uint32_t bitwise_crc(std::span<const std::byte> bytes) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        crc ^= static_cast<std::uint8_t>(byte);
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

}  // namespace
#endif

// Purpose: Verify the production Neutron byte-plane kernel against a separate column traversal and allocation guards.
// Inputs: Empty, partial-word, workgroup-boundary and multi-workgroup inputs with several plane widths.
// Outputs: Exact forward/inverse bytes and unchanged guard tails through actual HIP execution when available.
TEST_CASE(gpu_byte_plane_kernel_independent_oracle_and_guards) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const auto& api = flitzzip::hip_kernel_api();
    using flitzzip::hip_detail::HipDeviceBuffer;
    for (const std::uint32_t bytes : {0U, 1U, 2U, 3U, 7U, 15U, 31U, 255U, 256U, 257U, 4095U, 4096U, 4097U, 65537U}) {
        for (const std::uint32_t width : {1U, 2U, 3U, 4U, 8U, 16U, 64U}) {
            std::vector<std::byte> input(bytes + 128U), expected(bytes + 128U, std::byte{0xA5});
            for (std::size_t index = 0; index < input.size(); ++index) {
                input[index] = std::byte((index * 83U + index / 19U) & 255U);
            }
            std::size_t cursor = 0;
            for (std::uint32_t column = 0; column < width; ++column) {
                for (std::uint32_t row = 0; row < bytes / width; ++row) {
                    expected[cursor++] = input[row * width + column];
                }
            }
            while (cursor < bytes) {
                expected[cursor] = input[cursor];
                ++cursor;
            }
            HipDeviceBuffer<std::byte> source(input.size(), "test byte-plane source");
            HipDeviceBuffer<std::byte> destination(expected.size(), "test byte-plane destination");
            REQUIRE_EQ(hipMemcpy(source.get(), input.data(), input.size(), hipMemcpyHostToDevice), hipSuccess);
            REQUIRE_EQ(hipMemset(destination.get(), 0xA5, expected.size()), hipSuccess);
            const auto* from = source.get();
            auto* to = destination.get();
            auto length = bytes;
            auto plane = width;
            bool inverse = false;
            void* arguments[]{&from, &to, &length, &plane, &inverse};
            REQUIRE_EQ(hipLaunchKernel(reinterpret_cast<const void*>(api.byte_plane_transform), dim3(bytes / 256U + 1U),
                                       dim3(256), arguments, 0, nullptr),
                       hipSuccess);
            std::vector<std::byte> actual(expected.size());
            REQUIRE_EQ(hipMemcpy(actual.data(), destination.get(), actual.size(), hipMemcpyDeviceToHost), hipSuccess);
            REQUIRE_EQ(actual, expected);
            REQUIRE_EQ(hipMemset(source.get(), 0xA5, input.size()), hipSuccess);
            from = destination.get();
            to = source.get();
            inverse = true;
            REQUIRE_EQ(hipLaunchKernel(reinterpret_cast<const void*>(api.byte_plane_transform), dim3(bytes / 256U + 1U),
                                       dim3(256), arguments, 0, nullptr),
                       hipSuccess);
            REQUIRE_EQ(hipMemcpy(actual.data(), source.get(), actual.size(), hipMemcpyDeviceToHost), hipSuccess);
            std::fill(input.begin() + bytes, input.end(), std::byte{0xA5});
            REQUIRE_EQ(actual, input);
            destination.reset_checked("test byte-plane destination release");
            source.reset_checked("test byte-plane source release");
        }
    }
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

#if FLITZZIP_ENABLE_HIP
namespace {

struct DictionaryDecodeOracle {
    std::vector<std::byte> encoded;
    std::uint32_t decoded_bytes;
    std::vector<std::byte> expected;
    bool valid;
};

// Purpose: Construct explicit binary dictionary fixtures without invoking the production encoder or reader.
// Inputs: Literal wire bytes, exact decoded extent and independently derived output for valid cases.
// Outputs: An owned small test packet; invalid packets intentionally have no expected decoded payload.
DictionaryDecodeOracle dictionary_decode_oracle(std::initializer_list<std::uint8_t> wire, std::uint32_t decoded_bytes,
                                                std::vector<std::byte> expected = {}, bool valid = false) {
    DictionaryDecodeOracle result{{}, decoded_bytes, std::move(expected), valid};
    for (const auto byte : wire) {
        result.encoded.push_back(std::byte{byte});
    }
    return result;
}

// Purpose: Exercise real cooperative device parsing without allowing one segment to overwrite its neighbors.
// Inputs: Independently specified valid and malformed packets, each with separate bounded input/output spans.
// Outputs: Exact statuses and valid bytes, unchanged uploaded inputs/tables, untouched output and status canaries.
void require_dictionary_device_oracles(const std::vector<DictionaryDecodeOracle>& cases) {
    using namespace flitzzip;
    using namespace flitzzip::hip_detail;
    std::vector<std::byte> wire(8U, std::byte{0xD6}), output(8U, std::byte{0xA7});
    std::vector<DictionarySegmentSpan> spans;
    for (const auto& packet : cases) {
        if (packet.valid) {
            std::vector<std::byte> independent(packet.decoded_bytes);
            REQUIRE_EQ(LZ4_decompress_safe(reinterpret_cast<const char*>(packet.encoded.data()),
                                           reinterpret_cast<char*>(independent.data()),
                                           static_cast<int>(packet.encoded.size()),
                                           static_cast<int>(independent.size())),
                       static_cast<int>(packet.decoded_bytes));
            REQUIRE_EQ(independent, packet.expected);
        }
        spans.push_back({static_cast<std::uint32_t>(wire.size()), static_cast<std::uint32_t>(packet.encoded.size()),
                         static_cast<std::uint32_t>(output.size()), packet.decoded_bytes});
        wire.insert(wire.end(), packet.encoded.begin(), packet.encoded.end());
        wire.insert(wire.end(), 8U, std::byte{0xD6});
        output.insert(output.end(), packet.decoded_bytes + 8U, std::byte{0xA7});
    }
    HipDeviceBuffer<std::byte> input(wire.size(), "dictionary oracle input");
    HipDeviceBuffer<std::byte> decoded(output.size(), "dictionary oracle output");
    HipDeviceBuffer<DictionarySegmentSpan> plans(spans.size() * sizeof(DictionarySegmentSpan),
                                                 "dictionary oracle spans");
    HipDeviceBuffer<std::uint32_t> statuses((cases.size() + 1U) * sizeof(std::uint32_t), "dictionary oracle statuses");
    std::vector<std::uint32_t> actual_status(cases.size() + 1U, 0xA5A5A5A5U);
    REQUIRE_EQ(hipMemcpy(input.get(), wire.data(), wire.size(), hipMemcpyHostToDevice), hipSuccess);
    REQUIRE_EQ(hipMemcpy(decoded.get(), output.data(), output.size(), hipMemcpyHostToDevice), hipSuccess);
    REQUIRE_EQ(hipMemcpy(plans.get(), spans.data(), spans.size() * sizeof(spans[0]), hipMemcpyHostToDevice),
               hipSuccess);
    REQUIRE_EQ(hipMemcpy(statuses.get(), actual_status.data(), actual_status.size() * sizeof(actual_status[0]),
                         hipMemcpyHostToDevice),
               hipSuccess);
    const auto* from = input.get();
    const auto* metadata = plans.get();
    auto* to = decoded.get();
    auto* status = statuses.get();
    void* arguments[]{&from, &metadata, &to, &status};
    REQUIRE_EQ(hipLaunchKernel(reinterpret_cast<const void*>(hip_kernel_api().decode_dictionary_segments),
                               dim3(static_cast<unsigned>(cases.size())), dim3(kDictionaryKernelThreads), arguments, 0,
                               nullptr),
               hipSuccess);
    std::vector<std::byte> actual(output.size());
    REQUIRE_EQ(hipMemcpy(actual.data(), decoded.get(), actual.size(), hipMemcpyDeviceToHost), hipSuccess);
    REQUIRE_EQ(hipMemcpy(actual_status.data(), statuses.get(), actual_status.size() * sizeof(actual_status[0]),
                         hipMemcpyDeviceToHost),
               hipSuccess);
    REQUIRE_EQ(actual_status.back(), 0xA5A5A5A5U);
    REQUIRE_TRUE(std::all_of(actual.begin(), actual.begin() + 8U, [](auto byte) { return byte == std::byte{0xA7}; }));
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto& packet = cases[index];
        const auto& span = spans[index];
        REQUIRE_EQ(actual_status[index], packet.valid ? 1U : 0U);
        if (packet.valid) {
            REQUIRE_EQ(packet.expected.size(), packet.decoded_bytes);
            REQUIRE_TRUE(
                std::equal(packet.expected.begin(), packet.expected.end(), actual.begin() + span.decoded_offset));
        }
        const auto guard = actual.begin() + span.decoded_offset + span.decoded_size;
        REQUIRE_TRUE(std::all_of(guard, guard + 8U, [](auto byte) { return byte == std::byte{0xA7}; }));
    }
    actual.resize(wire.size());
    REQUIRE_EQ(hipMemcpy(actual.data(), input.get(), actual.size(), hipMemcpyDeviceToHost), hipSuccess);
    REQUIRE_EQ(actual, wire);
    const auto original_spans = std::as_bytes(std::span(spans));
    actual.resize(original_spans.size());
    REQUIRE_EQ(hipMemcpy(actual.data(), plans.get(), actual.size(), hipMemcpyDeviceToHost), hipSuccess);
    REQUIRE_TRUE(std::equal(actual.begin(), actual.end(), original_spans.begin()));
    statuses.reset_checked("dictionary oracle statuses release");
    plans.reset_checked("dictionary oracle spans release");
    decoded.reset_checked("dictionary oracle output release");
    input.reset_checked("dictionary oracle input release");
}

}  // namespace
#endif

// Purpose: Verify dictionary GPU parsing against independent legal blocks and adjacent malformed wire boundaries.
// Inputs: Small literal, overlapping-match and extended-length LZ4 blocks; bounded malformed neighbors.
// Outputs: Exact success bytes or rejection, with per-segment guards and preserved input ownership.
TEST_CASE(gpu_dictionary_decoder_independent_wire_and_rejection_guards) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> repeated(16U, std::byte{0x41});
    repeated.insert(repeated.end(),
                    {std::byte{0x42}, std::byte{0x43}, std::byte{0x44}, std::byte{0x45}, std::byte{0x46}});
    std::vector<DictionaryDecodeOracle> cases{
        dictionary_decode_oracle({0x10, 0x41}, 1U, {std::byte{0x41}}, true),
        dictionary_decode_oracle({0x1B, 0x41, 1, 0, 0x50, 0x42, 0x43, 0x44, 0x45, 0x46}, 21U, repeated, true),
        dictionary_decode_oracle({}, 1U),
        dictionary_decode_oracle({0xF0, 255}, 300U),
        dictionary_decode_oracle({0xF0, 0}, 14U),
        dictionary_decode_oracle({0xF0, 255}, 32U),
        dictionary_decode_oracle({0x30, 0x41, 0x42}, 3U),
        dictionary_decode_oracle({0x10, 0x41, 1}, 20U),
        dictionary_decode_oracle({0x10, 0x41, 0, 0}, 20U),
        dictionary_decode_oracle({0x10, 0x41, 2, 0}, 20U),
        dictionary_decode_oracle({0x10, 0x41, 1, 0}, 3U),
        dictionary_decode_oracle({0x1F, 0x41, 1, 0, 255}, 20U),
        dictionary_decode_oracle({0x1F, 0x41, 1, 0, 255}, 300U),
        dictionary_decode_oracle({0x10, 0x41}, 2U),
        dictionary_decode_oracle({0x20, 0x41, 0x42}, 1U),
        dictionary_decode_oracle({0x1B, 0x41, 1, 0}, 21U),
        dictionary_decode_oracle({0x18, 0x41, 1, 0, 0x40, 0x42, 0x43, 0x44, 0x45}, 17U),
        dictionary_decode_oracle({0x12, 0x41, 1, 0, 0x50, 0x42, 0x43, 0x44, 0x45, 0x46}, 12U),
    };
    auto extended = dictionary_decode_oracle({0xF0, 255, 0}, 270U, std::vector<std::byte>(270U, std::byte{0x53}), true);
    extended.encoded.insert(extended.encoded.end(), 270U, std::byte{0x53});
    cases.push_back(std::move(extended));
    auto long_match = dictionary_decode_oracle({0x1F, 0x41, 1, 0, 255, 0, 0x50, 0x42, 0x43, 0x44, 0x45, 0x46}, 280U);
    long_match.expected.assign(275U, std::byte{0x41});
    long_match.expected.insert(long_match.expected.end(), repeated.end() - 5U, repeated.end());
    long_match.valid = true;
    cases.push_back(std::move(long_match));
    auto shortest = dictionary_decode_oracle({0x13, 0x41, 1, 0, 0x50, 0x42, 0x43, 0x44, 0x45, 0x46}, 13U);
    shortest.expected.assign(8U, std::byte{0x41});
    shortest.expected.insert(shortest.expected.end(), repeated.end() - 5U, repeated.end());
    shortest.valid = true;
    cases.push_back(std::move(shortest));
    auto periodic = dictionary_decode_oracle({0xFF, 242}, 1289U);
    for (unsigned index = 0; index < 257U; ++index) {
        periodic.encoded.push_back(std::byte{static_cast<std::uint8_t>(index * 37U + 19U)});
    }
    periodic.expected.assign(periodic.encoded.begin() + 2U, periodic.encoded.end());
    for (unsigned index = 0; index < 1027U; ++index) {
        periodic.expected.push_back(periodic.expected[index % 257U]);
    }
    periodic.encoded.insert(periodic.encoded.end(), {std::byte{1}, std::byte{1}, std::byte{255}, std::byte{255},
                                                     std::byte{255}, std::byte{243}, std::byte{0x50}});
    periodic.encoded.insert(periodic.encoded.end(), repeated.end() - 5U, repeated.end());
    periodic.expected.insert(periodic.expected.end(), repeated.end() - 5U, repeated.end());
    periodic.valid = true;
    cases.push_back(std::move(periodic));
    require_dictionary_device_oracles(cases);
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

// Purpose: Preserve device-buffer ownership after rejected reuse and allow explicit release and reacquisition.
// Inputs: One real 64-byte allocation, independent byte pattern and a second allocation attempt.
// Outputs: Refusal preserves the pointer and bytes; empty/repeated cleanup and subsequent reuse remain valid.
TEST_CASE(gpu_device_buffer_rejection_preserves_owner_and_bytes) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    using namespace flitzzip::hip_detail;
    std::array<std::byte, 64> expected{}, actual{};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        expected[index] = std::byte((index * 19U + 7U) & 255U);
    }
    HipDeviceBuffer<std::byte> buffer;
    buffer.allocate_checked(0, "empty test allocation");
    REQUIRE_TRUE(buffer.get() == nullptr);
    buffer.reset_checked("release empty test buffer");
    buffer.allocate_checked(expected.size(), "allocate ownership test buffer");
    auto* const original = buffer.get();
    check_hip(copy_on_codec_stream(original, expected.data(), expected.size(), hipMemcpyHostToDevice),
              "upload ownership test bytes");
    for (const auto attempted : {0U, 32U, 128U}) {
        bool rejected = false;
        try {
            buffer.allocate_checked(attempted, "reject occupied test owner");
        } catch (const flitzzip::GpuError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_TRUE(buffer.get() == original);
        check_hip(copy_on_codec_stream(actual.data(), buffer.get(), actual.size(), hipMemcpyDeviceToHost),
                  "read preserved ownership test bytes");
        REQUIRE_TRUE(actual == expected);
    }
    buffer.reset_checked("release ownership test buffer");
    REQUIRE_TRUE(buffer.get() == nullptr);
    buffer.reset_checked("repeat empty test release");
    buffer.allocate_checked(expected.size(), "reacquire ownership test buffer");
    check_hip(copy_on_codec_stream(buffer.get(), expected.data(), expected.size(), hipMemcpyHostToDevice),
              "upload reacquired test bytes");
    check_hip(copy_on_codec_stream(actual.data(), buffer.get(), actual.size(), hipMemcpyDeviceToHost),
              "read reacquired test bytes");
    REQUIRE_TRUE(actual == expected);
    buffer.reset_checked("release reacquired test buffer");
#endif
}

// Purpose: Prove reservation moves, rejected requests and exception unwinding preserve exact device accounting.
// Inputs: Real selected device, small bookkeeping reservations and an impossible request rejected before allocation.
// Outputs: Each transfer releases exactly its prior ownership; all scope and failure exits restore the baseline.
TEST_CASE(gpu_device_reservation_move_and_failure_accounting) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    using namespace flitzzip::hip_detail;
    int device = -1;
    check_hip(hipGetDevice(&device), "select reservation test device");
    auto& accounting = hip_device_reservation_state();
    const auto baseline = accounting.reserved_bytes(device);
    {
        HipDeviceMemoryReservation empty;
        HipDeviceMemoryReservation first(4096U, "first test reservation");
        REQUIRE_EQ(accounting.reserved_bytes(device), baseline + 4096U);
        HipDeviceMemoryReservation moved(std::move(first));
        REQUIRE_EQ(accounting.reserved_bytes(device), baseline + 4096U);
        HipDeviceMemoryReservation second(8192U, "second test reservation");
        REQUIRE_EQ(accounting.reserved_bytes(device), baseline + 12288U);
        moved = std::move(second);
        REQUIRE_EQ(accounting.reserved_bytes(device), baseline + 8192U);
        auto* same = &moved;
        moved = std::move(*same);
        REQUIRE_EQ(accounting.reserved_bytes(device), baseline + 8192U);
        moved = std::move(empty);
        REQUIRE_EQ(accounting.reserved_bytes(device), baseline);
    }
    REQUIRE_EQ(accounting.reserved_bytes(device), baseline);
    bool rejected = false;
    try {
        HipDeviceMemoryReservation impossible(std::numeric_limits<std::size_t>::max(), "reject impossible reservation");
    } catch (const flitzzip::GpuError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(accounting.reserved_bytes(device), baseline);
    bool unwound = false;
    try {
        HipDeviceMemoryReservation scoped(4096U, "exception test reservation");
        REQUIRE_EQ(accounting.reserved_bytes(device), baseline + 4096U);
        throw std::runtime_error("reservation test cancellation");
    } catch (const std::runtime_error& error) {
        unwound = std::string_view(error.what()) == "reservation test cancellation";
    }
    REQUIRE_TRUE(unwound);
    REQUIRE_EQ(accounting.reserved_bytes(device), baseline);
#endif
}

// Purpose: Check cooperative CRC reduction across empty, partial and nonuniform ranges with an extra grid block.
// Inputs: Actual production kernel, independent bitwise CRCs and a guarded result allocation.
// Outputs: Exact checksums and lengths; excess workgroups cannot modify the output guard.
TEST_CASE(gpu_crc_ranges_independent_oracle_and_guard) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    using namespace flitzzip::hip_detail;
    const auto& api = flitzzip::hip_kernel_api();
    constexpr std::array known{std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'}, std::byte{'5'},
                               std::byte{'6'}, std::byte{'7'}, std::byte{'8'}, std::byte{'9'}};
    REQUIRE_EQ(bitwise_crc(known), 0xCBF43926U);
    REQUIRE_EQ(bitwise_crc({}), 0U);
    std::vector<std::byte> input(65537U + 128U);
    for (std::size_t index = 0; index < input.size(); ++index) {
        input[index] = std::byte((index * 37U + index / 5U) & 255U);
    }
    std::vector<CrcInputRange> plans;
    for (std::uint32_t length : {0U, 1U, 2U, 255U, 256U, 257U, 8192U, 32768U, 65537U}) {
        plans.push_back({3U, length});
    }
    std::vector<DeviceCrcSegment> result(plans.size() + 1U);
    HipDeviceBuffer<std::byte> source(input.size(), "test CRC source");
    HipDeviceBuffer<CrcInputRange> metadata(plans.size() * sizeof(CrcInputRange), "test CRC ranges");
    HipDeviceBuffer<DeviceCrcSegment> output(result.size() * sizeof(DeviceCrcSegment), "test CRC output");
    REQUIRE_EQ(hipMemcpy(source.get(), input.data(), input.size(), hipMemcpyHostToDevice), hipSuccess);
    REQUIRE_EQ(hipMemcpy(metadata.get(), plans.data(), plans.size() * sizeof(CrcInputRange), hipMemcpyHostToDevice),
               hipSuccess);
    REQUIRE_EQ(hipMemset(output.get(), 0xA5, result.size() * sizeof(DeviceCrcSegment)), hipSuccess);
    const auto* from = source.get();
    const auto* descriptors = metadata.get();
    auto* to = output.get();
    auto count = static_cast<std::uint32_t>(plans.size());
    void* arguments[]{&from, &descriptors, &to, &count};
    REQUIRE_EQ(hipLaunchKernel(reinterpret_cast<const void*>(api.crc32_independent_ranges_kernel), dim3(count + 1U),
                               dim3(256), arguments, 0, nullptr),
               hipSuccess);
    REQUIRE_EQ(hipMemcpy(result.data(), output.get(), result.size() * sizeof(DeviceCrcSegment), hipMemcpyDeviceToHost),
               hipSuccess);
    for (std::size_t index = 0; index < plans.size(); ++index) {
        REQUIRE_EQ(result[index].length, plans[index].length);
        REQUIRE_EQ(result[index].crc32,
                   bitwise_crc(std::span(input).subspan(plans[index].offset, plans[index].length)));
    }
    REQUIRE_EQ(result.back().crc32, 0xA5A5A5A5U);
    REQUIRE_EQ(result.back().length, 0xA5A5A5A5U);
    output.reset_checked("test CRC output release");
    metadata.reset_checked("test CRC ranges release");
    source.reset_checked("test CRC source release");
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

// Purpose: Verify decoded-stream CRC across mixed block and cooperative-segment boundaries without a GPU decoder.
// Inputs: Independently generated raw/fill/motif bytes and noncontiguous encoded offsets, including short blocks.
// Outputs: Exact bitwise-oracle CRC and unchanged uploaded payload/table; no decoded GPU output allocation.
TEST_CASE(gpu_decoded_crc_mixed_blocks_matches_bitwise_oracle) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    using namespace flitzzip::hip_detail;
    constexpr std::array<std::uint32_t, 9> lengths{1U, 255U, 257U, 257U, 8193U, 513U, 1U, 255U, 4097U};
    constexpr std::array<std::uint32_t, 9> periods{0U, 0U, 3U, 0U, 0U, 257U, 0U, 0U, 17U};
    std::vector<std::byte> payload(11U, std::byte{0xD6}), decoded;
    std::vector<DeviceBlock> plans;
    for (std::size_t index = 0; index < lengths.size(); ++index) {
        const auto kind = static_cast<std::uint8_t>(index % 3U == 0U ? 0U : index % 3U == 1U ? 1U : 3U);
        const auto begin = decoded.size();
        for (std::uint32_t position = 0; position < lengths[index]; ++position) {
            const auto value =
                kind == 1U ? 0x73U : ((periods[index] ? position % periods[index] : position) * 29U + index);
            decoded.push_back(std::byte(value & 255U));
        }
        const auto encoded = kind == 1U ? 0U : kind == 3U ? periods[index] : lengths[index];
        plans.push_back({kind, 0x73U, 0U, lengths[index], payload.size(), begin, encoded});
        payload.insert(payload.end(), decoded.begin() + begin, decoded.begin() + begin + encoded);
        payload.insert(payload.end(), 7U, std::byte{0xD6});
    }
    REQUIRE_TRUE(decoded.size() > kSmallCrcSegmentBytes);
    HipDeviceBuffer<std::byte> source(payload.size(), "decoded CRC oracle payload");
    HipDeviceBuffer<DeviceBlock> descriptors(plans.size() * sizeof(DeviceBlock), "decoded CRC oracle plans");
    REQUIRE_EQ(hipMemcpy(source.get(), payload.data(), payload.size(), hipMemcpyHostToDevice), hipSuccess);
    REQUIRE_EQ(hipMemcpy(descriptors.get(), plans.data(), plans.size() * sizeof(DeviceBlock), hipMemcpyHostToDevice),
               hipSuccess);
    REQUIRE_EQ(compute_decoded_crc32_device(source.get(), descriptors.get(), static_cast<std::uint32_t>(plans.size()),
                                            decoded.size(), nullptr, "decoded CRC oracle"),
               bitwise_crc(decoded));
    std::vector<std::byte> actual(payload.size());
    REQUIRE_EQ(hipMemcpy(actual.data(), source.get(), actual.size(), hipMemcpyDeviceToHost), hipSuccess);
    REQUIRE_EQ(actual, payload);
    const auto original_plans = std::as_bytes(std::span(plans));
    actual.resize(original_plans.size());
    REQUIRE_EQ(hipMemcpy(actual.data(), descriptors.get(), actual.size(), hipMemcpyDeviceToHost), hipSuccess);
    REQUIRE_TRUE(std::equal(actual.begin(), actual.end(), original_plans.begin(), original_plans.end()));
    descriptors.reset_checked("decoded CRC oracle plans release");
    source.reset_checked("decoded CRC oracle payload release");
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

// Purpose: Reject sampled compression candidates that disagree with any byte, including segment tails.
// Inputs: Independent fill/257-byte motif data, late corruptions and invalid candidate kinds/periods.
// Outputs: Exact per-candidate mismatch flags, unchanged input and an untouched flag guard on the actual GPU.
TEST_CASE(gpu_candidate_verification_rejects_unsampled_corruption) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    using namespace flitzzip::hip_detail;
    const auto& api = flitzzip::hip_kernel_api();
    constexpr std::uint32_t length = 65537U;
    constexpr std::uint32_t period = 257U;
    std::vector<std::byte> original(length * 2U, std::byte{0x2D});
    for (std::size_t index = 0; index < length; ++index) {
        original[length + index] = std::byte(((index % period) * 37U + 19U) & 255U);
    }
    const std::array<DeviceBlock, 5> plans{{
        {0U, 0U, 0U, length, 0U, 0U, length},
        {1U, 0x2DU, 0U, length, 0U, 0U, 0U},
        {3U, 0U, 0U, length, 0U, length, period},
        {255U, 0U, 0U, length, 0U, 0U, 0U},
        {3U, 0U, 0U, length, 0U, 0U, 1U},
    }};
    HipDeviceBuffer<std::byte> source(original.size(), "candidate oracle source");
    HipDeviceBuffer<DeviceBlock> descriptors(sizeof(plans), "candidate oracle plans");
    HipDeviceBuffer<std::uint32_t> flags((plans.size() + 1U) * sizeof(std::uint32_t), "candidate oracle flags");
    REQUIRE_EQ(hipMemcpy(descriptors.get(), plans.data(), sizeof(plans), hipMemcpyHostToDevice), hipSuccess);
    // The first period is the sample. Every corrupted pattern byte lies beyond it.
    const std::array<std::size_t, 10> corruptions{original.size(),
                                                  0U,
                                                  255U,
                                                  length - 2U,
                                                  length - 1U,
                                                  length + period,
                                                  length + 511U,
                                                  length + 65535U,
                                                  length + 65536U,
                                                  length + 4097U};
    for (const auto corruption : corruptions) {
        auto input = original;
        const bool changed = corruption < input.size();
        if (changed) {
            input[corruption] ^= std::byte{0x80};
        }
        REQUIRE_EQ(hipMemcpy(source.get(), input.data(), input.size(), hipMemcpyHostToDevice), hipSuccess);
        const std::array<std::uint32_t, 6> initial{0U, 0U, 0U, 0U, 0U, 0xA5A5A5A5U};
        REQUIRE_EQ(hipMemcpy(flags.get(), initial.data(), sizeof(initial), hipMemcpyHostToDevice), hipSuccess);
        const auto* from = source.get();
        const auto* metadata = descriptors.get();
        auto* mismatch = flags.get();
        auto bytes = input.size();
        auto count = static_cast<std::uint32_t>(plans.size());
        std::uint32_t segments = 2U;
        void* arguments[]{&from, &bytes, &metadata, &mismatch, &count, &segments};
        REQUIRE_EQ(hipLaunchKernel(reinterpret_cast<const void*>(api.verify_analysis_candidates_kernel),
                                   dim3(count * segments + 1U), dim3(256), arguments, 0, nullptr),
                   hipSuccess);
        std::array<std::uint32_t, 6> actual{};
        REQUIRE_EQ(hipMemcpy(actual.data(), flags.get(), sizeof(actual), hipMemcpyDeviceToHost), hipSuccess);
        const std::array<std::uint32_t, 6> expected{0U,
                                                    static_cast<std::uint32_t>(changed && corruption < length),
                                                    static_cast<std::uint32_t>(changed && corruption >= length),
                                                    1U,
                                                    1U,
                                                    0xA5A5A5A5U};
        REQUIRE_EQ(actual, expected);
        std::vector<std::byte> readback(input.size());
        REQUIRE_EQ(hipMemcpy(readback.data(), source.get(), readback.size(), hipMemcpyDeviceToHost), hipSuccess);
        REQUIRE_EQ(readback, input);
    }
    flags.reset_checked("candidate oracle flags release");
    descriptors.reset_checked("candidate oracle plans release");
    source.reset_checked("candidate oracle source release");
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

// Purpose: Verify diagnostic reduction ownership, 64-bit carry and guard preservation independently of its GPU tree.
// Inputs: Empty, boundary and strided word counts; three supported 256-thread groups and full-width input values.
// Outputs: Exact per-group sums and a closed-form whole-buffer sum, unchanged input and an untouched output guard.
TEST_CASE(gpu_diagnostic_checksum_independent_sum_and_guards) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    using flitzzip::hip_detail::HipDeviceBuffer;
    const auto& api = flitzzip::hip_kernel_api();
    constexpr unsigned groups = 3U;
    constexpr unsigned threads = 256U;
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    for (const std::size_t words : {0U, 1U, 255U, 256U, 257U, 4097U}) {
        std::vector<std::uint32_t> input(words + 32U, maximum);
        std::array<std::uint64_t, groups> expected{};
        for (std::size_t index = 0; index < words; ++index) {
            input[index] = static_cast<std::uint32_t>(maximum - index);
            expected[(index / threads) % groups] += input[index];
        }
        const auto count = static_cast<std::uint64_t>(words);
        const auto triangle = count == 0U ? 0U : count * (count - 1U) / 2U;
        const auto total = count * maximum - triangle;
        REQUIRE_EQ(expected[0] + expected[1] + expected[2], total);
        HipDeviceBuffer<std::uint32_t> source(input.size() * sizeof(input[0]), "diagnostic sum source");
        HipDeviceBuffer<std::uint64_t> partials((groups + 1U) * sizeof(std::uint64_t), "diagnostic sum partials");
        REQUIRE_EQ(hipMemcpy(source.get(), input.data(), input.size() * sizeof(input[0]), hipMemcpyHostToDevice),
                   hipSuccess);
        REQUIRE_EQ(hipMemset(partials.get(), 0xFF, (groups + 1U) * sizeof(std::uint64_t)), hipSuccess);
        const auto* from = source.get();
        auto* to = partials.get();
        auto length = words;
        void* arguments[]{&from, &length, &to};
        REQUIRE_EQ(hipLaunchKernel(reinterpret_cast<const void*>(api.diagnostic_checksum_kernel), dim3(groups),
                                   dim3(threads), arguments, threads * sizeof(std::uint64_t), nullptr),
                   hipSuccess);
        std::array<std::uint64_t, groups + 1U> actual{};
        REQUIRE_EQ(hipMemcpy(actual.data(), partials.get(), sizeof(actual), hipMemcpyDeviceToHost), hipSuccess);
        for (std::size_t group = 0; group < groups; ++group) {
            REQUIRE_EQ(actual[group], expected[group]);
        }
        REQUIRE_EQ(actual.back(), std::numeric_limits<std::uint64_t>::max());
        std::vector<std::uint32_t> readback(input.size());
        REQUIRE_EQ(
            hipMemcpy(readback.data(), source.get(), readback.size() * sizeof(readback[0]), hipMemcpyDeviceToHost),
            hipSuccess);
        REQUIRE_EQ(readback, input);
        partials.reset_checked("diagnostic sum partial release");
        source.reset_checked("diagnostic sum source release");
    }
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

// Purpose: Verify diagnostic integer mixing without reimplementing its iterative kernel as the CPU oracle.
// Inputs: Complemented indices make the initial XOR constant; three independently derived xorshift32 values.
// Outputs: Analytic zero/one-round words, defined 32-bit carry and unchanged guards across empty and strided launches.
TEST_CASE(gpu_diagnostic_compute_closed_form_and_guards) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    using flitzzip::hip_detail::HipDeviceBuffer;
    const auto& api = flitzzip::hip_kernel_api();
    struct Control {
        std::uint32_t seed;
        std::uint32_t initial;
        std::uint32_t mixed;
    };
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    // xorshift32(0) = 0, xorshift32(1) = 270369, xorshift32(2^31) = 0x80084000.
    constexpr std::array controls{Control{maximum, 0U, 0U}, Control{maximum - 1U, 1U, 270369U},
                                  Control{0x7FFFFFFFU, 0x80000000U, 0x80084000U}};
    for (const std::size_t words : {0U, 1U, 255U, 256U, 257U, 4097U}) {
        for (const auto& control : controls) {
            for (std::uint32_t rounds : {0U, 1U}) {
                std::vector<std::uint32_t> input(words + 32U, maximum);
                auto expected = input;
                for (std::size_t index = 0; index < words; ++index) {
                    input[index] = static_cast<std::uint32_t>(maximum - index);
                    const auto one_round = static_cast<std::uint64_t>(control.mixed) + 0x9E3779B9ULL + index;
                    expected[index] = rounds == 0U ? control.initial : static_cast<std::uint32_t>(one_round);
                }
                HipDeviceBuffer<std::uint32_t> buffer(input.size() * sizeof(input[0]), "diagnostic integer source");
                REQUIRE_EQ(
                    hipMemcpy(buffer.get(), input.data(), input.size() * sizeof(input[0]), hipMemcpyHostToDevice),
                    hipSuccess);
                auto* data = buffer.get();
                auto length = words;
                auto seed = control.seed;
                void* arguments[]{&data, &length, &seed, &rounds};
                REQUIRE_EQ(hipLaunchKernel(reinterpret_cast<const void*>(api.diagnostic_compute_kernel), dim3(3),
                                           dim3(256), arguments, 0, nullptr),
                           hipSuccess);
                std::vector<std::uint32_t> actual(input.size());
                REQUIRE_EQ(
                    hipMemcpy(actual.data(), buffer.get(), actual.size() * sizeof(actual[0]), hipMemcpyDeviceToHost),
                    hipSuccess);
                REQUIRE_EQ(actual, expected);
                buffer.reset_checked("diagnostic integer source release");
            }
        }
    }
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}
