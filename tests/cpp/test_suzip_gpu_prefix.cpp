#include "core/archive.hpp"
#include "core/checksum.hpp"
#include "core/huffman_lookup.hpp"
#include "core/result.hpp"
#include "core/resource_limits.hpp"
#include "gpu/gpu_codec.hpp"
#include "test_suzip_helpers.hpp"
#include "test_util.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace {

using namespace superzip_test;

struct PrefixBlockLocation {
    std::uint64_t payload_offset;
    superzip::BlockDescriptor block;
};

// Purpose: Write deterministic low-entropy bytes that are not fill or periodic data.
// Inputs: `path` is the destination file and `byte_count` is the exact payload size.
// Outputs: Creates or replaces `path` with a biased byte distribution used by GPU-prefix tests.
void write_low_entropy_payload(const std::filesystem::path& path, std::size_t byte_count) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE_TRUE(out.is_open());
    std::uint32_t state = 0xC001D00DU;
    for (std::size_t i = 0; i < byte_count; ++i) {
        state = (state * 1664525U) + 1013904223U;
        const auto bucket = (state >> 16U) & 1023U;
        unsigned char value = 0;
        if (bucket < 180U) {
            value = 1;
        } else if (bucket < 330U) {
            value = 0;
        } else if (bucket < 450U) {
            value = 2;
        } else if (bucket < 520U) {
            value = 3;
        } else if (bucket < 720U) {
            value = static_cast<unsigned char>(4U + (bucket % 16U));
        } else if (bucket < 900U) {
            value = static_cast<unsigned char>(20U + (bucket % 64U));
        } else {
            value = static_cast<unsigned char>(84U + (bucket % 172U));
        }
        out.put(static_cast<char>(value));
    }
}

// Purpose: Write one fill block followed by one low-entropy raw block in the same chunk.
// Inputs: `path` is the destination file and `block_bytes` is the byte count for each half.
// Outputs: Creates a two-block file that must preserve fill encoding while prefix-compressing the raw half.
void write_fill_then_low_entropy_payload(const std::filesystem::path& path, std::size_t block_bytes) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE_TRUE(out.is_open());
    for (std::size_t i = 0; i < block_bytes; ++i) {
        out.put('\0');
    }
    std::uint32_t state = 0xB10C5A11U;
    for (std::size_t i = 0; i < block_bytes; ++i) {
        state = (state * 1664525U) + 1013904223U;
        const auto bucket = (state >> 16U) & 1023U;
        unsigned char value = 0;
        if (bucket < 180U) {
            value = 1;
        } else if (bucket < 330U) {
            value = 0;
        } else if (bucket < 450U) {
            value = 2;
        } else if (bucket < 520U) {
            value = 3;
        } else if (bucket < 720U) {
            value = static_cast<unsigned char>(4U + (bucket % 16U));
        } else if (bucket < 900U) {
            value = static_cast<unsigned char>(20U + (bucket % 64U));
        } else {
            value = static_cast<unsigned char>(84U + (bucket % 172U));
        }
        out.put(static_cast<char>(value));
    }
}

// Purpose: Write deterministic high-byte low-entropy data that requires adaptive GPU prefix coding to compress well.
// Inputs: `path` is the destination file and `byte_count` is the exact payload size.
// Outputs: Creates or replaces `path` with a biased distribution whose frequent bytes are not low numeric values.
void write_shifted_low_entropy_payload(const std::filesystem::path& path, std::size_t byte_count) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE_TRUE(out.is_open());
    std::uint32_t state = 0xA11CE5EEDU;
    for (std::size_t i = 0; i < byte_count; ++i) {
        state = (state * 1103515245U) + 12345U;
        const auto bucket = (state >> 16U) & 1023U;
        unsigned char value = 0;
        if (bucket < 220U) {
            value = 201U;
        } else if (bucket < 410U) {
            value = 233U;
        } else if (bucket < 560U) {
            value = 144U;
        } else if (bucket < 680U) {
            value = 177U;
        } else if (bucket < 860U) {
            value = static_cast<unsigned char>(96U + (bucket % 32U));
        } else {
            value = static_cast<unsigned char>(bucket % 256U);
        }
        out.put(static_cast<char>(value));
    }
}

// Purpose: Return the serialized offset-table byte count for one GPU-prefix test block.
// Inputs: `decoded_len` is the block's uncompressed byte count.
// Outputs: Returns the number of bytes before the encoded bitstream starts.
std::uint64_t test_gpu_prefix_table_bytes(std::uint32_t decoded_len) {
    const auto segments = (static_cast<std::uint64_t>(decoded_len) + superzip::kGpuPrefixSegmentBytes - 1U) /
                          superzip::kGpuPrefixSegmentBytes;
    return (segments + 1U) * sizeof(std::uint32_t);
}

// Purpose: Locate the first GPU-prefix block inside a test archive index.
// Inputs: `index` is a parsed SUZIP test archive index.
// Outputs: Returns the entry payload offset and block descriptor; fails the test when no prefix block exists.
PrefixBlockLocation find_first_prefix_block(const superzip::ArchiveIndex& index) {
    for (const auto& entry : index.entries) {
        for (const auto& block : entry.blocks) {
            if (block.kind == superzip::BlockKind::GpuPrefix) {
                return PrefixBlockLocation{.payload_offset = entry.payload_offset, .block = block};
            }
        }
    }
    REQUIRE_TRUE(false);
    return PrefixBlockLocation{};
}

