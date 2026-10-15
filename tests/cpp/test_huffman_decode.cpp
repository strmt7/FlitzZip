#include "test_util.hpp"

#include "core/archive_blocks.hpp"
#include "core/result.hpp"

namespace {

// Purpose: Encode fixed-width little-endian table entries independently of the production parser.
// Inputs: Owned fixture buffer, bounded offset and 32-bit value. Outputs: Replaces exactly four bytes.
void put_huffman_offset(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    REQUIRE_TRUE(offset <= bytes.size() && bytes.size() - offset >= 4U);
    for (std::size_t index = 0; index < 4U; ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 255U);
    }
}

// Purpose: Construct a two-symbol version-eight frame without using the GPU encoder.
// Inputs: A bounded decoded size exceeding lookup overhead. Outputs: One-bit codes for alternating 201/202 bytes.
std::vector<std::byte> binary_huffman_fixture(std::uint32_t size) {
    const auto segments = (size + flitzzip::kGpuPrefixSegmentBytes - 1U) / flitzzip::kGpuPrefixSegmentBytes;
    const auto table_end = flitzzip::kGpuHuffmanLookupBytes + (segments + 1U) * 4U;
    std::vector<std::byte> bytes(table_end);
    for (std::size_t slot = 0; slot < flitzzip::kGpuHuffmanLookupEntries; ++slot) {
        bytes[slot * 2U] = static_cast<std::byte>(201U + (slot & 1U));
        bytes[slot * 2U + 1U] = std::byte{1};
    }
    for (std::uint32_t segment = 0; segment < segments; ++segment) {
        const auto count =
            std::min(flitzzip::kGpuPrefixSegmentBytes, size - segment * flitzzip::kGpuPrefixSegmentBytes);
        const auto start = bytes.size();
        bytes.resize(start + ((count + 31U) / 32U) * 4U);
        for (std::size_t index = 1; index < count; index += 2U) {
            bytes[start + index / 8U] |= static_cast<std::byte>(1U << (index & 7U));
        }
        put_huffman_offset(bytes, flitzzip::kGpuHuffmanLookupBytes + (segment + 1U) * 4U,
                           static_cast<std::uint32_t>(bytes.size() - table_end));
    }
    return bytes;
}

// Purpose: Exercise the actual CPU decoder with independent output bounds and no GPU availability dependency.
// Inputs: A valid or malformed frame, decoded size and expected acceptance. Outputs: Exact content or ArchiveError.
void require_huffman_cpu_decode(const std::vector<std::byte>& payload, std::uint32_t size, bool valid) {
    const flitzzip::BlockDescriptor block{
        .kind = flitzzip::BlockKind::GpuHuffman,
        .uncompressed_len = size,
        .encoded_len = static_cast<std::uint32_t>(payload.size()),
    };
    flitzzip::ArchiveCodecOptions options;
    options.block_size = 131072U;
    options.worker_count = 3U;
    std::vector<std::byte> guarded(size + 2U, std::byte{0xCB});
    const auto output = std::span(guarded).subspan(1U, size);
    bool rejected = false;
    try {
        flitzzip::decode_chunk_cpu(payload, std::span(&block, 1U), output, options);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_EQ(rejected, !valid);
    REQUIRE_EQ(guarded.front(), std::byte{0xCB});
    REQUIRE_EQ(guarded.back(), std::byte{0xCB});
    if (valid) {
        for (std::size_t index = 0; index < output.size(); ++index) {
            REQUIRE_EQ(output[index], static_cast<std::byte>(201U + (index & 1U)));
        }
    }
}

}  // namespace

// Purpose: Qualify CPU-only compatibility reading of GPU Huffman archives independently of hardware.
// Inputs: Hand-built one-bit frames at segment/tail boundaries. Outputs: Every decoded byte and guard must match.
TEST_CASE(huffman_cpu_reference_frames_restore_segment_tails) {
    for (const auto size : {16384U, 16385U, 65536U, 65537U}) {
        require_huffman_cpu_decode(binary_huffman_fixture(size), size, true);
    }
}

// Purpose: Detect hostile lookup entries and segment extents before allowing out-of-bounds decoding.
// Inputs: Independent width/leaf/start/monotonicity/end/truncation mutations of a valid two-symbol frame.
// Outputs: Every mutation fails with intact guards; an independent valid job still succeeds afterward.
TEST_CASE(huffman_cpu_rejects_corrupt_lookup_and_segment_boundaries) {
    constexpr std::uint32_t size = 16385U;
    const auto original = binary_huffman_fixture(size);
    constexpr auto table = flitzzip::kGpuHuffmanLookupBytes;
    for (int corruption = 0; corruption < 9; ++corruption) {
        auto invalid = original;
        switch (corruption) {
        case 0:
            invalid[1] = std::byte{0};
            break;
        case 1:
            invalid[1] = std::byte{13};
            break;
        case 2:
            invalid[0] = std::byte{202};
            break;
        case 3:
            put_huffman_offset(invalid, table, 1U);
            break;
        case 4:
            put_huffman_offset(invalid, table + 4U, 0xFFFFFFFFU);
            break;
        case 5:
            put_huffman_offset(invalid, table + 8U, 0U);
            break;
        case 6:
            put_huffman_offset(invalid, table + 4U, 1U);
            break;
        case 7:
            invalid.pop_back();
            break;
        case 8:
            invalid.push_back(std::byte{0});
            break;
        }
        require_huffman_cpu_decode(invalid, size, false);
    }
    require_huffman_cpu_decode(original, size, true);
}
