#include "core/sparse_pattern_block.hpp"
#include "gpu/gpu_codec.hpp"
#if SUPERZIP_ENABLE_HIP
#include "gpu/sparse_pattern_device.hpp"
#endif
#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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
                             std::uint32_t max_period = superzip::kMaxGpuPatternBytes) {
    bool rejected = false;
    try {
        (void)superzip::parse_sparse_pattern_block(payload, decoded_size, max_period);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

}  // namespace

// Purpose: Admit a canonical sparse block and preserve exact borrowed motif/patch spans.
// Inputs: A 64-byte decoded extent with two strictly increasing nonredundant patches.
// Outputs: Requires the expected period and patch count without copying the payload.
TEST_CASE(sparse_pattern_block_admits_canonical_payload) {
    const auto bytes = make_sparse_test_payload();
    const auto layout = superzip::parse_sparse_pattern_block(bytes, 64U);
    REQUIRE_EQ(layout.motif.size(), 4U);
    REQUIRE_EQ(layout.patch_count, 2U);
    REQUIRE_EQ(layout.patches.size(), 2U * superzip::kSparsePatternPatchBytes);
    REQUIRE_EQ(layout.motif.front(), std::byte{'A'});
    REQUIRE_EQ(superzip::read_sparse_u32(layout.patches, 0U), 5U);
    REQUIRE_EQ(superzip::read_sparse_u32(layout.patches, superzip::kSparsePatternPatchBytes), 11U);
}

// Purpose: Reject sparse blocks whose metadata could cause partial, ambiguous, or out-of-bounds decode.
// Inputs: Truncation, false sizes, noncanonical periods, patch counts, positions, values, and trailing bytes.
// Outputs: Every mutation fails with `ArchiveError` before materialization.
TEST_CASE(sparse_pattern_block_rejects_malformed_payload) {
    const auto valid = make_sparse_test_payload();
    require_sparse_rejected(std::span(valid).first(7U), 64U);
    require_sparse_rejected(valid, 0U);
    require_sparse_rejected(valid, superzip::kMaxArchiveBlockBytes + 1U);
    require_sparse_rejected(valid, static_cast<std::uint32_t>(valid.size()));

    auto invalid = valid;
    replace_sparse_test_u32(invalid, 0U, 1U);
    require_sparse_rejected(invalid, 64U);
    invalid = valid;
    replace_sparse_test_u32(invalid, 0U, superzip::kMaxGpuPatternBytes + 1U);
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
    for (const auto period : {superzip::kMaxGpuPatternBytes, superzip::kMaxGpuPatternBytes + 1U,
                              superzip::kMaxGpuLongSparsePatternBytes, superzip::kMaxGpuLongSparsePatternBytes + 1U}) {
        std::vector<std::byte> payload;
        append_sparse_test_u32(payload, period);
        append_sparse_test_u32(payload, 1U);
        payload.resize(superzip::kSparsePatternHeaderBytes + period, std::byte{0});
        append_sparse_test_u32(payload, period);
        payload.push_back(std::byte{1});
        const auto decoded_size = 2U * period + 1U;
        if (period <= superzip::kMaxGpuPatternBytes) {
            REQUIRE_EQ(superzip::parse_sparse_pattern_block(payload, decoded_size).motif.size(), period);
        } else {
            require_sparse_rejected(payload, decoded_size);
        }
        if (period > superzip::kMaxGpuPatternBytes && period <= superzip::kMaxGpuLongSparsePatternBytes) {
            REQUIRE_EQ(
                superzip::parse_sparse_pattern_block(payload, decoded_size, superzip::kMaxGpuLongSparsePatternBytes)
                    .motif.size(),
                period);
        } else {
            require_sparse_rejected(payload, decoded_size, superzip::kMaxGpuLongSparsePatternBytes);
        }
    }
}

// Purpose: Exercise the new GPU-native long-sparse kind through independent CPU and HIP decoding.
// Inputs: A seeded 128 KiB motif repeated twice with one changed byte in a 256 KiB block.
// Outputs: Requires exact payload size, v7 block kind, GPU telemetry, and byte-exact CPU/HIP roundtrips.
TEST_CASE(long_sparse_pattern_required_hip_roundtrip) {
    if (!superzip::query_gpu_info().available) {
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

    superzip::GpuCodecOptions options;
    options.block_size = static_cast<std::uint32_t>(input.size());
    options.compression_level = 5;
    options.require_gpu = true;
    options.telemetry = std::make_shared<superzip::GpuTelemetry>();
    const auto encoded = superzip::encode_chunk(input, options);
    REQUIRE_TRUE(encoded.gpu_used);
    REQUIRE_EQ(encoded.blocks.size(), 1U);
    REQUIRE_EQ(encoded.blocks[0].kind, superzip::BlockKind::GpuLongSparsePattern);
    REQUIRE_EQ(encoded.payload.size(),
               superzip::kSparsePatternHeaderBytes + period + superzip::kSparsePatternPatchBytes);
    REQUIRE_EQ(superzip::snapshot_gpu_telemetry(*options.telemetry).sparse_pattern_blocks, 1U);
    std::vector<std::byte> decoded(input.size());
    REQUIRE_TRUE(superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
    REQUIRE_EQ(decoded, input);
    options.require_gpu = false;
    options.force_cpu = true;
    std::fill(decoded.begin(), decoded.end(), std::byte{0});
    REQUIRE_TRUE(!superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
    REQUIRE_EQ(decoded, input);

    auto corrupt = encoded.payload;
    corrupt[superzip::kSparsePatternHeaderBytes + period + sizeof(std::uint32_t)] = input[1024U];
    bool rejected = false;
    try {
        (void)superzip::decode_chunk(corrupt, encoded.blocks, decoded, options);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify required-HIP sparse selection, telemetry, and both independent decode paths.
// Inputs: A seeded 4 KiB motif with one correction in each later record of a 1 MiB block.
// Outputs: Requires a complete smaller sparse payload, exact CPU/HIP decode, and one reported sparse block.
TEST_CASE(sparse_pattern_required_hip_encode_roundtrip) {
    if (!superzip::query_gpu_info().available) {
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

    superzip::GpuCodecOptions options;
    options.block_size = static_cast<std::uint32_t>(block_bytes);
    options.compression_level = 5;
    options.require_gpu = true;
    options.telemetry = std::make_shared<superzip::GpuTelemetry>();
    const auto encoded = superzip::encode_chunk(input, options);
    REQUIRE_TRUE(encoded.gpu_used);
    REQUIRE_EQ(encoded.blocks.size(), 1U);
    REQUIRE_EQ(encoded.blocks[0].kind, superzip::BlockKind::GpuSparsePattern);
    const auto layout = superzip::parse_sparse_pattern_block(encoded.payload, static_cast<std::uint32_t>(block_bytes));
    REQUIRE_EQ(layout.motif.size(), motif_bytes);
    REQUIRE_EQ(layout.patch_count, 255U);
    REQUIRE_EQ(encoded.payload.size(), superzip::kSparsePatternHeaderBytes + motif_bytes +
                                           layout.patch_count * superzip::kSparsePatternPatchBytes);
    REQUIRE_EQ(superzip::snapshot_gpu_telemetry(*options.telemetry).sparse_pattern_blocks, 1U);

    std::vector<std::byte> decoded(input.size());
    REQUIRE_TRUE(superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
    REQUIRE_EQ(decoded, input);
    options.require_gpu = false;
    options.force_cpu = true;
    std::fill(decoded.begin(), decoded.end(), std::byte{0});
    REQUIRE_TRUE(!superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
    REQUIRE_EQ(decoded, input);

    options.force_cpu = false;
    options.telemetry = std::make_shared<superzip::GpuTelemetry>();
    const auto optional = superzip::encode_chunk(input, options);
    REQUIRE_TRUE(optional.gpu_used);
    REQUIRE_EQ(optional.payload, encoded.payload);
    REQUIRE_EQ(superzip::snapshot_gpu_telemetry(*options.telemetry).sparse_pattern_blocks, 1U);

    options.require_gpu = true;
    options.compression_level = 9;
    options.telemetry = std::make_shared<superzip::GpuTelemetry>();
    const auto high_effort = superzip::encode_chunk(input, options);
    REQUIRE_TRUE(high_effort.gpu_used);
    REQUIRE_TRUE(high_effort.payload.size() <= encoded.payload.size());
    std::fill(decoded.begin(), decoded.end(), std::byte{0});
    REQUIRE_TRUE(superzip::decode_chunk(high_effort.payload, high_effort.blocks, decoded, options));
    REQUIRE_EQ(decoded, input);
}

// Purpose: Cover batched sparse collection when admitted blocks are separated by a noncandidate block.
// Inputs: Three sparse MiB blocks with divisible, nondivisible, and short motif periods, separated by a fill block.
// Outputs: Requires exact candidate-to-block mapping, three HIP sparse blocks, and byte-exact CPU/HIP decode.
TEST_CASE(sparse_pattern_hip_batch_preserves_block_offsets) {
    if (!superzip::query_gpu_info().available) {
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

    superzip::GpuCodecOptions options;
    options.block_size = static_cast<std::uint32_t>(block_bytes);
    options.compression_level = 5;
    options.require_gpu = true;
    options.telemetry = std::make_shared<superzip::GpuTelemetry>();
    const auto encoded = superzip::encode_chunk(input, options);
    REQUIRE_TRUE(encoded.gpu_used);
    REQUIRE_EQ(encoded.blocks.size(), 4U);
    REQUIRE_EQ(encoded.blocks[0].kind, superzip::BlockKind::GpuSparsePattern);
    REQUIRE_EQ(encoded.blocks[1].kind, superzip::BlockKind::Fill);
    REQUIRE_EQ(encoded.blocks[2].kind, superzip::BlockKind::GpuSparsePattern);
    REQUIRE_EQ(encoded.blocks[3].kind, superzip::BlockKind::GpuSparsePattern);
    REQUIRE_EQ(superzip::snapshot_gpu_telemetry(*options.telemetry).sparse_pattern_blocks, 3U);
    std::vector<std::byte> decoded(input.size());
    REQUIRE_TRUE(superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
    REQUIRE_EQ(decoded, input);
    options.require_gpu = false;
    options.force_cpu = true;
    std::fill(decoded.begin(), decoded.end(), std::byte{0});
    REQUIRE_TRUE(!superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
    REQUIRE_EQ(decoded, input);
}

#if SUPERZIP_ENABLE_HIP
// Purpose: Reject caller-provided gather offsets before they reach HIP allocation or launch.
// Inputs: A candidate with valid source bounds but nonzero output-only fields.
// Outputs: Both forged offset and patch count fail with `GpuError` without touching the borrowed pointer.
TEST_CASE(sparse_pattern_hip_batch_rejects_prepopulated_output_fields) {
    std::array<std::byte, 32U> input{};
    std::array<superzip::sparse_pattern::SparseCandidate, 1U> candidates{{{
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
            (void)superzip::sparse_pattern::collect_positions_device_batch(
                input.data(), static_cast<std::uint32_t>(input.size()), candidates, nullptr);
        } catch (const superzip::GpuError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}
#endif
