#include "core/sparse_pattern_block.hpp"
#include "gpu/gpu_codec.hpp"
#include "gpu/sparse_pattern_candidate.hpp"
#if FLITZZIP_ENABLE_HIP
#include "gpu/sparse_pattern_device.hpp"
#endif
#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace {

// Purpose: Append an unsigned field in the sparse block's little-endian wire order.
// Inputs: `bytes` is a test payload and `value` is the field to append.
// Outputs: Adds four bytes to `bytes`.
void append_sparse_test_u32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (std::uint32_t index = 0U; index < sizeof(value); ++index) {
        bytes.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xFFU));
    }
}

// Purpose: Replace an existing unsigned field without changing payload shape.
// Inputs: `bytes` contains an admitted field at `offset`; `value` is its replacement.
// Outputs: Mutates the field in little-endian wire order.
void replace_sparse_test_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    REQUIRE_TRUE(offset <= bytes.size() && bytes.size() - offset >= sizeof(value));
    for (std::uint32_t index = 0U; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

// Purpose: Create a canonical four-byte motif and two disjoint corrections.
// Inputs: None.
// Outputs: Returns a fixed encoded block whose decoded size is 64 bytes.
std::vector<std::byte> make_sparse_test_payload() {
    std::vector<std::byte> bytes;
    append_sparse_test_u32(bytes, 4U);
    append_sparse_test_u32(bytes, 2U);
    for (const char symbol : {'A', 'B', 'C', 'D'}) {
        bytes.push_back(static_cast<std::byte>(symbol));
    }
    append_sparse_test_u32(bytes, 5U);
    bytes.push_back(std::byte{'X'});
    append_sparse_test_u32(bytes, 11U);
    bytes.push_back(std::byte{'Y'});
    return bytes;
}

// Purpose: Require untrusted sparse metadata to fail closed.
// Inputs: `payload` is invalid, `decoded_size` is its output extent, and `max_period` is the versioned bound.
// Outputs: Requires `ArchiveError` rather than acceptance or another exception type.
void require_sparse_rejected(std::span<const std::byte> payload, std::uint32_t decoded_size,
                             std::uint32_t max_period = flitzzip::kMaxGpuPatternBytes) {
    bool rejected = false;
    try {
        (void)flitzzip::parse_sparse_pattern_block(payload, decoded_size, max_period);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

}  // namespace

// Purpose: Keep sparse-field reads bounded even when an untrusted offset would wrap an addition-based check.
// Inputs: An independent byte sequence, valid unaligned reads and truncated/extreme offset cases.
// Outputs: Exact little-endian values for admitted reads; every incomplete field raises ArchiveError.
TEST_CASE(sparse_pattern_field_reader_rejects_truncation_and_wrapping_offsets) {
    const std::array<std::byte, 8> bytes{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78},
                                         std::byte{0x90}, std::byte{0xAB}, std::byte{0xCD}, std::byte{0xEF}};
    REQUIRE_EQ(flitzzip::read_sparse_u32(bytes, 0U), 0x78563412U);
    REQUIRE_EQ(flitzzip::read_sparse_u32(bytes, 1U), 0x90785634U);
    REQUIRE_EQ(flitzzip::read_sparse_u32(bytes, 4U), 0xEFCDAB90U);
    for (const auto offset : std::array<std::size_t, 5>{5U, 8U, 9U, std::numeric_limits<std::size_t>::max() - 1U,
                                                        std::numeric_limits<std::size_t>::max()}) {
        bool rejected = false;
        try {
            (void)flitzzip::read_sparse_u32(bytes, offset);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    for (std::size_t prefix = 0U; prefix < 4U; ++prefix) {
        bool rejected = false;
        try {
            (void)flitzzip::read_sparse_u32(std::span(bytes).first(prefix), 0U);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Admit a canonical sparse block and preserve exact borrowed motif/patch spans.
// Inputs: A 64-byte decoded extent with two strictly increasing nonredundant patches.
// Outputs: Requires the expected period and patch count without copying the payload.
TEST_CASE(sparse_pattern_block_admits_canonical_payload) {
    const auto bytes = make_sparse_test_payload();
    const auto layout = flitzzip::parse_sparse_pattern_block(bytes, 64U);
    REQUIRE_EQ(layout.motif.size(), 4U);
    REQUIRE_EQ(layout.patch_count, 2U);
    REQUIRE_EQ(layout.patches.size(), 2U * flitzzip::kSparsePatternPatchBytes);
    REQUIRE_EQ(layout.motif.front(), std::byte{'A'});
    REQUIRE_EQ(flitzzip::read_sparse_u32(layout.patches, 0U), 5U);
    REQUIRE_EQ(flitzzip::read_sparse_u32(layout.patches, flitzzip::kSparsePatternPatchBytes), 11U);
}

// Purpose: Reject sparse blocks whose metadata could cause partial, ambiguous, or out-of-bounds decode.
// Inputs: Truncation, false sizes, noncanonical periods, patch counts, positions, values, and trailing bytes.
// Outputs: Every mutation fails with `ArchiveError` before materialization.
TEST_CASE(sparse_pattern_block_rejects_malformed_payload) {
    const auto valid = make_sparse_test_payload();
    require_sparse_rejected(std::span(valid).first(7U), 64U);
    require_sparse_rejected(valid, 0U);
    require_sparse_rejected(valid, flitzzip::kMaxArchiveBlockBytes + 1U);
    require_sparse_rejected(valid, static_cast<std::uint32_t>(valid.size()));

    auto invalid = valid;
    replace_sparse_test_u32(invalid, 0U, 1U);
    require_sparse_rejected(invalid, 64U);
    invalid = valid;
    replace_sparse_test_u32(invalid, 0U, flitzzip::kMaxGpuPatternBytes + 1U);
    require_sparse_rejected(invalid, 64U);
    invalid = valid;
    replace_sparse_test_u32(invalid, 4U, 0U);
    require_sparse_rejected(invalid, 64U);
    invalid = valid;
    replace_sparse_test_u32(invalid, 4U, 0xFFFFFFFFU);
    require_sparse_rejected(invalid, 64U);
    invalid = valid;
    replace_sparse_test_u32(invalid, 12U, 3U);
    require_sparse_rejected(invalid, 64U);
    invalid = valid;
    replace_sparse_test_u32(invalid, 17U, 5U);
    require_sparse_rejected(invalid, 64U);
    invalid = valid;
    replace_sparse_test_u32(invalid, 17U, 64U);
    require_sparse_rejected(invalid, 64U);
    invalid = valid;
    invalid[16U] = std::byte{'B'};
    require_sparse_rejected(invalid, 64U);
    invalid = valid;
    invalid.push_back(std::byte{0});
    require_sparse_rejected(invalid, 64U);
}

// Purpose: Keep v5 sparse bounds unchanged while admitting only bounded v7 long motifs.
// Inputs: Canonical single-patch payloads at 16 KiB and 1 MiB period boundaries.
// Outputs: Each parser admits only its distinct versioned period range.
TEST_CASE(sparse_pattern_long_period_boundaries) {
    for (const auto period : {flitzzip::kMaxGpuPatternBytes, flitzzip::kMaxGpuPatternBytes + 1U,
                              flitzzip::kMaxGpuLongSparsePatternBytes, flitzzip::kMaxGpuLongSparsePatternBytes + 1U}) {
        std::vector<std::byte> payload;
        append_sparse_test_u32(payload, period);
        append_sparse_test_u32(payload, 1U);
        payload.resize(flitzzip::kSparsePatternHeaderBytes + period, std::byte{0});
        append_sparse_test_u32(payload, period);
        payload.push_back(std::byte{1});
        const auto decoded_size = 2U * period + 1U;
        if (period <= flitzzip::kMaxGpuPatternBytes) {
            REQUIRE_EQ(flitzzip::parse_sparse_pattern_block(payload, decoded_size).motif.size(), period);
        } else {
            require_sparse_rejected(payload, decoded_size);
        }
        if (period > flitzzip::kMaxGpuPatternBytes && period <= flitzzip::kMaxGpuLongSparsePatternBytes) {
            REQUIRE_EQ(
                flitzzip::parse_sparse_pattern_block(payload, decoded_size, flitzzip::kMaxGpuLongSparsePatternBytes)
                    .motif.size(),
                period);
        } else {
            require_sparse_rejected(payload, decoded_size, flitzzip::kMaxGpuLongSparsePatternBytes);
        }
    }
}

// Purpose: Exercise the new GPU-native long-sparse kind through independent CPU and HIP decoding.
// Inputs: A seeded 128 KiB motif repeated twice with one changed byte in a 256 KiB block.
// Outputs: Requires exact payload size, v7 block kind, GPU telemetry, and byte-exact CPU/HIP roundtrips.
TEST_CASE(long_sparse_pattern_required_hip_roundtrip) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    constexpr std::size_t period = 128U * 1024U;
    std::vector<std::byte> input(2U * period);
    std::uint32_t state = 0xC67A349DU;
    for (std::size_t index = 0U; index < period; ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        input[index] = static_cast<std::byte>(state >> 24U);
        input[index + period] = input[index];
    }
    input[period + 1024U] ^= std::byte{0xFF};

    flitzzip::GpuCodecOptions options;
    options.block_size = static_cast<std::uint32_t>(input.size());
    options.compression_level = 5;
    options.require_gpu = true;
    options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
    const auto encoded = flitzzip::encode_chunk(input, options);
    REQUIRE_TRUE(encoded.gpu_used);
    REQUIRE_EQ(encoded.blocks.size(), 1U);
    REQUIRE_EQ(encoded.blocks[0].kind, flitzzip::BlockKind::GpuLongSparsePattern);
    REQUIRE_EQ(encoded.payload.size(),
               flitzzip::kSparsePatternHeaderBytes + period + flitzzip::kSparsePatternPatchBytes);
    REQUIRE_EQ(flitzzip::snapshot_gpu_telemetry(*options.telemetry).sparse_pattern_blocks, 1U);
    std::vector<std::byte> decoded(input.size());
    REQUIRE_TRUE(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
    REQUIRE_EQ(decoded, input);
    options.require_gpu = false;
    options.force_cpu = true;
    std::fill(decoded.begin(), decoded.end(), std::byte{0});
    REQUIRE_TRUE(!flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
    REQUIRE_EQ(decoded, input);

    auto corrupt = encoded.payload;
    corrupt[flitzzip::kSparsePatternHeaderBytes + period + sizeof(std::uint32_t)] = input[1024U];
    bool rejected = false;
    try {
        (void)flitzzip::decode_chunk(corrupt, encoded.blocks, decoded, options);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify required-HIP sparse selection, telemetry, and both independent decode paths.
// Inputs: A seeded 4 KiB motif with one correction in each later record of a 1 MiB block.
// Outputs: Requires a complete smaller sparse payload, exact CPU/HIP decode, and one reported sparse block.
TEST_CASE(sparse_pattern_required_hip_encode_roundtrip) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    constexpr std::size_t motif_bytes = 4096U;
    constexpr std::size_t block_bytes = 1024U * 1024U;
    std::vector<std::byte> input(block_bytes);
    std::uint32_t state = 0x8A6754B3U;
    for (std::size_t index = 0U; index < motif_bytes; ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        input[index] = static_cast<std::byte>(state >> 24U);
    }
    for (std::size_t index = motif_bytes; index < input.size(); ++index) {
        input[index] = input[index % motif_bytes];
    }
    for (std::size_t record = 1U; record < input.size() / motif_bytes; ++record) {
        const auto position = record * motif_bytes + 512U;
        input[position] ^= std::byte{0xFF};
    }

    flitzzip::GpuCodecOptions options;
    options.block_size = static_cast<std::uint32_t>(block_bytes);
    options.compression_level = 5;
    options.require_gpu = true;
    options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
    const auto encoded = flitzzip::encode_chunk(input, options);
    REQUIRE_TRUE(encoded.gpu_used);
    REQUIRE_EQ(encoded.blocks.size(), 1U);
    REQUIRE_EQ(encoded.blocks[0].kind, flitzzip::BlockKind::GpuSparsePattern);
    const auto layout = flitzzip::parse_sparse_pattern_block(encoded.payload, static_cast<std::uint32_t>(block_bytes));
    REQUIRE_EQ(layout.motif.size(), motif_bytes);
    REQUIRE_EQ(layout.patch_count, 255U);
    REQUIRE_EQ(encoded.payload.size(), flitzzip::kSparsePatternHeaderBytes + motif_bytes +
                                           layout.patch_count * flitzzip::kSparsePatternPatchBytes);
    REQUIRE_EQ(flitzzip::snapshot_gpu_telemetry(*options.telemetry).sparse_pattern_blocks, 1U);

    std::vector<std::byte> decoded(input.size());
    REQUIRE_TRUE(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
    REQUIRE_EQ(decoded, input);
    options.require_gpu = false;
    options.force_cpu = true;
    std::fill(decoded.begin(), decoded.end(), std::byte{0});
    REQUIRE_TRUE(!flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
    REQUIRE_EQ(decoded, input);

    options.force_cpu = false;
    options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
    const auto optional = flitzzip::encode_chunk(input, options);
    REQUIRE_TRUE(optional.gpu_used);
    REQUIRE_EQ(optional.payload, encoded.payload);
    REQUIRE_EQ(flitzzip::snapshot_gpu_telemetry(*options.telemetry).sparse_pattern_blocks, 1U);

    options.require_gpu = true;
    options.compression_level = 9;
    options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
    const auto high_effort = flitzzip::encode_chunk(input, options);
    REQUIRE_TRUE(high_effort.gpu_used);
    REQUIRE_TRUE(high_effort.payload.size() <= encoded.payload.size());
    std::fill(decoded.begin(), decoded.end(), std::byte{0});
    REQUIRE_TRUE(flitzzip::decode_chunk(high_effort.payload, high_effort.blocks, decoded, options));
    REQUIRE_EQ(decoded, input);
}

// Purpose: Cover batched sparse collection when admitted blocks are separated by a noncandidate block.
// Inputs: Three sparse MiB blocks with divisible, nondivisible, and short motif periods, separated by a fill block.
// Outputs: Requires exact candidate-to-block mapping, three HIP sparse blocks, and byte-exact CPU/HIP decode.
TEST_CASE(sparse_pattern_hip_batch_preserves_block_offsets) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    constexpr std::size_t block_bytes = 1024U * 1024U;
    std::vector<std::byte> input(4U * block_bytes, std::byte{0});
    for (std::size_t block_index : {0U, 2U, 3U}) {
        const auto period = block_index == 0U ? 4096U : (block_index == 2U ? 8191U : 3U);
        const auto start = block_index * block_bytes;
        std::uint32_t state = block_index == 0U ? 0x8A6754B3U : 0xCA27E6D1U;
        for (std::size_t position = 0U; position < period; ++position) {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            input[start + position] = static_cast<std::byte>(state >> 24U);
        }
        for (std::size_t position = period; position < block_bytes; ++position) {
            input[start + position] = input[start + position % period];
        }
        const auto patch_stride = block_index == 3U ? 32768U : period;
        for (std::size_t position = period + 512U; position < block_bytes; position += patch_stride) {
            input[start + position] ^= std::byte{0xFF};
        }
    }

    flitzzip::GpuCodecOptions options;
    options.block_size = static_cast<std::uint32_t>(block_bytes);
    options.compression_level = 5;
    options.require_gpu = true;
    options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
    const auto encoded = flitzzip::encode_chunk(input, options);
    REQUIRE_TRUE(encoded.gpu_used);
    REQUIRE_EQ(encoded.blocks.size(), 4U);
    REQUIRE_EQ(encoded.blocks[0].kind, flitzzip::BlockKind::GpuSparsePattern);
    REQUIRE_EQ(encoded.blocks[1].kind, flitzzip::BlockKind::Fill);
    REQUIRE_EQ(encoded.blocks[2].kind, flitzzip::BlockKind::GpuSparsePattern);
    REQUIRE_EQ(encoded.blocks[3].kind, flitzzip::BlockKind::GpuSparsePattern);
    REQUIRE_EQ(flitzzip::snapshot_gpu_telemetry(*options.telemetry).sparse_pattern_blocks, 3U);
    std::vector<std::byte> decoded(input.size());
    REQUIRE_TRUE(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
    REQUIRE_EQ(decoded, input);
    options.require_gpu = false;
    options.force_cpu = true;
    std::fill(decoded.begin(), decoded.end(), std::byte{0});
    REQUIRE_TRUE(!flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
    REQUIRE_EQ(decoded, input);
}

// Purpose: Reject caller-provided gather offsets before they reach HIP allocation or launch.
// Inputs: A candidate with valid source bounds but nonzero output-only fields.
// Outputs: HIP builds reject both forgeries before borrowing input; CPU builds confirm backend unavailability.
TEST_CASE(sparse_pattern_hip_batch_rejects_prepopulated_output_fields) {
#if FLITZZIP_ENABLE_HIP
    std::array<std::byte, 32U> input{};
    std::array<flitzzip::sparse_pattern::SparseCandidate, 1U> candidates{{{
        .source_offset = 0U,
        .input_bytes = 32U,
        .period = 2U,
        .max_patches = 1U,
        .positions_offset = 1U,
    }}};
    for (const bool forge_count : {false, true}) {
        candidates[0].positions_offset = forge_count ? 0U : 1U;
        candidates[0].patch_count = forge_count ? 1U : 0U;
        bool rejected = false;
        try {
            (void)flitzzip::sparse_pattern::collect_positions_device_batch(
                input.data(), static_cast<std::uint32_t>(input.size()), candidates, nullptr);
        } catch (const flitzzip::GpuError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

// Purpose: Enforce the sparse replacement boundary independently from HIP candidate generation.
// Inputs: A two-block baseline, one smaller canonical payload, and forged count/extent/cost variants.
// Outputs: HIP builds enforce replacement invariants; CPU-only builds confirm the backend is unavailable.
TEST_CASE(sparse_pattern_replacements_validate_complete_internal_results) {
#if FLITZZIP_ENABLE_HIP
    flitzzip::EncodedChunk baseline;
    baseline.blocks = {
        {.kind = flitzzip::BlockKind::Raw, .uncompressed_len = 32U, .encoded_offset = 0U, .encoded_len = 8U},
        {.kind = flitzzip::BlockKind::GpuPrefix, .uncompressed_len = 16U, .encoded_offset = 8U, .encoded_len = 5U},
    };
    baseline.payload = {std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14},
                        std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x20}, std::byte{0x21},
                        std::byte{0x22}, std::byte{0x23}, std::byte{0x24}};
    baseline.source_crc32 = 0xA1B2C3D4U;
    baseline.source_crc32_available = true;
    baseline.gpu_used = true;

    auto sparse = make_sparse_test_payload();
    REQUIRE_TRUE(sparse.size() < baseline.blocks.front().uncompressed_len);
    baseline.blocks.front().encoded_len = static_cast<std::uint32_t>(sparse.size() + 1U);
    baseline.blocks[1].encoded_offset = baseline.blocks.front().encoded_len;
    baseline.payload.resize(static_cast<std::size_t>(baseline.blocks.front().encoded_len) + 5U, std::byte{0x17});
    const auto retained =
        std::vector<std::byte>(baseline.payload.begin() + baseline.blocks[1].encoded_offset, baseline.payload.end());
    const flitzzip::sparse_pattern::SparseReplacements replacements{sparse, {}};
    const auto result = flitzzip::sparse_pattern::apply_replacements(baseline, replacements);
    REQUIRE_EQ(result.source_crc32, baseline.source_crc32);
    REQUIRE_TRUE(result.source_crc32_available && result.gpu_used);
    REQUIRE_EQ(result.blocks.size(), 2U);
    REQUIRE_EQ(result.blocks[0].kind, flitzzip::BlockKind::GpuSparsePattern);
    REQUIRE_EQ(result.blocks[0].encoded_offset, 0U);
    REQUIRE_EQ(result.blocks[0].encoded_len, sparse.size());
    REQUIRE_EQ(result.blocks[1].kind, flitzzip::BlockKind::GpuPrefix);
    REQUIRE_EQ(result.blocks[1].encoded_offset, sparse.size());
    REQUIRE_EQ(std::vector<std::byte>(result.payload.begin(), result.payload.begin() + sparse.size()), sparse);
    REQUIRE_EQ(std::vector<std::byte>(result.payload.begin() + sparse.size(), result.payload.end()), retained);

    const auto require_rejected = [](flitzzip::EncodedChunk chunk,
                                     flitzzip::sparse_pattern::SparseReplacements candidates,
                                     std::string_view expected) {
        bool rejected = false;
        try {
            (void)flitzzip::sparse_pattern::apply_replacements(std::move(chunk), candidates);
        } catch (const flitzzip::GpuError& error) {
            rejected = std::string_view(error.what()) == expected;
        }
        REQUIRE_TRUE(rejected);
    };
    require_rejected(baseline, {sparse}, "sparse replacement count differs from baseline blocks");
    auto invalid_extent = baseline;
    invalid_extent.blocks[0].encoded_offset = invalid_extent.payload.size() + 1U;
    require_rejected(invalid_extent, replacements, "sparse baseline block exceeds encoded payload");
    invalid_extent = baseline;
    invalid_extent.blocks[0].encoded_len = static_cast<std::uint32_t>(invalid_extent.payload.size() + 1U);
    require_rejected(invalid_extent, replacements, "sparse baseline block exceeds encoded payload");
    auto non_improving = sparse;
    non_improving.resize(baseline.blocks[0].encoded_len, std::byte{0x55});
    require_rejected(baseline, {non_improving, {}}, "sparse replacement does not improve the source block");
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

// Purpose: Reject sparse candidate geometry before any device query, allocation, or kernel work.
// Inputs: Null device input, an overlong descriptor, and descriptors that leave an uncovered source suffix.
// Outputs: Each malformed call returns its precise GpuError in HIP builds, without requiring a live device.
TEST_CASE(sparse_pattern_candidate_layout_precedes_device_work) {
#if FLITZZIP_ENABLE_HIP
    const std::array<std::byte, 8U> input{};
    const auto require_rejected = [&](const std::byte* device, std::vector<flitzzip::BlockDescriptor> blocks,
                                      std::string_view expected) {
        bool rejected = false;
        try {
            (void)flitzzip::sparse_pattern::select_replacements(input, device, blocks, nullptr);
        } catch (const flitzzip::GpuError& error) {
            rejected = std::string_view(error.what()) == expected;
        }
        REQUIRE_TRUE(rejected);
    };
    require_rejected(nullptr, {}, "sparse candidate device input is unavailable");
    require_rejected(input.data(), {{.uncompressed_len = 9U}}, "sparse candidate block exceeds source chunk");
    require_rejected(input.data(), {{.uncompressed_len = 7U}}, "sparse candidate blocks do not cover source chunk");
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}