// Purpose: Resolve one reference symbol without using the production GPU encoder.
// Inputs: Byte value and an empty static or serialized adaptive codebook.
// Outputs: Returns little-bit-order code and width for the native prefix format.
std::pair<std::uint32_t, std::uint32_t> reference_prefix_symbol(std::byte value, std::span<const std::byte> codebook) {
    const auto literal = static_cast<std::uint32_t>(value);
    const auto rank =
        codebook.empty()
            ? literal
            : static_cast<std::uint32_t>(std::find(codebook.begin(), codebook.end(), value) - codebook.begin());
    if (rank < 4U) {
        return {rank << 1U, 3U};
    }
    if (rank < 20U) {
        return {1U | ((rank - 4U) << 2U), 6U};
    }
    if (rank < 84U) {
        return {3U | ((rank - 20U) << 3U), 9U};
    }
    return {7U | ((codebook.empty() ? literal - 84U : literal) << 3U), 11U};
}

// Purpose: Encode a bounded test block using independent bit-by-bit reference logic.
// Inputs: Uncompressed input and either no codebook or the 84-byte adaptive codebook.
// Outputs: Returns the complete native prefix payload, including little-endian offsets and zero padding.
std::vector<std::byte> reference_prefix_payload(std::span<const std::byte> input, std::span<const std::byte> codebook) {
    const auto segments = (input.size() + superzip::kGpuPrefixSegmentBytes - 1U) / superzip::kGpuPrefixSegmentBytes;
    std::vector<std::byte> result(codebook.size() + (segments + 1U) * 4U);
    std::copy(codebook.begin(), codebook.end(), result.begin());
    std::uint32_t offset = 0;
    for (std::size_t segment = 0; segment <= segments; ++segment) {
        for (unsigned int byte = 0; byte < 4U; ++byte) {
            result[codebook.size() + segment * 4U + byte] = static_cast<std::byte>(offset >> (byte * 8U));
        }
        if (segment == segments) {
            break;
        }
        const auto start = segment * superzip::kGpuPrefixSegmentBytes;
        const auto bytes =
            input.subspan(start, std::min<std::size_t>(superzip::kGpuPrefixSegmentBytes, input.size() - start));
        std::vector<std::byte> stream(((bytes.size() * 11U + 31U) / 32U) * 4U);
        std::size_t position = 0;
        for (const auto value : bytes) {
            const auto [code, width] = reference_prefix_symbol(value, codebook);
            for (std::uint32_t bit = 0; bit < width; ++bit, ++position) {
                stream[position / 8U] |= static_cast<std::byte>(((code >> bit) & 1U) << (position % 8U));
            }
        }
        stream.resize(((position + 31U) / 32U) * 4U);
        offset += static_cast<std::uint32_t>(stream.size());
        result.insert(result.end(), stream.begin(), stream.end());
    }
    return result;
}

}  // namespace

// Purpose: Anchor the independent encoder to manually calculated format bytes even on CPU-only hosts.
// Inputs: The four three-bit static symbols and a four-byte-aligned segment.
// Outputs: Requires the exact offset table and 0x0D10 little-bit-order symbol stream.
TEST_CASE(suzip_prefix_reference_known_bytes) {
    const std::array input{std::byte{0}, std::byte{1}, std::byte{2}, std::byte{3}};
    const std::vector<std::byte> expected{std::byte{0},    std::byte{0},    std::byte{0}, std::byte{0},
                                          std::byte{4},    std::byte{0},    std::byte{0}, std::byte{0},
                                          std::byte{0x10}, std::byte{0x0D}, std::byte{0}, std::byte{0}};
    REQUIRE_TRUE(reference_prefix_payload(input, {}) == expected);
}

// Purpose: Reject malformed prefix grammar identically in CPU and required-HIP decode and CRC paths.
// Inputs: One static/adaptive block with a truncated bitstream or an invalid static high-byte code.
// Outputs: Requires every decode and CRC route to throw ArchiveError before accepting the block.
TEST_CASE(suzip_gpu_prefix_malformed_codewords_are_rejected) {
    const bool gpu_available = superzip::query_gpu_info().available;
    constexpr std::uint32_t decoded_len = 128U;
    for (const auto [kind, invalid_high] : {
             std::pair{superzip::BlockKind::GpuPrefix, false},
             std::pair{superzip::BlockKind::GpuPrefix, true},
             std::pair{superzip::BlockKind::GpuAdaptivePrefix, false},
         }) {
        const auto codebook_bytes =
            kind == superzip::BlockKind::GpuAdaptivePrefix ? superzip::kGpuAdaptivePrefixCodebookBytes : 0U;
        std::vector<std::byte> payload(codebook_bytes + 8U + 4U);
        payload[codebook_bytes + 4U] = std::byte{4};
        if (invalid_high) {
            std::fill(payload.end() - 4, payload.end(), std::byte{0xFF});
        }
        const superzip::BlockDescriptor block{
            .kind = kind,
            .uncompressed_len = decoded_len,
            .encoded_offset = 0U,
            .encoded_len = static_cast<std::uint32_t>(payload.size()),
        };
        std::vector<std::byte> output(decoded_len);
        for (const bool force_cpu : {true, false}) {
            if (!force_cpu && !gpu_available) {
                continue;
            }
            superzip::GpuCodecOptions options;
            options.require_gpu = !force_cpu;
            options.force_cpu = force_cpu;
            bool decode_rejected = false;
            try {
                (void)superzip::decode_chunk(payload, std::span(&block, 1), output, options);
            } catch (const superzip::ArchiveError&) {
                decode_rejected = true;
            }
            REQUIRE_TRUE(decode_rejected);

            bool crc_rejected = false;
            try {
                (void)superzip::crc_decoded_chunk(payload, std::span(&block, 1), decoded_len, options);
            } catch (const superzip::ArchiveError&) {
                crc_rejected = true;
            }
            REQUIRE_TRUE(crc_rejected);
        }
    }
}

