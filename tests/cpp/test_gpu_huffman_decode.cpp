#include "test_util.hpp"

#include "core/result.hpp"
#include "gpu/gpu_codec.hpp"
#include "gpu/hip_device.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#if FLITZZIP_ENABLE_HIP
namespace {
struct IndependentHuffmanWire {
    std::vector<std::byte> payload;
    std::vector<std::byte> expected;
    std::uint32_t encoded_segment_bytes;
};

// Purpose: Assemble version-eight bytes from complete one/two-bit codebooks without a production encoder.
// Inputs: A code width of one or two. Outputs: Three independently framed 4096-symbol segments and literal cycles.
IndependentHuffmanWire independent_huffman_wire(std::uint8_t width) {
    REQUIRE_TRUE(width == 1U || width == 2U);
    const std::array<std::uint8_t, 4> symbols = width == 1U ? std::array<std::uint8_t, 4>{0x41, 0xD2, 0, 0}
                                                            : std::array<std::uint8_t, 4>{0x21, 0x80, 0xA7, 0xFE};
    IndependentHuffmanWire result{{}, {}, 512U * width};
    for (std::uint32_t slot = 0; slot < 4096U; ++slot) {
        result.payload.push_back(std::byte{symbols[slot & ((1U << width) - 1U)]});
        result.payload.push_back(std::byte{width});
    }
    for (std::uint32_t segment = 0; segment <= 3U; ++segment) {
        const auto offset = segment * result.encoded_segment_bytes;
        for (unsigned byte = 0; byte < 4U; ++byte) {
            result.payload.push_back(std::byte{static_cast<std::uint8_t>(offset >> (8U * byte))});
        }
    }
    result.payload.insert(result.payload.end(), 3U * result.encoded_segment_bytes,
                          width == 1U ? std::byte{0xA6} : std::byte{0xE4});
    // LSB-first A6 bits are 0,1,1,0,0,1,0,1; E4 two-bit groups are 0,1,2,3.
    const std::vector<std::byte> cycle =
        width == 1U ? std::vector<std::byte>{std::byte{0x41}, std::byte{0xD2}, std::byte{0xD2}, std::byte{0x41},
                                             std::byte{0x41}, std::byte{0xD2}, std::byte{0x41}, std::byte{0xD2}}
                    : std::vector<std::byte>{std::byte{0x21}, std::byte{0x80}, std::byte{0xA7}, std::byte{0xFE}};
    for (std::size_t position = 0; position < 12288U; ++position) {
        result.expected.push_back(cycle[position % cycle.size()]);
    }
    REQUIRE_EQ(result.payload.size(), width == 1U ? 9744U : 11280U);
    REQUIRE_TRUE(result.payload.size() < result.expected.size());
    return result;
}
}  // namespace
#endif

// Purpose: Exercise the actual Huffman decoder with independent legal bytes, bounded bit exhaustion and reuse.
// Inputs: Complete one/two-bit tables; final segments missing one byte or all their bytes, with dense valid offsets.
// Outputs: CPU/HIP success matches explicit symbol cycles; GPU rejection preserves caller output and then recovers.
TEST_CASE(gpu_huffman_independent_wire_truncation_and_recovery) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    using namespace flitzzip;
    for (const auto width : std::array<std::uint8_t, 2>{1U, 2U}) {
        const auto fixture = independent_huffman_wire(width);
        const std::array blocks{BlockDescriptor{.kind = BlockKind::GpuHuffman,
                                                .uncompressed_len = 12288U,
                                                .encoded_len = static_cast<std::uint32_t>(fixture.payload.size())}};
        std::vector<std::byte> output(fixture.expected.size() + 64U, std::byte{0xA5});
        auto body = std::span(output).subspan(32U, fixture.expected.size());
        auto expected = output;
        std::copy(fixture.expected.begin(), fixture.expected.end(), expected.begin() + 32U);
        decode_chunk_cpu(fixture.payload, blocks, body, {});
        REQUIRE_EQ(output, expected);
        GpuCodecOptions gpu{.require_gpu = true};
        std::fill(output.begin(), output.end(), std::byte{0xA5});
        REQUIRE_TRUE(decode_chunk(fixture.payload, blocks, body, gpu));
        REQUIRE_EQ(output, expected);
        for (const auto cut : std::array<std::uint32_t, 2>{1U, fixture.encoded_segment_bytes}) {
            auto damaged = fixture.payload;
            const auto final_offset = 3U * fixture.encoded_segment_bytes - cut;
            damaged.resize(8192U + 16U + final_offset);
            for (unsigned byte = 0; byte < 4U; ++byte) {
                damaged[8192U + 12U + byte] = std::byte{static_cast<std::uint8_t>(final_offset >> (8U * byte))};
            }
            auto bad_blocks = blocks;
            bad_blocks[0].encoded_len = static_cast<std::uint32_t>(damaged.size());
            std::fill(output.begin(), output.end(), std::byte{0xA5});
            const auto preserved = output;
            bool rejected = false;
            try {
                (void)decode_chunk(damaged, bad_blocks, body, gpu);
            } catch (const ArchiveError& error) {
                rejected = std::string(error.what()) == "GPU prefix block contains a truncated or invalid codeword";
            }
            REQUIRE_TRUE(rejected);
            REQUIRE_EQ(output, preserved);
            REQUIRE_TRUE(decode_chunk(fixture.payload, blocks, body, gpu));
            REQUIRE_EQ(output, expected);
        }
    }
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}
