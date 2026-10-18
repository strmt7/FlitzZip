#include "core/archive.hpp"
#include "core/checksum.hpp"
#include "core/huffman_lookup.hpp"
#include "core/result.hpp"
#include "core/resource_limits.hpp"
#include "gpu/gpu_codec.hpp"
#include "gpu/entropy_sampling.hpp"
#include "test_flzip_helpers.hpp"
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

using namespace flitzzip_test;

struct PrefixBlockLocation {
    std::uint64_t payload_offset;
    flitzzip::BlockDescriptor block;
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
    const auto segments = (static_cast<std::uint64_t>(decoded_len) + flitzzip::kGpuPrefixSegmentBytes - 1U) /
                          flitzzip::kGpuPrefixSegmentBytes;
    return (segments + 1U) * sizeof(std::uint32_t);
}

// Purpose: Locate the first GPU-prefix block inside a test archive index.
// Inputs: `index` is a parsed FLZIP test archive index.
// Outputs: Returns the entry payload offset and block descriptor; fails the test when no prefix block exists.
PrefixBlockLocation find_first_prefix_block(const flitzzip::ArchiveIndex& index) {
    for (const auto& entry : index.entries) {
        for (const auto& block : entry.blocks) {
            if (block.kind == flitzzip::BlockKind::GpuPrefix) {
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
    const auto segments = (input.size() + flitzzip::kGpuPrefixSegmentBytes - 1U) / flitzzip::kGpuPrefixSegmentBytes;
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
        const auto start = segment * flitzzip::kGpuPrefixSegmentBytes;
        const auto bytes =
            input.subspan(start, std::min<std::size_t>(flitzzip::kGpuPrefixSegmentBytes, input.size() - start));
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

// Purpose: Prevent premature full sampling and rounded-stride aliases at every supported block size.
// Inputs: All production block sizes, uneven tails, and entropy efforts 2-9; no GPU is required.
// Outputs: Requires strictly growing exact budgets for normal blocks, with complete sampling only at nine.
TEST_CASE(flzip_entropy_sampling_budgets_are_distinct_and_bounded) {
    for (const auto bytes : {4096U, 65537U, 262144U, 524288U, 1048576U, 2097152U, 4194304U, 8388608U, 16777216U}) {
        std::size_t previous = 0U;
        for (int effort = 2; effort <= 9; ++effort) {
            const auto count = flitzzip::entropy_sample_count(bytes, effort);
            REQUIRE_TRUE(count > previous);
            REQUIRE_TRUE(count <= bytes);
            REQUIRE_EQ(count == bytes, effort == 9);
            previous = count;
        }
    }
    for (const auto bytes : {0U, flitzzip::kMaxArchiveBlockBytes + 1U}) {
        bool rejected = false;
        try {
            (void)flitzzip::entropy_sample_count(bytes, 5);
        } catch (const flitzzip::GpuError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Prove exact nested counting and full histogram identity across permutation and tail boundaries.
// Inputs: Deterministic nonperiodic spans of 1-259 bytes and odd large spans, sampled at every budget.
// Outputs: Requires sample totals, unchanged repeated targets, monotone per-symbol counts, and exact full counts.
TEST_CASE(flzip_entropy_sampling_histograms_are_exact_nested_permutations) {
    std::vector<std::size_t> lengths;
    for (std::size_t size = 1U; size <= 259U; ++size) {
        lengths.push_back(size);
    }
    lengths.insert(lengths.end(), {4095U, 4096U, 4097U, 65537U, 262145U, 8388609U});
    for (const auto size : lengths) {
        std::vector<std::byte> input(size);
        std::array<std::uint64_t, 256> exact{};
        std::uint32_t state = 0x183D75A9U;
        for (auto& byte : input) {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            byte = static_cast<std::byte>(state & 255U);
            ++exact[state & 255U];
        }
        flitzzip::EntropyHistogramSampler sampler(input);
        std::array<std::uint64_t, 256> previous{};
        for (int effort = 2; effort <= 9; ++effort) {
            const auto target = flitzzip::entropy_sample_count(size, effort);
            const auto current = sampler.sample_to(target);
            std::uint64_t sum = 0U;
            for (std::size_t symbol = 0U; symbol < exact.size(); ++symbol) {
                REQUIRE_TRUE(current[symbol] >= previous[symbol]);
                REQUIRE_TRUE(current[symbol] <= exact[symbol]);
                sum += current[symbol];
            }
            REQUIRE_EQ(sum, target);
            REQUIRE_EQ(sampler.sample_to(target), current);
            previous = current;
        }
        REQUIRE_EQ(previous, exact);
        bool rejected = false;
        try {
            (void)sampler.sample_to(size + 1U);
        } catch (const flitzzip::GpuError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Detect sampling aliases that mistake periodic record headers for the full byte distribution.
// Inputs: Four MiB of periodic records with shifted 64-byte zero headers and nonzero bodies, entirely in RAM.
// Outputs: Requires early counts to avoid header domination and full counts to match every period and phase.
TEST_CASE(flzip_entropy_sampling_does_not_alias_periodic_record_headers) {
    for (const std::size_t period : {512U, 4096U, 32768U}) {
        for (const std::size_t phase : {0U, 64U, 192U}) {
            std::vector<std::byte> input(4U * 1024U * 1024U, std::byte{1});
            for (std::size_t start = phase; start < input.size(); start += period) {
                std::fill_n(input.begin() + static_cast<std::ptrdiff_t>(start), 64U, std::byte{0});
            }
            flitzzip::EntropyHistogramSampler sampler(input);
            const auto target = flitzzip::entropy_sample_count(input.size(), 2);
            const auto first = sampler.sample_to(target);
            REQUIRE_TRUE(first[0] <= target * 64U / period + target / 8U);
            REQUIRE_EQ(first[0] + first[1], target);
            const auto full = sampler.sample_to(input.size());
            REQUIRE_EQ(full[0], input.size() * 64U / period);
            REQUIRE_EQ(full[1], input.size() - full[0]);
        }
    }
}

// Purpose: Exercise every static/adaptive prefix symbol class without depending on a GPU encoder or device.
// Inputs: Independent reference frames, all byte values, segment boundaries and uneven final segments.
// Outputs: Requires exact CPU readback with guards intact, including escape symbols absent from adaptive codebooks.
TEST_CASE(flzip_prefix_reference_cpu_decode_covers_every_symbol) {
    for (const auto size : {4097U, 16384U, 65537U}) {
        for (const bool adaptive : {false, true}) {
            std::vector<std::byte> codebook;
            if (adaptive) {
                for (std::uint32_t symbol = 0; symbol < flitzzip::kGpuAdaptivePrefixCodebookBytes; ++symbol) {
                    codebook.push_back(static_cast<std::byte>(128U + symbol));
                }
            }
            std::vector<std::byte> expected(size, adaptive ? std::byte{128} : std::byte{0});
            for (std::size_t index = 0; index < expected.size(); index += 8U) {
                expected[index] = static_cast<std::byte>((index / 8U) & 255U);
            }
            const auto payload = reference_prefix_payload(expected, codebook);
            REQUIRE_TRUE(payload.size() < expected.size());
            const flitzzip::BlockDescriptor block{
                .kind = adaptive ? flitzzip::BlockKind::GpuAdaptivePrefix : flitzzip::BlockKind::GpuPrefix,
                .uncompressed_len = size,
                .encoded_len = static_cast<std::uint32_t>(payload.size()),
            };
            std::vector<std::byte> guarded(expected.size() + 2U, std::byte{0xBA});
            auto output = std::span(guarded).subspan(1U, expected.size());
            flitzzip::ArchiveCodecOptions options;
            options.block_size = 131072U;
            options.worker_count = 3U;
            flitzzip::decode_chunk_cpu(payload, std::span(&block, 1U), output, options);
            REQUIRE_TRUE(std::equal(output.begin(), output.end(), expected.begin()));
            REQUIRE_EQ(guarded.front(), std::byte{0xBA});
            REQUIRE_EQ(guarded.back(), std::byte{0xBA});
        }
    }
}

// Purpose: Anchor the independent encoder to manually calculated format bytes even on CPU-only hosts.
// Inputs: The four three-bit static symbols and a four-byte-aligned segment.
// Outputs: Requires the exact offset table and 0x0D10 little-bit-order symbol stream.
TEST_CASE(flzip_prefix_reference_known_bytes) {
    const std::array input{std::byte{0}, std::byte{1}, std::byte{2}, std::byte{3}};
    const std::vector<std::byte> expected{std::byte{0},    std::byte{0},    std::byte{0}, std::byte{0},
                                          std::byte{4},    std::byte{0},    std::byte{0}, std::byte{0},
                                          std::byte{0x10}, std::byte{0x0D}, std::byte{0}, std::byte{0}};
    REQUIRE_TRUE(reference_prefix_payload(input, {}) == expected);
}

// Purpose: Reject malformed prefix grammar identically in CPU and required-HIP decode and CRC paths.
// Inputs: One static/adaptive block with a truncated bitstream or an invalid static high-byte code.
// Outputs: Requires every decode and CRC route to throw ArchiveError before accepting the block.
TEST_CASE(flzip_gpu_prefix_malformed_codewords_are_rejected) {
    const bool gpu_available = flitzzip::query_gpu_info().available;
    constexpr std::uint32_t decoded_len = 128U;
    for (const auto [kind, invalid_high] : {
             std::pair{flitzzip::BlockKind::GpuPrefix, false},
             std::pair{flitzzip::BlockKind::GpuPrefix, true},
             std::pair{flitzzip::BlockKind::GpuAdaptivePrefix, false},
         }) {
        const auto codebook_bytes =
            kind == flitzzip::BlockKind::GpuAdaptivePrefix ? flitzzip::kGpuAdaptivePrefixCodebookBytes : 0U;
        std::vector<std::byte> payload(codebook_bytes + 8U + 4U);
        payload[codebook_bytes + 4U] = std::byte{4};
        if (invalid_high) {
            std::fill(payload.end() - 4, payload.end(), std::byte{0xFF});
        }
        const flitzzip::BlockDescriptor block{
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
            flitzzip::GpuCodecOptions options;
            options.require_gpu = !force_cpu;
            options.force_cpu = force_cpu;
            bool decode_rejected = false;
            try {
                (void)flitzzip::decode_chunk(payload, std::span(&block, 1), output, options);
            } catch (const flitzzip::ArchiveError&) {
                decode_rejected = true;
            }
            REQUIRE_TRUE(decode_rejected);

            bool crc_rejected = false;
            try {
                (void)flitzzip::crc_decoded_chunk(payload, std::span(&block, 1), decoded_len, options);
            } catch (const flitzzip::ArchiveError&) {
                crc_rejected = true;
            }
            REQUIRE_TRUE(crc_rejected);
        }
    }
}

// Purpose: Prove exact HIP packing against independent reference bytes across symbols and word boundaries.
// Inputs: RAM-only fixtures with every static symbol at all 32 bit alignments, shifted alphabets, and partial tails.
// Outputs: Requires exact payloads, including offset tables and padding, plus CPU/HIP byte-exact decoding.
TEST_CASE(flzip_gpu_prefix_packing_matches_reference) {
    if (!flitzzip::query_gpu_info().available) {
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
                if (fixture.size() % flitzzip::kGpuPrefixSegmentBytes == 0U) {
                    bit_position = 0;
                }
            }
            fixture.push_back(static_cast<std::byte>(value));
            bit_position += reference_prefix_symbol(static_cast<std::byte>(value), {}).second;
            if (fixture.size() % flitzzip::kGpuPrefixSegmentBytes == 0U) {
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
            flitzzip::GpuCodecOptions options;
            options.block_size = 256U * 1024U;
            options.compression_level = level;
            const auto encoded = flitzzip::encode_chunk(input, options);
            REQUIRE_TRUE(encoded.gpu_used);
            REQUIRE_EQ(encoded.blocks.size(), 1U);
            REQUIRE_EQ(encoded.blocks.front().kind,
                       level == 1 ? flitzzip::BlockKind::GpuPrefix : flitzzip::BlockKind::GpuAdaptivePrefix);
            REQUIRE_TRUE(encoded.payload.size() >= (level == 2 ? flitzzip::kGpuAdaptivePrefixCodebookBytes : 0U));
            const auto codebook = std::span<const std::byte>(encoded.payload)
                                      .first(level == 2 ? flitzzip::kGpuAdaptivePrefixCodebookBytes : 0U);
            REQUIRE_TRUE(encoded.payload == reference_prefix_payload(input, codebook));
            std::vector<std::byte> decoded(input.size());
            REQUIRE_TRUE(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
            REQUIRE_TRUE(decoded == input);
            options.require_gpu = false;
            options.force_cpu = true;
            REQUIRE_TRUE(!flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
            REQUIRE_TRUE(decoded == input);
        }
    }
}

// Purpose: Keep the smallest native representation independently for each block in a heterogeneous chunk.
// Inputs: RAM-only low/high-byte entropy, fill, pattern, and short raw regions at strong compression levels.
// Outputs: Requires mixed static/adaptive selection, dense payloads, and exact CPU/HIP restoration and CRC.
TEST_CASE(flzip_gpu_prefix_candidate_selection_is_per_block) {
    if (!flitzzip::query_gpu_info().available) {
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
            flitzzip::GpuCodecOptions options;
            options.require_gpu = true;
            options.compression_level = level;
            options.block_size = block_size;
            const auto encoded = flitzzip::encode_chunk(input, options);
            REQUIRE_EQ(encoded.blocks.size(), 5U);
            REQUIRE_EQ(encoded.blocks[0].kind, flitzzip::BlockKind::GpuHuffman);
            REQUIRE_EQ(encoded.blocks[1].kind, flitzzip::BlockKind::GpuAdaptivePrefix);
            REQUIRE_EQ(encoded.blocks[2].kind, flitzzip::BlockKind::Fill);
            REQUIRE_EQ(encoded.blocks[3].kind, flitzzip::BlockKind::Pattern);
            REQUIRE_EQ(encoded.blocks[4].kind, flitzzip::BlockKind::Raw);
            std::size_t offset = 0;
            for (std::size_t i = 0; i < encoded.blocks.size(); ++i) {
                const auto& block = encoded.blocks[i];
                const auto individual = flitzzip::encode_chunk(
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
            REQUIRE_TRUE(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
            REQUIRE_TRUE(decoded == input);
            const auto crc = flitzzip::crc_decoded_chunk(encoded.payload, encoded.blocks, input.size(), options);
            REQUIRE_TRUE(crc.gpu_used);
            REQUIRE_EQ(crc.crc32, flitzzip::crc32(input));
            options.require_gpu = false;
            options.force_cpu = true;
            std::fill(decoded.begin(), decoded.end(), std::byte{0xBB});
            REQUIRE_TRUE(!flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
            REQUIRE_TRUE(decoded == input);
        }
    }
}

// Purpose: Replace a static GPU prefix only when the first stronger effort measures a size gain.
// Inputs: One deterministic low-byte RAM-only block encoded at levels one and two with independent telemetry.
// Outputs: Level two emits a smaller Huffman block and still decodes byte-for-byte on the GPU.
TEST_CASE(flzip_gpu_huffman_replaces_static_prefix_when_smaller) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> input(1024U * 1024U);
    std::uint32_t random = 0xC001D00DU;
    for (auto& byte : input) {
        random = random * 1664525U + 1013904223U;
        byte = static_cast<std::byte>((random >> 16U) & 3U);
    }
    flitzzip::GpuCodecOptions options;
    options.require_gpu = true;
    options.compression_level = 1;
    options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
    const auto balanced = flitzzip::encode_chunk(input, options);
    const auto balanced_stats = flitzzip::snapshot_gpu_telemetry(*options.telemetry);
    options.compression_level = 2;
    options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
    const auto maximum = flitzzip::encode_chunk(input, options);
    const auto maximum_stats = flitzzip::snapshot_gpu_telemetry(*options.telemetry);
    REQUIRE_TRUE(maximum.payload.size() < balanced.payload.size());
    REQUIRE_EQ(maximum.blocks.size(), 1U);
    REQUIRE_EQ(maximum.blocks[0].kind, flitzzip::BlockKind::GpuHuffman);
    REQUIRE_TRUE(maximum_stats.kernel_launches > balanced_stats.kernel_launches);
    std::vector<std::byte> decoded(input.size());
    REQUIRE_TRUE(flitzzip::decode_chunk(maximum.payload, maximum.blocks, decoded, options));
    REQUIRE_TRUE(decoded == input);
}

// Purpose: Demonstrate stronger native compression on data that the static low-byte code cannot compact.
// Inputs: A deterministic 1 MiB high-byte alphabet encoded at all nine efforts, entirely in RAM.
// Outputs: Reports exact sizes, requires a full-sample Huffman gain, and checks CPU/HIP roundtrips.
TEST_CASE(flzip_gpu_prefix_levels_compact_shifted_alphabet) {
    if (!flitzzip::query_gpu_info().available) {
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
        flitzzip::GpuCodecOptions options;
        options.require_gpu = true;
        options.compression_level = level;
        const auto encoded = flitzzip::encode_chunk(input, options);
        REQUIRE_TRUE(encoded.gpu_used);
        REQUIRE_TRUE(encoded.payload.size() <= previous_bytes);
        previous_bytes = encoded.payload.size();
        encoded_sizes[level_index++] = encoded.payload.size();
        std::vector<std::byte> decoded(input.size());
        REQUIRE_TRUE(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
        REQUIRE_TRUE(decoded == input);
        options.require_gpu = false;
        options.force_cpu = true;
        std::fill(decoded.begin(), decoded.end(), std::byte{0xBB});
        REQUIRE_TRUE(!flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
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
TEST_CASE(flzip_gpu_entropy_efforts_preserve_per_block_winners) {
    if (!flitzzip::query_gpu_info().available) {
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
        flitzzip::GpuCodecOptions options;
        options.require_gpu = true;
        options.block_size = block_bytes;
        options.compression_level = effort;
        options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
        const auto encoded = flitzzip::encode_chunk(input, options);
        REQUIRE_TRUE(encoded.gpu_used);
        REQUIRE_EQ(encoded.blocks.size(), previous_blocks.size());
        REQUIRE_TRUE(encoded.payload.size() <= previous_bytes);
        previous_bytes = encoded.payload.size();
        for (std::size_t block = 0U; block < encoded.blocks.size(); ++block) {
            REQUIRE_TRUE(encoded.blocks[block].encoded_len <= previous_blocks[block]);
            previous_blocks[block] = encoded.blocks[block].encoded_len;
        }
        std::vector<std::byte> decoded(input.size());
        REQUIRE_TRUE(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
        REQUIRE_TRUE(decoded == input);
        REQUIRE_TRUE(flitzzip::snapshot_gpu_telemetry(*options.telemetry).kernel_launches > 0U);
        options.require_gpu = false;
        options.force_cpu = true;
        std::fill(decoded.begin(), decoded.end(), std::byte{0xBD});
        REQUIRE_TRUE(!flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
        REQUIRE_TRUE(decoded == input);
        std::cout << "nested_entropy_case level=" << effort << " input_bytes=" << input.size()
                  << " output_bytes=" << encoded.payload.size() << " memory_only=true disk_write_bytes=0\n";
    }
}

// Purpose: Preserve dictionary competition when a stronger entropy table crosses a heuristic ratio threshold.
// Inputs: A bounded zero/text mixed block at all nine efforts, with the available HIP device.
// Outputs: Requires non-growing dictionary payloads, exact CPU/HIP read-back, and real kernel telemetry.
TEST_CASE(flzip_gpu_efforts_preserve_dictionary_candidates_after_entropy_gain) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    constexpr std::string_view phrase = "Shared IEEE CRC compatibility; independent output identity.";
    std::vector<std::byte> input(1024U * 1024U, std::byte{0});
    for (std::size_t index = input.size() / 2U; index < input.size(); ++index) {
        input[index] = static_cast<std::byte>(phrase[(index - input.size() / 2U) % phrase.size()]);
    }
    auto previous_bytes = input.size();
    for (const int effort : {1, 2, 3, 4, 5, 6, 7, 8, 9}) {
        flitzzip::GpuCodecOptions options;
        options.require_gpu = true;
        options.block_size = static_cast<std::uint32_t>(input.size());
        options.compression_level = effort;
        options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
        const auto encoded = flitzzip::encode_chunk(input, options);
        REQUIRE_TRUE(encoded.gpu_used);
        REQUIRE_EQ(encoded.blocks.size(), 1U);
        REQUIRE_EQ(encoded.blocks.front().kind, flitzzip::BlockKind::GpuDictionary);
        REQUIRE_TRUE(encoded.payload.size() < 32768U);
        REQUIRE_TRUE(encoded.payload.size() <= previous_bytes);
        previous_bytes = encoded.payload.size();
        std::vector<std::byte> decoded(input.size());
        REQUIRE_TRUE(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
        REQUIRE_TRUE(decoded == input);
        REQUIRE_TRUE(flitzzip::snapshot_gpu_telemetry(*options.telemetry).kernel_launches > 0U);
        options.require_gpu = false;
        options.force_cpu = true;
        std::fill(decoded.begin(), decoded.end(), std::byte{0xBC});
        REQUIRE_TRUE(!flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
        REQUIRE_TRUE(decoded == input);
    }
}

// Purpose: Accept complete sparse-alphabet Huffman lookups while rejecting uncovered or conflicting slots.
// Inputs: A two-symbol 4096-entry lookup with individual malformed-entry mutations.
// Outputs: Validates the version-eight trust boundary independently of GPU availability.
TEST_CASE(flzip_gpu_huffman_sparse_lookup_validation) {
    std::vector<std::byte> lookup(flitzzip::kGpuHuffmanLookupBytes);
    for (std::size_t slot = 0U; slot < flitzzip::kGpuHuffmanLookupEntries; ++slot) {
        lookup[slot * 2U] = static_cast<std::byte>(201U + (slot & 1U));
        lookup[slot * 2U + 1U] = std::byte{1};
    }
    REQUIRE_TRUE(flitzzip::huffman_lookup_is_complete(lookup));
    lookup[1] = std::byte{0};
    REQUIRE_TRUE(!flitzzip::huffman_lookup_is_complete(lookup));
    lookup[1] = std::byte{1};
    lookup[0] = std::byte{202};
    REQUIRE_TRUE(!flitzzip::huffman_lookup_is_complete(lookup));
}

// Purpose: Reject truncated and overlong Huffman tables even when their available entries are individually valid.
// Inputs: A complete two-symbol lookup with every shorter byte extent and one extra trailing byte.
// Outputs: Requires rejection at the length boundary and valid reuse without changing the caller's bytes.
TEST_CASE(flzip_gpu_huffman_lookup_requires_exact_extent) {
    std::vector<std::byte> lookup(flitzzip::kGpuHuffmanLookupBytes + 1U);
    for (std::size_t slot = 0U; slot < flitzzip::kGpuHuffmanLookupEntries; ++slot) {
        lookup[slot * 2U] = static_cast<std::byte>(201U + (slot & 1U));
        lookup[slot * 2U + 1U] = std::byte{1};
    }
    const auto original = lookup;
    const auto bytes = std::span<const std::byte>(lookup);
    for (std::size_t length = 0; length < flitzzip::kGpuHuffmanLookupBytes; ++length) {
        REQUIRE_TRUE(!flitzzip::huffman_lookup_is_complete(bytes.first(length)));
    }
    REQUIRE_TRUE(!flitzzip::huffman_lookup_is_complete(bytes));
    REQUIRE_TRUE(flitzzip::huffman_lookup_is_complete(bytes.first(flitzzip::kGpuHuffmanLookupBytes)));
    REQUIRE_TRUE(lookup == original);
}

// Purpose: Reject malformed version-eight Huffman lookup and segment metadata in both decoders.
// Inputs: Fully populated and sparse-alphabet Huffman blocks with invalid widths, leaves, or offsets.
// Outputs: CPU and GPU decode reject each mutation rather than publishing a partial result.
TEST_CASE(flzip_gpu_huffman_corruption_is_rejected) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> input(1024U * 1024U);
    std::uint32_t state = 0xC001D00DU;
    for (auto& byte : input) {
        state = state * 1664525U + 1013904223U;
        byte = static_cast<std::byte>(201U + ((state >> 16U) & 3U));
    }
    flitzzip::GpuCodecOptions options;
    options.require_gpu = true;
    for (const int level : {5, 9}) {
        options.compression_level = level;
        const auto encoded = flitzzip::encode_chunk(input, options);
        REQUIRE_EQ(encoded.blocks.size(), 1U);
        REQUIRE_EQ(encoded.blocks[0].kind, flitzzip::BlockKind::GpuHuffman);
        for (const int corruption : {0, 1, 2}) {
            auto damaged = encoded.payload;
            if (corruption == 0) {
                for (std::size_t index = 1U; index < flitzzip::kGpuHuffmanLookupBytes; index += 2U) {
                    damaged[index] = std::byte{0};
                }
            } else if (corruption == 1) {
                const auto segment_count =
                    (input.size() + flitzzip::kGpuPrefixSegmentBytes - 1U) / flitzzip::kGpuPrefixSegmentBytes;
                const auto final_offset = flitzzip::kGpuHuffmanLookupBytes + segment_count * sizeof(std::uint32_t);
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
                    (void)flitzzip::decode_chunk(damaged, encoded.blocks, output, options);
                } catch (const flitzzip::ArchiveError&) {
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
TEST_CASE(flzip_gpu_prefix_segment_boundaries_roundtrip) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    constexpr std::array<std::size_t, 5> segment_counts{1U, 31U, 127U, 128U, 129U};
    for (const auto segments : segment_counts) {
        for (const int level : {1, 2}) {
            const auto tail_trim = segments == 1U ? 0U : 13U;
            std::vector<std::byte> input(segments * flitzzip::kGpuPrefixSegmentBytes - tail_trim);
            std::uint32_t random = 0xC001D00DU;
            for (auto& byte : input) {
                random = random * 1664525U + 1013904223U;
                const auto symbol = (random >> 16U) & 3U;
                byte = static_cast<std::byte>(level == 2 ? symbol + 201U : symbol);
            }
            flitzzip::GpuCodecOptions options;
            options.require_gpu = true;
            options.compression_level = level;
            options.block_size = 1024U * 1024U;
            const auto encoded = flitzzip::encode_chunk(input, options);
            const auto expected_kind =
                level == 2 ? flitzzip::BlockKind::GpuAdaptivePrefix : flitzzip::BlockKind::GpuPrefix;
            REQUIRE_TRUE(std::ranges::any_of(
                encoded.blocks, [expected_kind](const auto& block) { return block.kind == expected_kind; }));
            std::vector<std::byte> decoded(input.size(), std::byte{0xAA});
            REQUIRE_TRUE(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
            REQUIRE_TRUE(decoded == input);
            const auto crc = flitzzip::crc_decoded_chunk(encoded.payload, encoded.blocks, input.size(), options);
            REQUIRE_TRUE(crc.gpu_used);
            REQUIRE_EQ(crc.crc32, flitzzip::crc32(input));
            options.require_gpu = false;
            options.force_cpu = true;
            std::fill(decoded.begin(), decoded.end(), std::byte{0xBB});
            REQUIRE_TRUE(!flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
            REQUIRE_TRUE(decoded == input);
        }
    }
}

// Purpose: Verify required-HIP compression can emit a real GPU prefix-compressed native block.
// Inputs: A low-byte payload that is not fill or periodic and is compressed with `gpu_required`.
// Outputs: Throws if the archive stays raw-sized, uses CPU deflate, omits prefix blocks, or fails required-HIP
// verification/extraction.
TEST_CASE(flzip_required_gpu_prefix_blocks_compress_low_entropy_payload) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("flzip-required-gpu-prefix");
    const auto input = root / "low-entropy.bin";
    write_low_entropy_payload(input, 2U * 1024U * 1024U);
    const auto source_size = std::filesystem::file_size(input);
    const auto archive = root / "archive.flzip";

    flitzzip::CompressOptions compress;
    compress.gpu_required = true;
    compress.force_cpu = false;
    compress.chunk_size = 2U * 1024U * 1024U;
    compress.block_size = 1024U * 1024U;
    compress.compression_level = 1;
    compress.verify_after_write = true;
    const auto compressed = flitzzip::compress_flzip({input}, archive, compress);
    REQUIRE_TRUE(compressed.gpu_used);
    REQUIRE_TRUE(compressed.gpu_runtime.prefix_blocks > 0U);
    REQUIRE_TRUE(compressed.output_bytes < (source_size * 3U) / 4U);

    const auto index = read_test_archive_index(archive);
    REQUIRE_TRUE(archive_contains_block_kind(index, flitzzip::BlockKind::GpuPrefix));
    REQUIRE_TRUE(!archive_contains_block_kind(index, flitzzip::BlockKind::Deflate));

    flitzzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 2U * 1024U * 1024U;
    verify.block_size = 1024U * 1024U;
    const auto verified = flitzzip::verify_flzip(archive, verify);
    REQUIRE_TRUE(verified.gpu_used);

    const auto output = root / "out";
    verify.overwrite = true;
    const auto extracted = flitzzip::extract_flzip(archive, output, verify);
    REQUIRE_TRUE(extracted.gpu_used);
    REQUIRE_EQ(std::filesystem::file_size(output / "low-entropy.bin"), source_size);
    REQUIRE_TRUE(files_are_equal(output / "low-entropy.bin", input));
    std::filesystem::remove_all(root);
}

// Purpose: Verify raw blocks inside a mixed fill/raw chunk still receive GPU prefix compression.
// Inputs: A two-block payload where the first block is fill-compressed and the second is low-entropy raw data.
// Outputs: Throws if the raw block stays uncompressed only because the same chunk also contains a fill block.
TEST_CASE(flzip_required_gpu_prefix_blocks_compress_raw_blocks_inside_mixed_chunk) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("flzip-required-gpu-mixed-prefix");
    const auto input = root / "mixed-fill-low-entropy.bin";
    write_fill_then_low_entropy_payload(input, 1024U * 1024U);
    const auto source_size = std::filesystem::file_size(input);
    const auto archive = root / "archive.flzip";

    flitzzip::CompressOptions compress;
    compress.gpu_required = true;
    compress.force_cpu = false;
    compress.chunk_size = 2U * 1024U * 1024U;
    compress.block_size = 1024U * 1024U;
    compress.compression_level = 1;
    compress.verify_after_write = true;
    const auto compressed = flitzzip::compress_flzip({input}, archive, compress);
    REQUIRE_TRUE(compressed.gpu_used);
    REQUIRE_TRUE(compressed.gpu_runtime.prefix_blocks > 0U);
    REQUIRE_TRUE(compressed.output_bytes < source_size);

    const auto index = read_test_archive_index(archive);
    REQUIRE_TRUE(archive_contains_block_kind(index, flitzzip::BlockKind::Fill));
    REQUIRE_TRUE(archive_contains_block_kind(index, flitzzip::BlockKind::GpuPrefix));
    REQUIRE_TRUE(!archive_contains_block_kind(index, flitzzip::BlockKind::Deflate));

    flitzzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 2U * 1024U * 1024U;
    verify.block_size = 1024U * 1024U;
    const auto verified = flitzzip::verify_flzip(archive, verify);
    REQUIRE_TRUE(verified.gpu_used);

    const auto output = root / "out";
    verify.overwrite = true;
    const auto extracted = flitzzip::extract_flzip(archive, output, verify);
    REQUIRE_TRUE(extracted.gpu_used);
    REQUIRE_EQ(std::filesystem::file_size(output / "mixed-fill-low-entropy.bin"), source_size);
    REQUIRE_TRUE(files_are_equal(output / "mixed-fill-low-entropy.bin", input));
    std::filesystem::remove_all(root);
}

// Purpose: Verify Balanced and Maximum HIP levels can emit GPU Huffman blocks without CPU deflate.
// Inputs: A high-byte low-entropy payload where static low-value prefix coding is intentionally weak.
// Outputs: Throws if either level fails to beat level 1, omits Huffman blocks, emits deflate, or fails read-back.
TEST_CASE(flzip_required_gpu_huffman_blocks_honor_compression_level) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("flzip-required-gpu-adaptive-prefix");
    const auto input = root / "shifted-low-entropy.bin";
    write_shifted_low_entropy_payload(input, 2U * 1024U * 1024U);
    const auto source_size = std::filesystem::file_size(input);
    const auto fast_archive = root / "fast.flzip";
    const auto balanced_archive = root / "balanced.flzip";
    const auto strong_archive = root / "strong.flzip";

    flitzzip::CompressOptions fast;
    fast.gpu_required = true;
    fast.force_cpu = false;
    fast.chunk_size = 2U * 1024U * 1024U;
    fast.block_size = 1024U * 1024U;
    fast.compression_level = 1;
    fast.verify_after_write = true;
    const auto fast_stats = flitzzip::compress_flzip({input}, fast_archive, fast);
    REQUIRE_TRUE(fast_stats.gpu_used);

    auto balanced = fast;
    balanced.compression_level = 5;
    const auto balanced_stats = flitzzip::compress_flzip({input}, balanced_archive, balanced);
    REQUIRE_TRUE(balanced_stats.gpu_used);
    REQUIRE_TRUE(balanced_stats.gpu_runtime.prefix_blocks > 0U);
    REQUIRE_TRUE(balanced_stats.output_bytes < fast_stats.output_bytes);
    const auto balanced_index = read_test_archive_index(balanced_archive);
    REQUIRE_TRUE(archive_contains_block_kind(balanced_index, flitzzip::BlockKind::GpuHuffman));
    REQUIRE_EQ(balanced_index.version, 8U);
    REQUIRE_TRUE(!archive_contains_block_kind(balanced_index, flitzzip::BlockKind::Deflate));
    flitzzip::ExtractOptions cpu_verify;
    cpu_verify.gpu_required = false;
    cpu_verify.force_cpu = true;
    REQUIRE_TRUE(!flitzzip::verify_flzip(balanced_archive, cpu_verify).gpu_used);

    auto strong = fast;
    strong.compression_level = 9;
    const auto strong_stats = flitzzip::compress_flzip({input}, strong_archive, strong);
    REQUIRE_TRUE(strong_stats.gpu_used);
    REQUIRE_TRUE(strong_stats.gpu_runtime.prefix_blocks > 0U);
    REQUIRE_TRUE(strong_stats.output_bytes < fast_stats.output_bytes);
    REQUIRE_TRUE(strong_stats.output_bytes < (source_size * 3U) / 4U);

    const auto index = read_test_archive_index(strong_archive);
    REQUIRE_TRUE(archive_contains_block_kind(index, flitzzip::BlockKind::GpuHuffman));
    REQUIRE_TRUE(!archive_contains_block_kind(index, flitzzip::BlockKind::Deflate));

    flitzzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 2U * 1024U * 1024U;
    verify.block_size = 1024U * 1024U;
    const auto verified = flitzzip::verify_flzip(strong_archive, verify);
    REQUIRE_TRUE(verified.gpu_used);

    const auto output = root / "out";
    verify.overwrite = true;
    const auto extracted = flitzzip::extract_flzip(strong_archive, output, verify);
    REQUIRE_TRUE(extracted.gpu_used);
    REQUIRE_EQ(std::filesystem::file_size(output / "shifted-low-entropy.bin"), source_size);
    REQUIRE_TRUE(files_are_equal(output / "shifted-low-entropy.bin", input));
    std::filesystem::remove_all(root);
}

// Purpose: Verify required-HIP verification rejects corrupted GPU-prefix payload bytes.
// Inputs: A valid required-HIP prefix archive with one encoded bitstream byte flipped after compression.
// Outputs: Throws if verification silently accepts the corrupted native GPU-prefix payload.
TEST_CASE(flzip_required_gpu_prefix_payload_corruption_is_rejected) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("flzip-required-gpu-prefix-corrupt");
    const auto input = root / "low-entropy.bin";
    write_low_entropy_payload(input, 1024U * 1024U);
    const auto archive = root / "archive.flzip";

    flitzzip::CompressOptions compress;
    compress.gpu_required = true;
    compress.force_cpu = false;
    compress.chunk_size = 1024U * 1024U;
    compress.block_size = 1024U * 1024U;
    compress.compression_level = 1;
    const auto compressed = flitzzip::compress_flzip({input}, archive, compress);
    REQUIRE_TRUE(compressed.gpu_used);
    REQUIRE_TRUE(compressed.gpu_runtime.prefix_blocks > 0U);

    const auto prefix = find_first_prefix_block(read_test_archive_index(archive));
    const auto table_bytes = test_gpu_prefix_table_bytes(prefix.block.uncompressed_len);
    REQUIRE_TRUE(prefix.block.encoded_len > table_bytes);
    xor_archive_byte(archive, prefix.payload_offset + prefix.block.encoded_offset + table_bytes, 0x01U);

    flitzzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 1024U * 1024U;
    verify.block_size = 1024U * 1024U;
    bool rejected = false;
    try {
        (void)flitzzip::verify_flzip(archive, verify);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify required-HIP verification rejects corrupted GPU-prefix table offsets before GPU materialization.
// Inputs: A valid required-HIP prefix archive with its final prefix table offset corrupted after compression.
// Outputs: Throws if verification reaches a GPU read using malformed table-controlled offsets.
TEST_CASE(flzip_required_gpu_prefix_table_corruption_is_rejected) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("flzip-required-gpu-prefix-table-corrupt");
    const auto input = root / "low-entropy.bin";
    write_low_entropy_payload(input, 1024U * 1024U);
    const auto archive = root / "archive.flzip";

    flitzzip::CompressOptions compress;
    compress.gpu_required = true;
    compress.force_cpu = false;
    compress.chunk_size = 1024U * 1024U;
    compress.block_size = 1024U * 1024U;
    compress.compression_level = 1;
    const auto compressed = flitzzip::compress_flzip({input}, archive, compress);
    REQUIRE_TRUE(compressed.gpu_used);
    REQUIRE_TRUE(compressed.gpu_runtime.prefix_blocks > 0U);

    const auto prefix = find_first_prefix_block(read_test_archive_index(archive));
    const auto table_bytes = test_gpu_prefix_table_bytes(prefix.block.uncompressed_len);
    REQUIRE_TRUE(prefix.block.encoded_len > table_bytes);
    xor_archive_byte(archive, prefix.payload_offset + prefix.block.encoded_offset + table_bytes - 1U, 0x80U);

    flitzzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 1024U * 1024U;
    verify.block_size = 1024U * 1024U;
    bool rejected = false;
    try {
        (void)flitzzip::verify_flzip(archive, verify);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Measure required-HIP pattern materialization across short, non-power-of-two, and long periods.
// Inputs: Opt-in environment flag and three deterministic 16 MiB in-memory pattern blocks.
// Outputs: Prints median host-wall decode times only after exact-byte validation; has no pass/fail timing threshold.
TEST_CASE(flzip_gpu_pattern_decode_benchmark_opt_in) {
    const auto* enabled = std::getenv("FLITZZIP_PATTERN_GPU_BENCHMARK");
    if (enabled == nullptr || std::string_view(enabled) != "1" || !flitzzip::query_gpu_info().available) {
        return;
    }
    constexpr std::size_t output_bytes = flitzzip::kMaxArchiveBlockBytes;
    flitzzip::GpuCodecOptions options;
    options.require_gpu = true;
    options.force_cpu = false;
    for (const std::uint32_t period : {3U, 257U, 16384U}) {
        std::vector<std::byte> pattern(period);
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            pattern[i] = static_cast<std::byte>((i * 73U + i / 11U) & 255U);
        }
        const flitzzip::BlockDescriptor block{
            .kind = flitzzip::BlockKind::Pattern,
            .uncompressed_len = static_cast<std::uint32_t>(output_bytes),
            .encoded_offset = 0U,
            .encoded_len = period,
        };
        std::vector<std::byte> decoded(output_bytes);
        std::array<double, 5> samples{};
        for (auto& milliseconds : samples) {
            const auto start = std::chrono::steady_clock::now();
            REQUIRE_TRUE(flitzzip::decode_chunk(pattern, std::span(&block, 1), decoded, options));
            milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            for (std::size_t i = 0; i < decoded.size(); ++i) {
                REQUIRE_EQ(decoded[i], pattern[i % pattern.size()]);
            }
        }
        std::sort(samples.begin(), samples.end());
        std::cout << "flzip_gpu_pattern_decode period=" << period << " output_bytes=" << output_bytes
                  << " median_ms=" << samples[samples.size() / 2U] << '\n';
    }
}

// Purpose: Cover GPU pattern phases at 64 KiB segment and independently encoded block boundaries.
// Inputs: Four consecutive non-aligned pattern blocks with short, odd, and maximum periods.
// Outputs: Requires byte-identical required-HIP and forced-CPU materialization for every decoded position.
TEST_CASE(flzip_gpu_pattern_decode_unaligned_boundaries) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const std::array<std::uint32_t, 4> periods{2U, 3U, 257U, 16384U};
    const std::array<std::uint32_t, 4> lengths{70013U, 131071U, 65539U, 200003U};
    std::vector<std::byte> payload;
    std::vector<std::byte> expected;
    std::vector<flitzzip::BlockDescriptor> blocks;
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
        blocks.push_back(flitzzip::BlockDescriptor{
            .kind = flitzzip::BlockKind::Pattern,
            .uncompressed_len = length,
            .encoded_offset = encoded_offset,
            .encoded_len = period,
        });
    }
    std::vector<std::byte> decoded(expected.size());
    flitzzip::GpuCodecOptions options;
    REQUIRE_TRUE(flitzzip::decode_chunk(payload, blocks, decoded, options));
    REQUIRE_TRUE(decoded == expected);
    options.require_gpu = false;
    options.force_cpu = true;
    std::fill(decoded.begin(), decoded.end(), std::byte{0});
    REQUIRE_TRUE(!flitzzip::decode_chunk(payload, blocks, decoded, options));
    REQUIRE_TRUE(decoded == expected);
}

// Purpose: Preserve lane positions while a mixed materializer skips independently decoded prefix windows.
// Inputs: Independent static/adaptive reference frames, unaligned 64 KiB boundaries and sub-thread-stride blocks.
// Outputs: Requires identical CPU/HIP bytes and device CRC, including raw/fill/pattern bytes after skipped windows.
TEST_CASE(flzip_gpu_mixed_materialization_skips_unaligned_prefix_windows) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    for (const auto lead : {1U, 255U, 256U, 257U, 65535U}) {
        for (const bool adaptive : {false, true}) {
            std::vector<std::byte> payload;
            std::vector<std::byte> expected;
            std::vector<flitzzip::BlockDescriptor> blocks;
            // Purpose: Append an independent block fixture; Inputs: kind, decoded/encoded bytes and fill value.
            // Outputs: Extends the dense layout without using production encode or materialization logic.
            const auto append = [&](flitzzip::BlockKind kind, const std::vector<std::byte>& input,
                                    const std::vector<std::byte>& encoded, std::uint8_t fill = 0U) {
                blocks.push_back({.kind = kind,
                                  .fill_value = fill,
                                  .uncompressed_len = static_cast<std::uint32_t>(input.size()),
                                  .encoded_offset = payload.size(),
                                  .encoded_len = static_cast<std::uint32_t>(encoded.size())});
                payload.insert(payload.end(), encoded.begin(), encoded.end());
                expected.insert(expected.end(), input.begin(), input.end());
            };
            const std::vector<std::byte> first(lead, std::byte{0xA7});
            append(flitzzip::BlockKind::Raw, first, first);
            std::vector<std::byte> codebook;
            if (adaptive) {
                codebook.resize(flitzzip::kGpuAdaptivePrefixCodebookBytes);
                for (std::size_t i = 0; i < codebook.size(); ++i) {
                    codebook[i] = static_cast<std::byte>(i);
                }
            }
            for (const auto coded_length : {65549U, 4113U}) {
                std::vector<std::byte> coded(coded_length);
                for (std::size_t i = 0; i < coded.size(); ++i) {
                    coded[i] = static_cast<std::byte>((i * 13U + i / 7U) & 3U);
                }
                append(adaptive ? flitzzip::BlockKind::GpuAdaptivePrefix : flitzzip::BlockKind::GpuPrefix, coded,
                       reference_prefix_payload(coded, codebook));
                // Several blocks are shorter than a lane stride: a lane can already be beyond their end.
                for (const auto fill_length : {1U, 2U, 253U, 257U}) {
                    append(flitzzip::BlockKind::Fill, std::vector<std::byte>(fill_length, std::byte{0xEF}), {}, 0xEFU);
                }
                std::vector<std::byte> patterned(509U);
                const std::vector motif{std::byte{0x91}, std::byte{0x26}, std::byte{0xF4}};
                for (std::size_t i = 0; i < patterned.size(); ++i) {
                    patterned[i] = motif[i % motif.size()];
                }
                append(flitzzip::BlockKind::Pattern, patterned, motif);
                const std::vector<std::byte> raw_tail(513U, std::byte{0x42});
                append(flitzzip::BlockKind::Raw, raw_tail, raw_tail);
            }
            flitzzip::GpuCodecOptions options;
            options.require_gpu = true;
            std::vector<std::byte> decoded(expected.size(), std::byte{0xCC});
            REQUIRE_TRUE(flitzzip::decode_chunk(payload, blocks, decoded, options));
            REQUIRE_TRUE(decoded == expected);
            const auto crc = flitzzip::crc_decoded_chunk(payload, blocks, expected.size(), options);
            REQUIRE_TRUE(crc.gpu_used);
            REQUIRE_EQ(crc.crc32, flitzzip::crc32(expected));
            options.require_gpu = false;
            options.force_cpu = true;
            std::fill(decoded.begin(), decoded.end(), std::byte{0xDD});
            REQUIRE_TRUE(!flitzzip::decode_chunk(payload, blocks, decoded, options));
            REQUIRE_TRUE(decoded == expected);
        }
    }
}