// Purpose: Prove exact HIP packing against independent reference bytes across symbols and word boundaries.
// Inputs: RAM-only fixtures with every static symbol at all 32 bit alignments, shifted alphabets, and partial tails.
// Outputs: Requires exact payloads, including offset tables and padding, plus CPU/HIP byte-exact decoding.
TEST_CASE(suzip_gpu_prefix_packing_matches_reference) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> fixture;
    std::uint32_t bit_position = 0;
    std::uint32_t filler_state = 0x6A47D321U;
    for (std::uint32_t value = 0; value < 256U; ++value) {
        for (std::uint32_t alignment = 0; alignment < 32U; ++alignment) {
            while (bit_position % 32U != alignment) {
                filler_state ^= filler_state << 13U;
                filler_state ^= filler_state >> 17U;
                filler_state ^= filler_state << 5U;
                fixture.push_back(static_cast<std::byte>(filler_state & 3U));
                bit_position += 3U;
                if (fixture.size() % superzip::kGpuPrefixSegmentBytes == 0U) {
                    bit_position = 0;
                }
            }
            fixture.push_back(static_cast<std::byte>(value));
            bit_position += reference_prefix_symbol(static_cast<std::byte>(value), {}).second;
            if (fixture.size() % superzip::kGpuPrefixSegmentBytes == 0U) {
                bit_position = 0;
            }
        }
    }
    const auto aligned_size = ((fixture.size() + 4095U) / 4096U) * 4096U;
    for (const int level : {1, 2}) {
        for (const std::size_t tail : {0U, 1U, 15U, 16U, 17U, 31U, 32U, 4095U}) {
            auto input = fixture;
            while (input.size() < aligned_size + tail) {
                filler_state ^= filler_state << 13U;
                filler_state ^= filler_state >> 17U;
                filler_state ^= filler_state << 5U;
                input.push_back(static_cast<std::byte>(filler_state & 3U));
            }
            if (level == 2) {
                for (auto& byte : input) {
                    byte = static_cast<std::byte>((static_cast<std::uint32_t>(byte) + 201U) & 255U);
                }
            }
            superzip::GpuCodecOptions options;
            options.block_size = 256U * 1024U;
            options.compression_level = level;
            const auto encoded = superzip::encode_chunk(input, options);
            REQUIRE_TRUE(encoded.gpu_used);
            REQUIRE_EQ(encoded.blocks.size(), 1U);
            REQUIRE_EQ(encoded.blocks.front().kind,
                       level == 1 ? superzip::BlockKind::GpuPrefix : superzip::BlockKind::GpuAdaptivePrefix);
            REQUIRE_TRUE(encoded.payload.size() >= (level == 2 ? superzip::kGpuAdaptivePrefixCodebookBytes : 0U));
            const auto codebook = std::span<const std::byte>(encoded.payload)
                                      .first(level == 2 ? superzip::kGpuAdaptivePrefixCodebookBytes : 0U);
            REQUIRE_TRUE(encoded.payload == reference_prefix_payload(input, codebook));
            std::vector<std::byte> decoded(input.size());
            REQUIRE_TRUE(superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
            REQUIRE_TRUE(decoded == input);
            options.require_gpu = false;
            options.force_cpu = true;
            REQUIRE_TRUE(!superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
            REQUIRE_TRUE(decoded == input);
        }
    }
}

// Purpose: Keep the smallest native representation independently for each block in a heterogeneous chunk.
// Inputs: RAM-only low/high-byte entropy, fill, pattern, and short raw regions at strong compression levels.
// Outputs: Requires mixed static/adaptive selection, dense payloads, and exact CPU/HIP restoration and CRC.
TEST_CASE(suzip_gpu_prefix_candidate_selection_is_per_block) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    for (const std::uint32_t block_size : {256U * 1024U, 1024U * 1024U}) {
        std::vector<std::byte> input(static_cast<std::size_t>(block_size) * 4U + 257U);
        std::uint32_t random = 0xC001D00DU;
        for (std::size_t i = 0; i < input.size(); ++i) {
            random = random * 1664525U + 1013904223U;
            const auto region = i / block_size;
            const auto symbol = (random >> 16U) & 3U;
            input[i] = static_cast<std::byte>(region == 0U   ? symbol
                                              : region == 1U ? symbol + 201U
                                              : region == 2U ? 0xAAU
                                              : region == 3U ? (i % 3U) + 30U
                                                             : random >> 16U);
        }
        for (const int level : {2}) {
            superzip::GpuCodecOptions options;
            options.require_gpu = true;
            options.compression_level = level;
            options.block_size = block_size;
            const auto encoded = superzip::encode_chunk(input, options);
            REQUIRE_EQ(encoded.blocks.size(), 5U);
            REQUIRE_EQ(encoded.blocks[0].kind, superzip::BlockKind::GpuHuffman);
            REQUIRE_EQ(encoded.blocks[1].kind, superzip::BlockKind::GpuAdaptivePrefix);
            REQUIRE_EQ(encoded.blocks[2].kind, superzip::BlockKind::Fill);
            REQUIRE_EQ(encoded.blocks[3].kind, superzip::BlockKind::Pattern);
            REQUIRE_EQ(encoded.blocks[4].kind, superzip::BlockKind::Raw);
            std::size_t offset = 0;
            for (std::size_t i = 0; i < encoded.blocks.size(); ++i) {
                const auto& block = encoded.blocks[i];
                const auto individual = superzip::encode_chunk(
                    std::span<const std::byte>(input).subspan(i * block_size, block.uncompressed_len), options);
                REQUIRE_EQ(block.encoded_offset, offset);
                REQUIRE_EQ(block.encoded_len, individual.payload.size());
                REQUIRE_TRUE(offset <= encoded.payload.size());
                REQUIRE_TRUE(individual.payload.size() <= encoded.payload.size() - offset);
                REQUIRE_TRUE(std::equal(individual.payload.begin(), individual.payload.end(),
                                        encoded.payload.begin() + static_cast<std::ptrdiff_t>(offset)));
                offset += block.encoded_len;
            }
            REQUIRE_EQ(offset, encoded.payload.size());
            std::vector<std::byte> decoded(input.size());
            REQUIRE_TRUE(superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
            REQUIRE_TRUE(decoded == input);
            const auto crc = superzip::crc_decoded_chunk(encoded.payload, encoded.blocks, input.size(), options);
            REQUIRE_TRUE(crc.gpu_used);
            REQUIRE_EQ(crc.crc32, superzip::crc32(input));
            options.require_gpu = false;
            options.force_cpu = true;
            std::fill(decoded.begin(), decoded.end(), std::byte{0xBB});
            REQUIRE_TRUE(!superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
            REQUIRE_TRUE(decoded == input);
        }
    }
}

// Purpose: Replace a static GPU prefix only when the first stronger effort measures a size gain.
// Inputs: One deterministic low-byte RAM-only block encoded at levels one and two with independent telemetry.
// Outputs: Level two emits a smaller Huffman block and still decodes byte-for-byte on the GPU.
TEST_CASE(suzip_gpu_huffman_replaces_static_prefix_when_smaller) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> input(1024U * 1024U);
    std::uint32_t random = 0xC001D00DU;
    for (auto& byte : input) {
        random = random * 1664525U + 1013904223U;
        byte = static_cast<std::byte>((random >> 16U) & 3U);
    }
    superzip::GpuCodecOptions options;
    options.require_gpu = true;
    options.compression_level = 1;
    options.telemetry = std::make_shared<superzip::GpuTelemetry>();
    const auto balanced = superzip::encode_chunk(input, options);
    const auto balanced_stats = superzip::snapshot_gpu_telemetry(*options.telemetry);
    options.compression_level = 2;
    options.telemetry = std::make_shared<superzip::GpuTelemetry>();
    const auto maximum = superzip::encode_chunk(input, options);
    const auto maximum_stats = superzip::snapshot_gpu_telemetry(*options.telemetry);
    REQUIRE_TRUE(maximum.payload.size() < balanced.payload.size());
    REQUIRE_EQ(maximum.blocks.size(), 1U);
    REQUIRE_EQ(maximum.blocks[0].kind, superzip::BlockKind::GpuHuffman);
    REQUIRE_TRUE(maximum_stats.kernel_launches > balanced_stats.kernel_launches);
    std::vector<std::byte> decoded(input.size());
    REQUIRE_TRUE(superzip::decode_chunk(maximum.payload, maximum.blocks, decoded, options));
    REQUIRE_TRUE(decoded == input);
}

// Purpose: Demonstrate stronger native compression on data that the static low-byte code cannot compact.
// Inputs: A deterministic 1 MiB high-byte alphabet encoded at all nine efforts, entirely in RAM.
// Outputs: Reports exact sizes, requires a full-sample Huffman gain, and checks CPU/HIP roundtrips.
TEST_CASE(suzip_gpu_prefix_levels_compact_shifted_alphabet) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> input(1024U * 1024U);
    std::uint32_t random = 0xC001D00DU;
    for (auto& byte : input) {
        random = random * 1664525U + 1013904223U;
        byte = static_cast<std::byte>(201U + ((random >> 16U) & 3U));
    }
    std::size_t previous_bytes = input.size();
    std::array<std::size_t, 9> encoded_sizes{};
    std::size_t level_index = 0U;
    for (const int level : std::array{1, 2, 3, 4, 5, 6, 7, 8, 9}) {
        superzip::GpuCodecOptions options;
        options.require_gpu = true;
        options.compression_level = level;
        const auto encoded = superzip::encode_chunk(input, options);
        REQUIRE_TRUE(encoded.gpu_used);
        REQUIRE_TRUE(encoded.payload.size() <= previous_bytes);
        previous_bytes = encoded.payload.size();
        encoded_sizes[level_index++] = encoded.payload.size();
        std::vector<std::byte> decoded(input.size());
        REQUIRE_TRUE(superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
        REQUIRE_TRUE(decoded == input);
        options.require_gpu = false;
        options.force_cpu = true;
        std::fill(decoded.begin(), decoded.end(), std::byte{0xBB});
        REQUIRE_TRUE(!superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
        REQUIRE_TRUE(decoded == input);
        std::cout << "prefix_level_case level=" << level << " input_bytes=" << input.size()
                  << " output_bytes=" << encoded.payload.size() << " memory_only=true disk_write_bytes=0\n";
    }
    REQUIRE_TRUE(std::all_of(encoded_sizes.begin() + 1, encoded_sizes.end(),
                             [&](const std::size_t bytes) { return bytes < input.size() / 2U; }));
    REQUIRE_TRUE(encoded_sizes.back() < encoded_sizes[2]);
    REQUIRE_TRUE(encoded_sizes.back() < 280000U);
}

// Purpose: Preserve each block's lower-effort entropy winner across mixed alphabets and an uneven raw tail.
// Inputs: A deterministic non-periodic three-block RAM workload, efforts 1-9, and the available HIP device.
// Outputs: Requires non-growing block/payload sizes, exact CPU/HIP decoding, and real kernel telemetry at every effort.
TEST_CASE(suzip_gpu_entropy_efforts_preserve_per_block_winners) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    constexpr std::uint32_t block_bytes = 1024U * 1024U;
    std::vector<std::byte> input(3U * block_bytes + 123U);
    std::uint32_t random = 0xBA5ECA5EU;
    for (std::size_t index = 0U; index < input.size(); ++index) {
        random = random * 1664525U + 1013904223U;
        const auto bucket = (random >> 16U) & 255U;
        const auto value = index < block_bytes        ? bucket & 3U
                           : index < 2U * block_bytes ? 201U + (bucket & 3U)
                           : index < 3U * block_bytes
                               ? (bucket < 160U ? 201U + (index / 16384U) % 3U : 205U + (bucket & 15U))
                               : bucket;
        input[index] = static_cast<std::byte>(value);
    }
    std::array<std::uint32_t, 4> previous_blocks{};
    previous_blocks.fill(block_bytes);
    previous_blocks.back() = 123U;
    auto previous_bytes = input.size();
    for (const int effort : {1, 2, 3, 4, 5, 6, 7, 8, 9}) {
        superzip::GpuCodecOptions options;
        options.require_gpu = true;
        options.block_size = block_bytes;
        options.compression_level = effort;
        options.telemetry = std::make_shared<superzip::GpuTelemetry>();
        const auto encoded = superzip::encode_chunk(input, options);
        REQUIRE_TRUE(encoded.gpu_used);
        REQUIRE_EQ(encoded.blocks.size(), previous_blocks.size());
        REQUIRE_TRUE(encoded.payload.size() <= previous_bytes);
        previous_bytes = encoded.payload.size();
        for (std::size_t block = 0U; block < encoded.blocks.size(); ++block) {
            REQUIRE_TRUE(encoded.blocks[block].encoded_len <= previous_blocks[block]);
            previous_blocks[block] = encoded.blocks[block].encoded_len;
        }
        std::vector<std::byte> decoded(input.size());
        REQUIRE_TRUE(superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
        REQUIRE_TRUE(decoded == input);
        REQUIRE_TRUE(superzip::snapshot_gpu_telemetry(*options.telemetry).kernel_launches > 0U);
        options.require_gpu = false;
        options.force_cpu = true;
        std::fill(decoded.begin(), decoded.end(), std::byte{0xBD});
        REQUIRE_TRUE(!superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
        REQUIRE_TRUE(decoded == input);
        std::cout << "nested_entropy_case level=" << effort << " input_bytes=" << input.size()
                  << " output_bytes=" << encoded.payload.size() << " memory_only=true disk_write_bytes=0\n";
    }
}

// Purpose: Accept complete sparse-alphabet Huffman lookups while rejecting uncovered or conflicting slots.
// Inputs: A two-symbol 4096-entry lookup with individual malformed-entry mutations.
// Outputs: Validates the version-eight trust boundary independently of GPU availability.
TEST_CASE(suzip_gpu_huffman_sparse_lookup_validation) {
    std::vector<std::byte> lookup(superzip::kGpuHuffmanLookupBytes);
    for (std::size_t slot = 0U; slot < superzip::kGpuHuffmanLookupEntries; ++slot) {
        lookup[slot * 2U] = static_cast<std::byte>(201U + (slot & 1U));
        lookup[slot * 2U + 1U] = std::byte{1};
    }
    REQUIRE_TRUE(superzip::huffman_lookup_is_complete(lookup));
    lookup[1] = std::byte{0};
    REQUIRE_TRUE(!superzip::huffman_lookup_is_complete(lookup));
    lookup[1] = std::byte{1};
    lookup[0] = std::byte{202};
    REQUIRE_TRUE(!superzip::huffman_lookup_is_complete(lookup));
}

// Purpose: Reject malformed version-eight Huffman lookup and segment metadata in both decoders.
// Inputs: Fully populated and sparse-alphabet Huffman blocks with invalid widths, leaves, or offsets.
// Outputs: CPU and GPU decode reject each mutation rather than publishing a partial result.
TEST_CASE(suzip_gpu_huffman_corruption_is_rejected) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> input(1024U * 1024U);
    std::uint32_t state = 0xC001D00DU;
    for (auto& byte : input) {
        state = state * 1664525U + 1013904223U;
        byte = static_cast<std::byte>(201U + ((state >> 16U) & 3U));
    }
    superzip::GpuCodecOptions options;
    options.require_gpu = true;
    for (const int level : {5, 9}) {
        options.compression_level = level;
        const auto encoded = superzip::encode_chunk(input, options);
        REQUIRE_EQ(encoded.blocks.size(), 1U);
        REQUIRE_EQ(encoded.blocks[0].kind, superzip::BlockKind::GpuHuffman);
        for (const int corruption : {0, 1, 2}) {
            auto damaged = encoded.payload;
            if (corruption == 0) {
                for (std::size_t index = 1U; index < superzip::kGpuHuffmanLookupBytes; index += 2U) {
                    damaged[index] = std::byte{0};
                }
            } else if (corruption == 1) {
                const auto segment_count =
                    (input.size() + superzip::kGpuPrefixSegmentBytes - 1U) / superzip::kGpuPrefixSegmentBytes;
                const auto final_offset = superzip::kGpuHuffmanLookupBytes + segment_count * sizeof(std::uint32_t);
                std::fill_n(damaged.begin() + static_cast<std::ptrdiff_t>(final_offset), 4U, std::byte{0xFF});
            } else {
                damaged[0] ^= std::byte{0x01};
            }
            for (const bool force_cpu : {true, false}) {
                options.force_cpu = force_cpu;
                options.require_gpu = !force_cpu;
                std::vector<std::byte> output(input.size());
                bool rejected = false;
                try {
                    (void)superzip::decode_chunk(damaged, encoded.blocks, output, options);
                } catch (const superzip::ArchiveError&) {
                    rejected = true;
                }
                REQUIRE_TRUE(rejected);
            }
        }
        options.force_cpu = false;
        options.require_gpu = true;
    }
}

// Purpose: Verify HIP prefix decoding covers single segments, uneven segment counts, and partial final segments.
// Inputs: RAM-only deterministic static/adaptive inputs with one through 129 segments.
// Outputs: Requires byte-identical CPU/HIP decoding and matching GPU CRC without changing encoded format semantics.
TEST_CASE(suzip_gpu_prefix_segment_boundaries_roundtrip) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    constexpr std::array<std::size_t, 5> segment_counts{1U, 31U, 127U, 128U, 129U};
    for (const auto segments : segment_counts) {
        for (const int level : {1, 2}) {
            const auto tail_trim = segments == 1U ? 0U : 13U;
            std::vector<std::byte> input(segments * superzip::kGpuPrefixSegmentBytes - tail_trim);
            std::uint32_t random = 0xC001D00DU;
            for (auto& byte : input) {
                random = random * 1664525U + 1013904223U;
                const auto symbol = (random >> 16U) & 3U;
                byte = static_cast<std::byte>(level == 2 ? symbol + 201U : symbol);
            }
            superzip::GpuCodecOptions options;
            options.require_gpu = true;
            options.compression_level = level;
            options.block_size = 1024U * 1024U;
            const auto encoded = superzip::encode_chunk(input, options);
            const auto expected_kind =
                level == 2 ? superzip::BlockKind::GpuAdaptivePrefix : superzip::BlockKind::GpuPrefix;
            REQUIRE_TRUE(std::ranges::any_of(
                encoded.blocks, [expected_kind](const auto& block) { return block.kind == expected_kind; }));
            std::vector<std::byte> decoded(input.size(), std::byte{0xAA});
            REQUIRE_TRUE(superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
            REQUIRE_TRUE(decoded == input);
            const auto crc = superzip::crc_decoded_chunk(encoded.payload, encoded.blocks, input.size(), options);
            REQUIRE_TRUE(crc.gpu_used);
            REQUIRE_EQ(crc.crc32, superzip::crc32(input));
            options.require_gpu = false;
            options.force_cpu = true;
            std::fill(decoded.begin(), decoded.end(), std::byte{0xBB});
            REQUIRE_TRUE(!superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
            REQUIRE_TRUE(decoded == input);
        }
    }
}

// Purpose: Verify required-HIP compression can emit a real GPU prefix-compressed native block.
// Inputs: A low-byte payload that is not fill or periodic and is compressed with `gpu_required`.
// Outputs: Throws if the archive stays raw-sized, uses CPU deflate, omits prefix blocks, or fails required-HIP
// verification/extraction.
TEST_CASE(suzip_required_gpu_prefix_blocks_compress_low_entropy_payload) {
    if (!superzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("suzip-required-gpu-prefix");
    const auto input = root / "low-entropy.bin";
    write_low_entropy_payload(input, 2U * 1024U * 1024U);
    const auto source_size = std::filesystem::file_size(input);
    const auto archive = root / "archive.suzip";

    superzip::CompressOptions compress;
    compress.gpu_required = true;
    compress.force_cpu = false;
    compress.chunk_size = 2U * 1024U * 1024U;
    compress.block_size = 1024U * 1024U;
    compress.compression_level = 1;
    compress.verify_after_write = true;
    const auto compressed = superzip::compress_suzip({input}, archive, compress);
    REQUIRE_TRUE(compressed.gpu_used);
    REQUIRE_TRUE(compressed.gpu_runtime.prefix_blocks > 0U);
    REQUIRE_TRUE(compressed.output_bytes < (source_size * 3U) / 4U);

    const auto index = read_test_archive_index(archive);
    REQUIRE_TRUE(archive_contains_block_kind(index, superzip::BlockKind::GpuPrefix));
    REQUIRE_TRUE(!archive_contains_block_kind(index, superzip::BlockKind::Deflate));

    superzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 2U * 1024U * 1024U;
    verify.block_size = 1024U * 1024U;
    const auto verified = superzip::verify_suzip(archive, verify);
    REQUIRE_TRUE(verified.gpu_used);

    const auto output = root / "out";
    verify.overwrite = true;
    const auto extracted = superzip::extract_suzip(archive, output, verify);
    REQUIRE_TRUE(extracted.gpu_used);
    REQUIRE_EQ(std::filesystem::file_size(output / "low-entropy.bin"), source_size);
    REQUIRE_TRUE(files_are_equal(output / "low-entropy.bin", input));
    std::filesystem::remove_all(root);
}

// Purpose: Verify raw blocks inside a mixed fill/raw chunk still receive GPU prefix compression.
// Inputs: A two-block payload where the first block is fill-compressed and the second is low-entropy raw data.
// Outputs: Throws if the raw block stays uncompressed only because the same chunk also contains a fill block.
TEST_CASE(suzip_required_gpu_prefix_blocks_compress_raw_blocks_inside_mixed_chunk) {
    if (!superzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("suzip-required-gpu-mixed-prefix");
    const auto input = root / "mixed-fill-low-entropy.bin";
    write_fill_then_low_entropy_payload(input, 1024U * 1024U);
    const auto source_size = std::filesystem::file_size(input);
    const auto archive = root / "archive.suzip";

    superzip::CompressOptions compress;
    compress.gpu_required = true;
    compress.force_cpu = false;
    compress.chunk_size = 2U * 1024U * 1024U;
    compress.block_size = 1024U * 1024U;
    compress.compression_level = 1;
    compress.verify_after_write = true;
    const auto compressed = superzip::compress_suzip({input}, archive, compress);
    REQUIRE_TRUE(compressed.gpu_used);
    REQUIRE_TRUE(compressed.gpu_runtime.prefix_blocks > 0U);
    REQUIRE_TRUE(compressed.output_bytes < source_size);

    const auto index = read_test_archive_index(archive);
    REQUIRE_TRUE(archive_contains_block_kind(index, superzip::BlockKind::Fill));
    REQUIRE_TRUE(archive_contains_block_kind(index, superzip::BlockKind::GpuPrefix));
    REQUIRE_TRUE(!archive_contains_block_kind(index, superzip::BlockKind::Deflate));

    superzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 2U * 1024U * 1024U;
    verify.block_size = 1024U * 1024U;
    const auto verified = superzip::verify_suzip(archive, verify);
    REQUIRE_TRUE(verified.gpu_used);

    const auto output = root / "out";
    verify.overwrite = true;
    const auto extracted = superzip::extract_suzip(archive, output, verify);
    REQUIRE_TRUE(extracted.gpu_used);
    REQUIRE_EQ(std::filesystem::file_size(output / "mixed-fill-low-entropy.bin"), source_size);
    REQUIRE_TRUE(files_are_equal(output / "mixed-fill-low-entropy.bin", input));
    std::filesystem::remove_all(root);
}

// Purpose: Verify Balanced and Maximum HIP levels can emit GPU Huffman blocks without CPU deflate.
// Inputs: A high-byte low-entropy payload where static low-value prefix coding is intentionally weak.
// Outputs: Throws if either level fails to beat level 1, omits Huffman blocks, emits deflate, or fails read-back.
TEST_CASE(suzip_required_gpu_huffman_blocks_honor_compression_level) {
    if (!superzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("suzip-required-gpu-adaptive-prefix");
    const auto input = root / "shifted-low-entropy.bin";
    write_shifted_low_entropy_payload(input, 2U * 1024U * 1024U);
    const auto source_size = std::filesystem::file_size(input);
    const auto fast_archive = root / "fast.suzip";
    const auto balanced_archive = root / "balanced.suzip";
    const auto strong_archive = root / "strong.suzip";

    superzip::CompressOptions fast;
    fast.gpu_required = true;
    fast.force_cpu = false;
    fast.chunk_size = 2U * 1024U * 1024U;
    fast.block_size = 1024U * 1024U;
    fast.compression_level = 1;
    fast.verify_after_write = true;
    const auto fast_stats = superzip::compress_suzip({input}, fast_archive, fast);
    REQUIRE_TRUE(fast_stats.gpu_used);

    auto balanced = fast;
    balanced.compression_level = 5;
    const auto balanced_stats = superzip::compress_suzip({input}, balanced_archive, balanced);
    REQUIRE_TRUE(balanced_stats.gpu_used);
    REQUIRE_TRUE(balanced_stats.gpu_runtime.prefix_blocks > 0U);
    REQUIRE_TRUE(balanced_stats.output_bytes < fast_stats.output_bytes);
    const auto balanced_index = read_test_archive_index(balanced_archive);
    REQUIRE_TRUE(archive_contains_block_kind(balanced_index, superzip::BlockKind::GpuHuffman));
    REQUIRE_EQ(balanced_index.version, 8U);
    REQUIRE_TRUE(!archive_contains_block_kind(balanced_index, superzip::BlockKind::Deflate));
    superzip::ExtractOptions cpu_verify;
    cpu_verify.gpu_required = false;
    cpu_verify.force_cpu = true;
    REQUIRE_TRUE(!superzip::verify_suzip(balanced_archive, cpu_verify).gpu_used);

    auto strong = fast;
    strong.compression_level = 9;
    const auto strong_stats = superzip::compress_suzip({input}, strong_archive, strong);
    REQUIRE_TRUE(strong_stats.gpu_used);
    REQUIRE_TRUE(strong_stats.gpu_runtime.prefix_blocks > 0U);
    REQUIRE_TRUE(strong_stats.output_bytes < fast_stats.output_bytes);
    REQUIRE_TRUE(strong_stats.output_bytes < (source_size * 3U) / 4U);

    const auto index = read_test_archive_index(strong_archive);
    REQUIRE_TRUE(archive_contains_block_kind(index, superzip::BlockKind::GpuHuffman));
    REQUIRE_TRUE(!archive_contains_block_kind(index, superzip::BlockKind::Deflate));

    superzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 2U * 1024U * 1024U;
    verify.block_size = 1024U * 1024U;
    const auto verified = superzip::verify_suzip(strong_archive, verify);
    REQUIRE_TRUE(verified.gpu_used);

    const auto output = root / "out";
    verify.overwrite = true;
    const auto extracted = superzip::extract_suzip(strong_archive, output, verify);
    REQUIRE_TRUE(extracted.gpu_used);
    REQUIRE_EQ(std::filesystem::file_size(output / "shifted-low-entropy.bin"), source_size);
    REQUIRE_TRUE(files_are_equal(output / "shifted-low-entropy.bin", input));
    std::filesystem::remove_all(root);
}

// Purpose: Verify required-HIP verification rejects corrupted GPU-prefix payload bytes.
// Inputs: A valid required-HIP prefix archive with one encoded bitstream byte flipped after compression.
// Outputs: Throws if verification silently accepts the corrupted native GPU-prefix payload.
TEST_CASE(suzip_required_gpu_prefix_payload_corruption_is_rejected) {
    if (!superzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("suzip-required-gpu-prefix-corrupt");
    const auto input = root / "low-entropy.bin";
    write_low_entropy_payload(input, 1024U * 1024U);
    const auto archive = root / "archive.suzip";

    superzip::CompressOptions compress;
    compress.gpu_required = true;
    compress.force_cpu = false;
    compress.chunk_size = 1024U * 1024U;
    compress.block_size = 1024U * 1024U;
    compress.compression_level = 1;
    const auto compressed = superzip::compress_suzip({input}, archive, compress);
    REQUIRE_TRUE(compressed.gpu_used);
    REQUIRE_TRUE(compressed.gpu_runtime.prefix_blocks > 0U);

    const auto prefix = find_first_prefix_block(read_test_archive_index(archive));
    const auto table_bytes = test_gpu_prefix_table_bytes(prefix.block.uncompressed_len);
    REQUIRE_TRUE(prefix.block.encoded_len > table_bytes);
    xor_archive_byte(archive, prefix.payload_offset + prefix.block.encoded_offset + table_bytes, 0x01U);

    superzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 1024U * 1024U;
    verify.block_size = 1024U * 1024U;
    bool rejected = false;
    try {
        (void)superzip::verify_suzip(archive, verify);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify required-HIP verification rejects corrupted GPU-prefix table offsets before GPU materialization.
// Inputs: A valid required-HIP prefix archive with its final prefix table offset corrupted after compression.
// Outputs: Throws if verification reaches a GPU read using malformed table-controlled offsets.
TEST_CASE(suzip_required_gpu_prefix_table_corruption_is_rejected) {
    if (!superzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("suzip-required-gpu-prefix-table-corrupt");
    const auto input = root / "low-entropy.bin";
    write_low_entropy_payload(input, 1024U * 1024U);
    const auto archive = root / "archive.suzip";

    superzip::CompressOptions compress;
    compress.gpu_required = true;
    compress.force_cpu = false;
    compress.chunk_size = 1024U * 1024U;
    compress.block_size = 1024U * 1024U;
    compress.compression_level = 1;
    const auto compressed = superzip::compress_suzip({input}, archive, compress);
    REQUIRE_TRUE(compressed.gpu_used);
    REQUIRE_TRUE(compressed.gpu_runtime.prefix_blocks > 0U);

    const auto prefix = find_first_prefix_block(read_test_archive_index(archive));
    const auto table_bytes = test_gpu_prefix_table_bytes(prefix.block.uncompressed_len);
    REQUIRE_TRUE(prefix.block.encoded_len > table_bytes);
    xor_archive_byte(archive, prefix.payload_offset + prefix.block.encoded_offset + table_bytes - 1U, 0x80U);

    superzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 1024U * 1024U;
    verify.block_size = 1024U * 1024U;
    bool rejected = false;
    try {
        (void)superzip::verify_suzip(archive, verify);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Measure required-HIP pattern materialization across short, non-power-of-two, and long periods.
// Inputs: Opt-in environment flag and three deterministic 16 MiB in-memory pattern blocks.
// Outputs: Prints median host-wall decode times only after exact-byte validation; has no pass/fail timing threshold.
TEST_CASE(suzip_gpu_pattern_decode_benchmark_opt_in) {
    const auto* enabled = std::getenv("SUPERZIP_PATTERN_GPU_BENCHMARK");
    if (enabled == nullptr || std::string_view(enabled) != "1" || !superzip::query_gpu_info().available) {
        return;
    }
    constexpr std::size_t output_bytes = superzip::kMaxArchiveBlockBytes;
    superzip::GpuCodecOptions options;
    options.require_gpu = true;
    options.force_cpu = false;
    for (const std::uint32_t period : {3U, 257U, 16384U}) {
        std::vector<std::byte> pattern(period);
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            pattern[i] = static_cast<std::byte>((i * 73U + i / 11U) & 255U);
        }
        const superzip::BlockDescriptor block{
            .kind = superzip::BlockKind::Pattern,
            .uncompressed_len = static_cast<std::uint32_t>(output_bytes),
            .encoded_offset = 0U,
            .encoded_len = period,
        };
        std::vector<std::byte> decoded(output_bytes);
        std::array<double, 5> samples{};
        for (auto& milliseconds : samples) {
            const auto start = std::chrono::steady_clock::now();
            REQUIRE_TRUE(superzip::decode_chunk(pattern, std::span(&block, 1), decoded, options));
            milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            for (std::size_t i = 0; i < decoded.size(); ++i) {
                REQUIRE_EQ(decoded[i], pattern[i % pattern.size()]);
            }
        }
        std::sort(samples.begin(), samples.end());
        std::cout << "suzip_gpu_pattern_decode period=" << period << " output_bytes=" << output_bytes
                  << " median_ms=" << samples[samples.size() / 2U] << '\n';
    }
}

// Purpose: Cover GPU pattern phases at 64 KiB segment and independently encoded block boundaries.
// Inputs: Four consecutive non-aligned pattern blocks with short, odd, and maximum periods.
// Outputs: Requires byte-identical required-HIP and forced-CPU materialization for every decoded position.
TEST_CASE(suzip_gpu_pattern_decode_unaligned_boundaries) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    const std::array<std::uint32_t, 4> periods{2U, 3U, 257U, 16384U};
    const std::array<std::uint32_t, 4> lengths{70013U, 131071U, 65539U, 200003U};
    std::vector<std::byte> payload;
    std::vector<std::byte> expected;
    std::vector<superzip::BlockDescriptor> blocks;
    for (std::size_t index = 0; index < periods.size(); ++index) {
        const auto period = periods[index];
        const auto length = lengths[index];
        const auto encoded_offset = payload.size();
        for (std::uint32_t i = 0; i < period; ++i) {
            payload.push_back(static_cast<std::byte>((i * 73U + i / 11U + index * 19U) & 255U));
        }
        for (std::uint32_t i = 0; i < length; ++i) {
            expected.push_back(payload[encoded_offset + i % period]);
        }
        blocks.push_back(superzip::BlockDescriptor{
            .kind = superzip::BlockKind::Pattern,
            .uncompressed_len = length,
            .encoded_offset = encoded_offset,
            .encoded_len = period,
        });
    }
    std::vector<std::byte> decoded(expected.size());
    superzip::GpuCodecOptions options;
    REQUIRE_TRUE(superzip::decode_chunk(payload, blocks, decoded, options));
    REQUIRE_TRUE(decoded == expected);
    options.require_gpu = false;
    options.force_cpu = true;
    std::fill(decoded.begin(), decoded.end(), std::byte{0});
    REQUIRE_TRUE(!superzip::decode_chunk(payload, blocks, decoded, options));
    REQUIRE_TRUE(decoded == expected);
}
