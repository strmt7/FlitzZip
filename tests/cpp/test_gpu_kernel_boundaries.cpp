#include "test_util.hpp"

#include "gpu/hip_device.hpp"

#if FLITZZIP_ENABLE_HIP
#include "gpu/hip_kernel_api.hpp"
#endif

#include <array>
#include <span>

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
