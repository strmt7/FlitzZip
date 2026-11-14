#include "test_util.hpp"
#include "test_flzip_helpers.hpp"

#include "core/archive_encode_batch.hpp"
#include "core/checksum.hpp"
#include "core/result.hpp"
#include "gpu/gpu_codec.hpp"
#include "zstd/zstd_runtime.hpp"
#include "miniz.h"

#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>

namespace {

std::uint32_t next_fixture_random(std::uint32_t& state) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

// Purpose: Generate unequal independent blocks covering codec and CRC boundaries without filesystem I/O.
// Inputs: lengths select positive block sizes; each position selects a deterministic content family.
// Outputs: Returns dense fill, pattern, prefix, random, and misleading sampled-pattern bytes.
std::vector<std::byte> batch_fixture(std::span<const std::uint32_t> lengths) {
    std::vector<std::byte> input;
    std::uint32_t random = 0x98765432U;
    for (std::size_t block = 0; block < lengths.size(); ++block) {
        for (std::uint32_t i = 0; i < lengths[block]; ++i) {
            next_fixture_random(random);
            const auto family = block % 6U;
            const auto value = family == 0   ? 73U
                               : family == 1 ? i % 7U
                               : family == 2 ? random % 5U
                               : family == 3 ? 193U + random % 5U
                               : family == 4 ? random % 256U
                                             : (i < 8192U ? i % 3U : random % 256U);
            input.push_back(static_cast<std::byte>(value));
        }
    }
    return input;
}

// Purpose: Compare batched descriptors, bytes, and checksums with separately encoded independent blocks.
// Inputs: lengths and options select bounded RAM fixtures and an explicit CPU or required-HIP backend.
// Outputs: Requires exact encoding identity and byte-exact decoding through every applicable backend.
void check_batch_input_identity(const std::vector<std::byte>& input, std::span<const std::uint32_t> lengths,
                                flitzzip::GpuCodecOptions options) {
    auto owned_input = input;
    const auto* input_allocation = owned_input.data();
    const auto input_capacity = owned_input.capacity();
    const auto batch = flitzzip::encode_owned_block_batch(std::move(owned_input), lengths, options);
    REQUIRE_EQ(batch.encoded.blocks.size(), lengths.size());
    REQUIRE_EQ(batch.block_crc32.size(), lengths.size());
    REQUIRE_TRUE(batch.encoded.source_crc32_available);
    REQUIRE_EQ(batch.encoded.source_crc32, flitzzip::crc32(input));
    REQUIRE_EQ(batch.encoded.gpu_used, !options.force_cpu && !input.empty());
    std::size_t source_offset = 0;
    std::size_t payload_offset = 0;
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        const auto source = std::span(input).subspan(source_offset, lengths[i]);
        const auto separate =
            flitzzip::encode_owned_chunk(std::vector<std::byte>(source.begin(), source.end()), options);
        REQUIRE_EQ(separate.blocks.size(), 1U);
        const auto& expected = separate.blocks.front();
        const auto& actual = batch.encoded.blocks[i];
        REQUIRE_EQ(actual.kind, expected.kind);
        REQUIRE_EQ(actual.fill_value, expected.fill_value);
        REQUIRE_EQ(actual.uncompressed_len, expected.uncompressed_len);
        REQUIRE_EQ(actual.encoded_len, expected.encoded_len);
        REQUIRE_EQ(actual.encoded_offset, payload_offset);
        const auto payload = std::span(batch.encoded.payload).subspan(payload_offset, actual.encoded_len);
        REQUIRE_TRUE(std::ranges::equal(payload, separate.payload));
        REQUIRE_EQ(batch.block_crc32[i], flitzzip::crc32(source));
        REQUIRE_EQ(batch.block_crc32[i], separate.source_crc32);
        source_offset += lengths[i];
        payload_offset += actual.encoded_len;
    }
    REQUIRE_EQ(payload_offset, batch.encoded.payload.size());
    std::vector<std::byte> decoded(input.size());
    if (!options.force_cpu) {
        flitzzip::decode_chunk(batch.encoded.payload, batch.encoded.blocks, decoded, options);
        REQUIRE_TRUE(decoded == input);
    }
    options.require_gpu = false;
    options.force_cpu = true;
    flitzzip::decode_chunk(batch.encoded.payload, batch.encoded.blocks, decoded, options);
    REQUIRE_TRUE(decoded == input);
    if (!batch.encoded.gpu_used && !batch.encoded.payload.empty()) {
        // A completed block never grows, so CPU output can compact into its owned
        // input without another chunk allocation. Check after byte/CRC readback.
        REQUIRE_TRUE(batch.encoded.payload.data() == input_allocation);
        REQUIRE_EQ(batch.encoded.payload.capacity(), input_capacity);
    }
}

void check_batch_identity(std::span<const std::uint32_t> lengths, flitzzip::GpuCodecOptions options) {
    check_batch_input_identity(batch_fixture(lengths), lengths, options);
}

// Purpose: Reject invalid layouts before any GPU launch or transfer.
// Inputs: size and lengths describe one intentionally invalid independent-block layout.
// Outputs: Requires ArchiveError and unchanged dispatch counters.
void require_invalid_batch(std::size_t size, std::span<const std::uint32_t> lengths) {
    flitzzip::GpuCodecOptions options;
    options.block_size = flitzzip::kMinArchiveBlockBytes;
    options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
    bool rejected = false;
    try {
        (void)flitzzip::encode_owned_block_batch(std::vector<std::byte>(size), lengths, options);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    const auto counters = flitzzip::snapshot_gpu_telemetry(*options.telemetry);
    REQUIRE_EQ(counters.encode_chunks, 0U);
    REQUIRE_EQ(counters.kernel_launches, 0U);
    REQUIRE_EQ(counters.h2d_bytes, 0U);
}

// Compare the new ownership boundary with the unchanged borrowed encoder,
// including complete descriptors, source lifetime, checksums and CPU readback.
flitzzip::EncodedChunk check_owned_cpu_identity(std::span<const std::byte> input,
                                                const flitzzip::ArchiveCodecOptions& options) {
    const std::vector<std::byte> original(input.begin(), input.end());
    const auto borrowed = flitzzip::encode_chunk_cpu(input, options);
    const auto owned = flitzzip::encode_owned_chunk_cpu(original, options);
    REQUIRE_TRUE(std::ranges::equal(input, original));
    REQUIRE_EQ(owned.payload, borrowed.payload);
    REQUIRE_EQ(owned.blocks.size(), borrowed.blocks.size());
    REQUIRE_TRUE(!borrowed.source_crc32_available);
    REQUIRE_TRUE(owned.source_crc32_available);
    REQUIRE_EQ(owned.source_crc32, flitzzip::crc32(original));
    REQUIRE_TRUE(!owned.gpu_used);
    for (std::size_t index = 0; index < owned.blocks.size(); ++index) {
        const auto& actual = owned.blocks[index];
        const auto& expected = borrowed.blocks[index];
        REQUIRE_EQ(actual.kind, expected.kind);
        REQUIRE_EQ(actual.fill_value, expected.fill_value);
        REQUIRE_EQ(actual.uncompressed_len, expected.uncompressed_len);
        REQUIRE_EQ(actual.encoded_offset, expected.encoded_offset);
        REQUIRE_EQ(actual.encoded_len, expected.encoded_len);
    }
    std::vector<std::byte> decoded(input.size(), std::byte{0xCD});
    flitzzip::decode_chunk_cpu(owned.payload, owned.blocks, decoded, options);
    REQUIRE_EQ(decoded, original);
    return owned;
}

// Restore the complete frame through the codec API, without the native FLZIP
// materializer or assembler. Expected bytes come from the original source span.
void require_independent_cpu_frame(std::span<const std::byte> frame, std::span<const std::byte> expected,
                                   flitzzip::BlockKind kind) {
    std::vector<std::byte> decoded(expected.size(), std::byte{0xCD});
    if (kind == flitzzip::BlockKind::Deflate) {
        auto written = static_cast<mz_ulong>(decoded.size());
        auto consumed = static_cast<mz_ulong>(frame.size());
        REQUIRE_EQ(mz_uncompress2(reinterpret_cast<unsigned char*>(decoded.data()), &written,
                                  reinterpret_cast<const unsigned char*>(frame.data()), &consumed),
                   MZ_OK);
        REQUIRE_EQ(written, expected.size());
        REQUIRE_EQ(consumed, frame.size());
    } else {
        REQUIRE_TRUE(kind == flitzzip::BlockKind::CpuZstd);
        const auto& zstd = flitzzip::zstd_runtime();
        const auto written = zstd.decompress_block(decoded.data(), decoded.size(), frame.data(), frame.size());
        REQUIRE_TRUE(!zstd.is_error(written));
        REQUIRE_EQ(written, expected.size());
    }
    REQUIRE_TRUE(std::ranges::equal(decoded, expected));
}

}  // namespace

// Purpose: Preserve per-block CPU codec policy and CRCs at all nine effort settings.
// Inputs: Unequal RAM-only blocks including segment tails, prefix alphabets, fill, and raw data.
// Outputs: Requires identical payloads to separate encoding and exact CPU decode.
TEST_CASE(gpu_block_batch_cpu_identity) {
    constexpr std::array<std::uint32_t, 12> lengths{1, 31,    4095,  4096,   4097,   65537,
                                                    2, 65535, 65536, 131073, 262144, 65539};
    for (int level = 1; level <= 9; ++level) {
        check_batch_identity(
            lengths, {.require_gpu = false, .force_cpu = true, .block_size = 256U * 1024U, .compression_level = level});
    }
}

// The owned CPU path must transfer an all-raw allocation instead of making a second
// chunk-sized copy. Pointer identity proves ownership transfer without timing.
TEST_CASE(gpu_block_batch_cpu_owned_raw_reuses_source) {
    constexpr auto block_size = flitzzip::kMinArchiveBlockBytes;
    std::vector<std::byte> input(8U * block_size + 127U);
    std::uint32_t random = 0x37A129D5U;
    for (auto& value : input) {
        value = static_cast<std::byte>(next_fixture_random(random) & 255U);
    }
    const auto original = input;
    const auto expected_crc = flitzzip::crc32(original);
    for (const auto workers : {1U, 3U, 64U}) {
        auto owned = original;
        owned.reserve(owned.size() + block_size);
        const auto* allocation = owned.data();
        const auto capacity = owned.capacity();
        const auto encoded = flitzzip::encode_owned_chunk(
            std::move(owned),
            {.require_gpu = false, .force_cpu = true, .block_size = block_size, .worker_count = workers});
        REQUIRE_TRUE(std::ranges::all_of(encoded.blocks,
                                         [](const auto& block) { return block.kind == flitzzip::BlockKind::Raw; }));
        REQUIRE_TRUE(encoded.payload.data() == allocation);
        REQUIRE_EQ(encoded.payload.capacity(), capacity);
        REQUIRE_EQ(encoded.payload, original);
        REQUIRE_TRUE(encoded.source_crc32_available);
        REQUIRE_EQ(encoded.source_crc32, expected_crc);
        REQUIRE_TRUE(!encoded.gpu_used);
    }
}

TEST_CASE(gpu_block_batch_cpu_owned_representation_identity) {
    constexpr auto block_size = flitzzip::kMinArchiveBlockBytes;
    constexpr std::array<std::uint32_t, 6> lengths{block_size, block_size, block_size, block_size, block_size, 127U};
    const auto mixed = batch_fixture(lengths);
    const std::vector<std::byte> fill(2U * block_size + 1U, std::byte{0xA7});
    for (const auto effort : {1, 5, 9}) {
        for (const auto workers : {0U, 3U, 64U}) {
            const flitzzip::ArchiveCodecOptions options{
                .block_size = block_size, .worker_count = workers, .compression_level = effort};
            const auto empty = check_owned_cpu_identity({}, options);
            REQUIRE_TRUE(empty.blocks.empty() && empty.payload.empty());
            REQUIRE_EQ(empty.source_crc32, 0U);
            const auto filled = check_owned_cpu_identity(fill, options);
            REQUIRE_TRUE(filled.payload.empty());
            REQUIRE_EQ(filled.blocks.size(), 3U);
            const auto encoded = check_owned_cpu_identity(mixed, options);
            for (const auto kind : {flitzzip::BlockKind::Fill, flitzzip::BlockKind::Raw, flitzzip::BlockKind::CpuZstd,
                                    flitzzip::BlockKind::Deflate}) {
                REQUIRE_TRUE(
                    std::ranges::any_of(encoded.blocks, [kind](const auto& block) { return block.kind == kind; }));
            }
        }
    }
}

// A sole complete frame can be moved even between fill blocks. Two frames must
// still be aggregated. The independent decoders reject dropped/overwritten bytes;
// no timing, allocation-profiler or alternative compression policy is involved.
TEST_CASE(gpu_block_batch_cpu_complete_frames_survive_payload_handoff) {
    constexpr auto block_size = flitzzip::kMinArchiveBlockBytes;
    struct Layout {
        std::uint32_t prefix_fills;
        std::uint32_t frames;
        std::uint32_t frame_bytes;
        std::uint32_t suffix_fills;
    };
    constexpr std::array<Layout, 6> layouts{{{0U, 1U, 127U, 0U},
                                             {2U, 1U, 127U, 0U},
                                             {0U, 1U, block_size, 0U},
                                             {1U, 1U, block_size, 1U},
                                             {2U, 1U, block_size, 0U},
                                             {1U, 2U, block_size, 1U}}};
    for (const auto layout : layouts) {
        std::vector<std::byte> source(layout.prefix_fills * block_size, std::byte{0xA7});
        for (std::uint32_t frame = 0; frame < layout.frames; ++frame) {
            for (std::uint32_t index = 0; index < layout.frame_bytes; ++index) {
                source.push_back(static_cast<std::byte>((index + frame) % 7U));
            }
        }
        source.insert(source.end(), layout.suffix_fills * block_size, std::byte{0xB9});
        const auto expected_kind =
            layout.frame_bytes < block_size ? flitzzip::BlockKind::Deflate : flitzzip::BlockKind::CpuZstd;
        for (int effort = 1; effort <= 9; ++effort) {
            for (const auto workers : {1U, 3U}) {
                const auto encoded = check_owned_cpu_identity(
                    source, {.block_size = block_size, .worker_count = workers, .compression_level = effort});
                REQUIRE_EQ(encoded.blocks.size(), layout.prefix_fills + layout.frames + layout.suffix_fills);
                std::size_t source_offset = 0U;
                std::size_t payload_offset = 0U;
                for (std::size_t index = 0U; index < encoded.blocks.size(); ++index) {
                    const auto& block = encoded.blocks[index];
                    const bool frame = index >= layout.prefix_fills && index < layout.prefix_fills + layout.frames;
                    REQUIRE_EQ(block.kind, frame ? expected_kind : flitzzip::BlockKind::Fill);
                    const auto length = frame ? layout.frame_bytes : block_size;
                    REQUIRE_EQ(block.uncompressed_len, length);
                    if (frame) {
                        REQUIRE_EQ(block.encoded_offset, payload_offset);
                        REQUIRE_TRUE(block.encoded_len > 0U && block.encoded_len < length);
                        require_independent_cpu_frame(
                            std::span(encoded.payload).subspan(payload_offset, block.encoded_len),
                            std::span(source).subspan(source_offset, length), expected_kind);
                        payload_offset += block.encoded_len;
                    } else {
                        REQUIRE_EQ(block.encoded_len, 0U);
                    }
                    source_offset += length;
                }
                REQUIRE_EQ(payload_offset, encoded.payload.size());
                REQUIRE_EQ(source_offset, source.size());
            }
        }
    }
}

TEST_CASE(gpu_block_batch_cpu_owned_validates_empty_options) {
    for (const auto options : {flitzzip::ArchiveCodecOptions{.block_size = 0U},
                               flitzzip::ArchiveCodecOptions{.block_size = flitzzip::kMaxArchiveBlockBytes + 1U},
                               flitzzip::ArchiveCodecOptions{.compression_level = 0},
                               flitzzip::ArchiveCodecOptions{.compression_level = 10}}) {
        bool rejected = false;
        try {
            (void)flitzzip::encode_owned_chunk_cpu({}, options);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// These controls observe storage ownership, not allocation timing or throughput.
TEST_CASE(gpu_block_batch_cpu_raw_batch_reuses_source) {
    constexpr std::array<std::uint32_t, 5> lengths{32768U, 4096U, 31U, 8192U, 127U};
    const flitzzip::GpuCodecOptions options{
        .require_gpu = false, .force_cpu = true, .block_size = 65536U, .compression_level = 5};
    std::vector<std::byte> input(45214U);
    std::uint32_t random = 0x71C29A53U;
    for (auto& value : input) {
        value = static_cast<std::byte>(next_fixture_random(random) & 255U);
    }
    check_batch_input_identity(input, lengths, options);
    const auto original = input;
    input.reserve(input.size() + 4096U);
    const auto* allocation = input.data();
    const auto capacity = input.capacity();
    const auto batch = flitzzip::encode_owned_block_batch(std::move(input), lengths, options);
    REQUIRE_TRUE(std::ranges::all_of(batch.encoded.blocks,
                                     [](const auto& block) { return block.kind == flitzzip::BlockKind::Raw; }));
    REQUIRE_TRUE(batch.encoded.payload.data() == allocation);
    REQUIRE_EQ(batch.encoded.payload.capacity(), capacity);
    REQUIRE_EQ(batch.encoded.payload, original);
}

TEST_CASE(gpu_block_batch_cpu_fill_batch_has_no_payload_allocation) {
    constexpr std::array<std::uint32_t, 4> lengths{1U, 31U, 4096U, 8192U};
    const std::vector<std::byte> input(12320U, std::byte{0xA7});
    const auto batch = flitzzip::encode_owned_block_batch(
        input, lengths, {.require_gpu = false, .force_cpu = true, .block_size = 8192U});
    REQUIRE_EQ(batch.encoded.blocks.size(), lengths.size());
    REQUIRE_TRUE(std::ranges::all_of(batch.encoded.blocks,
                                     [](const auto& block) { return block.kind == flitzzip::BlockKind::Fill; }));
    REQUIRE_TRUE(batch.encoded.payload.empty());
    REQUIRE_EQ(batch.encoded.payload.capacity(), 0U);
    REQUIRE_TRUE(batch.encoded.source_crc32_available);
    REQUIRE_EQ(batch.encoded.source_crc32, flitzzip::crc32(input));
}

TEST_CASE(gpu_block_batch_cpu_reused_state_preserves_variable_frames) {
    constexpr std::array<std::uint32_t, 6> source_lengths{4096U, 4095U, 8192U, 32768U, 16384U, 31U};
    constexpr std::array<std::array<std::size_t, 7>, 3> orders{{
        {4U, 4U, 0U, 2U, 1U, 5U, 3U},
        {0U, 4U, 1U, 2U, 5U, 4U, 3U},
        {2U, 0U, 4U, 1U, 5U, 3U, 4U},
    }};
    const auto source = batch_fixture(source_lengths);
    std::array<std::size_t, source_lengths.size()> offsets{};
    for (std::size_t index = 1; index < offsets.size(); ++index) {
        offsets[index] = offsets[index - 1U] + source_lengths[index - 1U];
    }
    for (const auto& order : orders) {
        std::vector<std::byte> input;
        std::vector<std::uint32_t> lengths;
        for (const auto index : order) {
            const auto part = std::span(source).subspan(offsets[index], source_lengths[index]);
            input.insert(input.end(), part.begin(), part.end());
            lengths.push_back(source_lengths[index]);
        }
        for (int effort = 1; effort <= 9; ++effort) {
            check_batch_input_identity(
                input, lengths,
                {.require_gpu = false, .force_cpu = true, .block_size = 32768U, .compression_level = effort});
        }
    }
}

TEST_CASE(gpu_block_batch_cpu_direct_layout_and_recovery) {
    const auto empty = flitzzip::encode_owned_block_batch_cpu({}, {}, {});
    REQUIRE_TRUE(empty.encoded.blocks.empty() && empty.encoded.payload.empty() && empty.block_crc32.empty());
    REQUIRE_TRUE(empty.encoded.source_crc32_available);
    REQUIRE_EQ(empty.encoded.source_crc32, 0U);
    const std::vector<std::byte> input(4U, std::byte{0xA7});
    for (const auto& lengths :
         {std::vector<std::uint32_t>{}, std::vector<std::uint32_t>{0U, 4U}, std::vector<std::uint32_t>{5U},
          std::vector<std::uint32_t>{1U, 2U}, std::vector<std::uint32_t>{4U, 1U}}) {
        bool rejected = false;
        try {
            (void)flitzzip::encode_owned_block_batch_cpu(input, lengths, {});
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_TRUE(std::ranges::all_of(input, [](const auto value) { return value == std::byte{0xA7}; }));
        constexpr std::array<std::uint32_t, 2> valid_lengths{2U, 2U};
        const auto valid = flitzzip::encode_owned_block_batch_cpu(input, valid_lengths, {});
        REQUIRE_EQ(valid.encoded.blocks.size(), 2U);
        REQUIRE_EQ(valid.encoded.source_crc32, flitzzip::crc32(input));
    }
    for (const auto count : {flitzzip::kMaxEncodeBatchBlocks, flitzzip::kMaxEncodeBatchBlocks + 1U}) {
        bool rejected = false;
        try {
            const auto batch = flitzzip::encode_owned_block_batch_cpu(std::vector<std::byte>(count, std::byte{0x7B}),
                                                                      std::vector<std::uint32_t>(count, 1U), {});
            REQUIRE_EQ(batch.encoded.blocks.size(), count);
            REQUIRE_EQ(batch.block_crc32.size(), count);
            REQUIRE_EQ(batch.encoded.payload.capacity(), 0U);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_EQ(rejected, count > flitzzip::kMaxEncodeBatchBlocks);
    }
    for (const auto options :
         {flitzzip::ArchiveCodecOptions{.block_size = 0U}, flitzzip::ArchiveCodecOptions{.compression_level = 0},
          flitzzip::ArchiveCodecOptions{.compression_level = 10}}) {
        bool rejected = false;
        try {
            (void)flitzzip::encode_owned_block_batch_cpu({}, {}, options);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Prove unequal HIP blocks retain independent encoding and device CRC semantics.
// Inputs: All effort settings and every product block-size option on an available HIP device.
// Outputs: Requires exact separate-encode bytes plus independent CPU and HIP roundtrips; absent HIP skips this case.
TEST_CASE(gpu_block_batch_hip_identity) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    constexpr std::array<std::uint32_t, 12> lengths{1, 31,    4095,  4096,   4097,   65537,
                                                    2, 65535, 65536, 131073, 262144, 65539};
    for (int level = 1; level <= 9; ++level) {
        check_batch_identity(lengths, {.block_size = 256U * 1024U, .compression_level = level});
    }
    constexpr std::array<std::uint32_t, 13> crc_edges{
        1U, 127U, 128U, 129U, 255U, 256U, 257U, 8191U, 8192U, 8193U, 32767U, 32768U, 32769U,
    };
    check_batch_identity(crc_edges, {.block_size = 256U * 1024U, .compression_level = 5});
    for (std::uint32_t size = 256U * 1024U; size <= flitzzip::kMaxArchiveBlockBytes; size *= 2U) {
        const std::array<std::uint32_t, 4> edges{3, size - 1U, size, 65537};
        check_batch_identity(edges, {.block_size = size, .compression_level = 9});
    }
}

// Purpose: Exercise empty/max-count batches and reject inconsistent layout or backend requirements.
// Inputs: RAM-only bounded valid and invalid length tables; no GPU is required for validation checks.
// Outputs: Requires exact coverage admission, no dispatch on invalid layouts, and explicit policy conflict rejection.
TEST_CASE(gpu_block_batch_layout_validation) {
    check_batch_identity({}, {.require_gpu = false, .force_cpu = true});
    const std::vector<std::uint32_t> maximum(flitzzip::kMaxEncodeBatchBlocks, 1U);
    check_batch_identity(maximum, {.require_gpu = false, .force_cpu = true});
    if (flitzzip::query_gpu_info().available) {
        check_batch_identity(maximum, {});
    }
    require_invalid_batch(1, {});
    for (const auto lengths : {std::vector<std::uint32_t>{0}, {2}, {1, 1}, {1, 0}, {0, 1}}) {
        require_invalid_batch(1, lengths);
    }
    require_invalid_batch(0, std::array<std::uint32_t, 1>{1});
    require_invalid_batch(2, std::array<std::uint32_t, 1>{1});
    require_invalid_batch(flitzzip::kMinArchiveBlockBytes + 1U,
                          std::array<std::uint32_t, 1>{flitzzip::kMinArchiveBlockBytes + 1U});
    require_invalid_batch(257, std::vector<std::uint32_t>(257, 1U));
    bool rejected = false;
    try {
        (void)flitzzip::encode_owned_block_batch({}, {}, {.require_gpu = true, .force_cpu = true});
    } catch (const flitzzip::GpuError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Bound production grouping by count, bytes, file kind, and selected CPU policy.
// Inputs: Synthetic manifest metadata only, including exact and just-over byte caps.
// Outputs: Requires deterministic grouping without reading files or launching HIP work.
TEST_CASE(gpu_block_batch_archive_policy) {
    flitzzip::CompressOptions options;
    options.block_size = 256U * 1024U;
    options.chunk_size = 8U * 1024U * 1024U;
    std::vector<flitzzip::ManifestEntry> entries(70);
    for (auto& entry : entries) {
        entry.size = 4096;
    }
    REQUIRE_EQ(flitzzip::archive_encode_batch_count(entries, options), 64U);
    options.force_cpu = true;
    REQUIRE_EQ(flitzzip::archive_encode_batch_count(entries, options), 0U);
    options.force_cpu = false;
    for (auto& entry : entries) {
        entry.size = options.block_size;
    }
    REQUIRE_EQ(flitzzip::archive_encode_batch_count(entries, options), 32U);
    options.chunk_size = 2U * options.block_size;
    REQUIRE_EQ(flitzzip::archive_encode_batch_count(entries, options), 2U);
    options.chunk_size -= 1;
    REQUIRE_EQ(flitzzip::archive_encode_batch_count(entries, options), 0U);
    options.chunk_size = 8U * 1024U * 1024U;
    entries[2].directory = true;
    REQUIRE_EQ(flitzzip::archive_encode_batch_count(entries, options), 2U);
    entries[2].directory = false;
    entries[2].size = 0;
    REQUIRE_EQ(flitzzip::archive_encode_batch_count(entries, options), 2U);
    entries[2].size = options.block_size + 1U;
    REQUIRE_EQ(flitzzip::archive_encode_batch_count(entries, options), 2U);
    entries[1].size = 0;
    REQUIRE_EQ(flitzzip::archive_encode_batch_count(entries, options), 0U);
}

namespace {

// Purpose: Independently serialize per-file encoding as a byte-level oracle for the batched archive scheduler.
// Inputs: manifest points to bounded fixture files; options selects the same backend/effort as production.
// Outputs: Returns a complete archive with the block-required version, independent of the batch writer.
std::string separate_archive_bytes(const flitzzip::Manifest& manifest, const flitzzip::GpuCodecOptions& options) {
    flitzzip::ArchiveIndex index;
    std::ostringstream output(std::ios::binary);
    for (const auto& source : manifest.entries) {
        flitzzip::ArchiveEntry entry{.path = source.archive_path,
                                     .directory = source.directory,
                                     .uncompressed_size = source.size,
                                     .payload_offset = static_cast<std::uint64_t>(output.tellp())};
        if (!source.directory) {
            std::vector<std::byte> bytes(static_cast<std::size_t>(source.size));
            std::ifstream input(source.source_path, std::ios::binary);
            input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            REQUIRE_EQ(input.gcount(), static_cast<std::streamsize>(bytes.size()));
            const auto encoded = flitzzip::encode_owned_chunk(std::move(bytes), options);
            entry.blocks = encoded.blocks;
            entry.crc32 = encoded.source_crc32;
            entry.payload_size = encoded.payload.size();
            if (!encoded.payload.empty()) {
                output.write(reinterpret_cast<const char*>(encoded.payload.data()),
                             static_cast<std::streamsize>(encoded.payload.size()));
            }
        }
        index.entries.push_back(std::move(entry));
    }
    for (const auto& entry : index.entries) {
        for (const auto& block : entry.blocks) {
            if (block.kind == flitzzip::BlockKind::GpuLongSparsePattern) {
                index.version = std::max(index.version, 7U);
            } else if (block.kind == flitzzip::BlockKind::CpuZstd) {
                index.version = std::max(index.version, 6U);
            } else if (block.kind == flitzzip::BlockKind::GpuSparsePattern) {
                index.version = std::max(index.version, 5U);
            } else if (block.kind == flitzzip::BlockKind::GpuDictionary ||
                       (block.kind == flitzzip::BlockKind::Pattern &&
                        block.encoded_len > flitzzip::kLegacyGpuPatternBytes)) {
                index.version = std::max(index.version, 4U);
            }
        }
    }
    const auto offset = static_cast<std::uint64_t>(output.tellp());
    flitzzip::write_archive_index(output, index);
    const auto size = static_cast<std::uint64_t>(output.tellp()) - offset;
    flitzzip_test::write_test_footer(output, offset, size, index.version);
    return output.str();
}

// Purpose: Populate deterministic small, empty, and larger files for production scheduler transition tests.
// Inputs: root is an isolated temporary fixture directory; payload stays below two MiB.
// Outputs: Creates 73 regular files and a directory boundary, with no timing measurement.
void write_archive_batch_fixture(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "nested");
    std::vector<std::uint32_t> lengths(70);
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        lengths[i] = static_cast<std::uint32_t>(4093U + i);
    }
    const auto bytes = batch_fixture(lengths);
    std::size_t offset = 0;
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        std::ofstream file(root / ("file-" + std::to_string(100U + i)), std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data() + offset), lengths[i]);
        REQUIRE_TRUE(file.good());
        offset += lengths[i];
    }
    std::ofstream(root / "file-135-empty", std::ios::binary);
    const std::vector<char> large(256U * 1024U + 1U, 'x');
    std::ofstream file(root / "file-145-large", std::ios::binary);
    file.write(large.data(), static_cast<std::streamsize>(large.size()));
    REQUIRE_TRUE(file.good());
    std::ofstream(root / "nested" / "empty", std::ios::binary);
}

}  // namespace

// Purpose: Validate production batching against complete independently serialized archives and both readers.
// Inputs: Small-file fixtures crossing empty/large/directory boundaries and two HIP effort tiers.
// Outputs: Requires full archive byte identity, fewer encode submissions, progress totals, and exact extracted files.
TEST_CASE(gpu_block_batch_archive_roundtrip) {
    const bool gpu = flitzzip::query_gpu_info().available;
    const auto root = test_temp_dir("block-batch-archive");
    write_archive_batch_fixture(root / "source");
    const auto manifest = flitzzip::build_manifest({root / "source"});
    for (const int level : {5, 9}) {
        flitzzip::CompressOptions options;
        options.gpu_required = gpu;
        options.block_size = 256U * 1024U;
        options.compression_level = level;
        flitzzip::ProgressSnapshot last;
        const auto archive = root / "batch.flzip";
        const auto stats =
            flitzzip::compress_flzip({root / "source"}, archive, options,
                                     [&last](const flitzzip::ProgressSnapshot& progress) { last = progress; });
        const auto reference = separate_archive_bytes(
            manifest, {.require_gpu = gpu, .block_size = options.block_size, .compression_level = level});
        std::ifstream actual(archive, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(actual)), std::istreambuf_iterator<char>());
        REQUIRE_EQ(bytes, reference);
        REQUIRE_EQ(last.processed_bytes, manifest.total_file_bytes);
        REQUIRE_EQ(last.completed_entries, manifest.entries.size());
        REQUIRE_EQ(stats.gpu_used, gpu);
        if (gpu) {
            REQUIRE_TRUE(stats.gpu_runtime.encode_chunks < 10U);
            flitzzip::verify_flzip(archive, {.gpu_required = true});
        }
        flitzzip::extract_flzip(archive, root / "extracted",
                                {.gpu_required = false, .force_cpu = true, .overwrite = true});
        for (const auto& entry : manifest.entries) {
            if (!entry.directory) {
                REQUIRE_TRUE(
                    flitzzip_test::files_are_equal(entry.source_path, root / "extracted" / entry.archive_path));
            }
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Preserve the old destination and release all pinned sources when a batch is interrupted.
// Inputs: An isolated fixture, existing destination sentinel, and a callback throwing after the first source read.
// Outputs: Requires exact sentinel preservation, no staging file, and unlocked source files after failure.
// Unexpected errors propagate so resource rejection cannot masquerade as a failed interruption control.
TEST_CASE(gpu_block_batch_archive_abort) {
    const auto root = test_temp_dir("block-batch-abort");
    write_archive_batch_fixture(root / "source");
    const auto archive = root / "batch.flzip";
    {
        std::ofstream old(archive, std::ios::binary);
        old << "sentinel";
    }
    bool interrupted = false;
    try {
        flitzzip::compress_flzip({root / "source"}, archive, {.gpu_required = false},
                                 [](const flitzzip::ProgressSnapshot& progress) {
                                     if (progress.processed_bytes != 0) {
                                         throw flitzzip::ArchiveError("test batch interruption");
                                     }
                                 });
    } catch (const flitzzip::ArchiveError& error) {
        if (std::string_view(error.what()) != "test batch interruption") {
            throw;
        }
        interrupted = true;
    }
    REQUIRE_TRUE(interrupted);
    {
        std::ifstream old(archive, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(old)), std::istreambuf_iterator<char>());
        REQUIRE_EQ(bytes, "sentinel");
    }
    REQUIRE_EQ(flitzzip_test::count_regular_files(root), 74U);
    std::filesystem::remove_all(root);
    REQUIRE_TRUE(!std::filesystem::exists(root));
}

namespace {

constexpr std::array<std::string_view, 2> kSmallBatchContents{"first::0123456789ABCDEF", "second::ZYXWVUT987654321"};

// Purpose: Produce two independently named, nonuniform sources for batch boundary tests.
// Inputs: A test-owned directory; the complete source payload totals fewer than 64 bytes.
// Outputs: Returns identity-pinned manifest metadata after closing both fixture writers.
flitzzip::Manifest small_archive_batch_fixture(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    for (std::size_t i = 0; i < kSmallBatchContents.size(); ++i) {
        files.push_back(root / (std::to_string(i) + ".bin"));
        std::ofstream output(files.back(), std::ios::binary);
        output << kSmallBatchContents[i];
        REQUIRE_TRUE(output.good());
    }
    return flitzzip::build_manifest(files);
}

// Purpose: Prove batch teardown released write/delete denial without mutating fixture identities.
// Inputs: The same source manifest after a successful or failed synchronous batch.
// Outputs: Opens and closes each source for writing/deletion, failing if a batch lock escaped.
void require_batch_sources_released(const flitzzip::Manifest& manifest) {
    for (const auto& source : manifest.entries) {
        const HANDLE handle = CreateFileW(source.source_path.c_str(), GENERIC_WRITE | DELETE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE_TRUE(handle != INVALID_HANDLE_VALUE);
        REQUIRE_TRUE(CloseHandle(handle) != 0);
    }
}

class BatchFailureBuffer final : public std::stringbuf {
  public:
    // Purpose: Inject only sink position or bounded-write failure into the real archive publisher.
    // Inputs: Whether tellp fails and the maximum accepted output bytes.
    // Outputs: Creates a RAM-only sink; no production codec or metadata implementation is replaced.
    BatchFailureBuffer(bool reject_position, std::size_t write_limit)
        : reject_position_(reject_position), write_limit_(write_limit) {}

  protected:
    // Purpose: Fail position discovery independently of data writes.
    // Inputs: Standard stream position arguments.
    // Outputs: The real string-buffer position or the standard failure sentinel.
    pos_type seekoff(off_type offset, std::ios::seekdir direction, std::ios::openmode mode) override {
        return reject_position_ ? pos_type(off_type(-1)) : std::stringbuf::seekoff(offset, direction, mode);
    }

    // Purpose: Model a short sink write without consuming the rejected suffix.
    // Inputs: A valid byte span and count from the production ostream.
    // Outputs: Writes at most the remaining budget and returns the actual count.
    std::streamsize xsputn(const char* bytes, std::streamsize count) override {
        const auto available = write_limit_ - written_;
        const auto accepted = std::min<std::size_t>(available, static_cast<std::size_t>(count));
        written_ += accepted;
        return std::stringbuf::xsputn(bytes, static_cast<std::streamsize>(accepted));
    }

  private:
    bool reject_position_;
    std::size_t write_limit_;
    std::size_t written_ = 0;
};

}  // namespace

// Purpose: Reject invalid batch/count geometry before any source read or output mutation.
// Inputs: Empty/single/oversized batches, disabled batching and exhausted/overflowing total-block counts.
// Outputs: The owning boundary reports its admission error and leaves index, counters and output unchanged.
TEST_CASE(gpu_block_batch_rejects_invalid_admission_before_io) {
    flitzzip::CompressOptions options{.gpu_required = false};
    const auto invoke = [&](std::span<const flitzzip::ManifestEntry> entries, const flitzzip::CompressOptions& selected,
                            std::uint64_t count, std::string_view diagnostic) {
        flitzzip::ArchiveIndex index;
        flitzzip::OperationStats stats;
        flitzzip::ProgressState progress;
        std::ostringstream output;
        output << "sentinel";
        const auto before = count;
        bool rejected = false;
        try {
            flitzzip::compress_manifest_batch(entries, selected, output, index, stats, count, progress, {}, {});
        } catch (const flitzzip::ArchiveError& error) {
            rejected = std::string_view(error.what()).find(diagnostic) != std::string_view::npos;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(output.str(), "sentinel");
        REQUIRE_TRUE(index.entries.empty());
        REQUIRE_EQ(count, before);
        REQUIRE_EQ(progress.snapshot().processed_bytes, 0U);
    };
    std::vector<flitzzip::ManifestEntry> entries(65);
    for (auto& entry : entries) {
        entry.size = 16;
    }
    for (const std::size_t size : {0U, 1U, 65U}) {
        invoke(std::span(entries).first(size), options, 0, "batch violates");
    }
    options.force_cpu = true;
    invoke(std::span(entries).first(2), options, 0, "batch violates");
    options.force_cpu = false;
    for (const std::uint64_t count :
         {static_cast<std::uint64_t>(flitzzip::kMaxArchiveBlocks - 1U),
          static_cast<std::uint64_t>(flitzzip::kMaxArchiveBlocks),
          static_cast<std::uint64_t>(flitzzip::kMaxArchiveBlocks) + 1U, std::numeric_limits<std::uint64_t>::max()}) {
        invoke(std::span(entries).first(2), options, count, "too many total blocks");
    }
}

// Purpose: Preserve cancellation semantics and source ownership at each batch phase.
// Inputs: Cancellation before reading, during reads, after all reads and after the first publication.
// Outputs: No subsequent entry is published; counters reflect committed entries and source locks are released.
TEST_CASE(gpu_block_batch_cancellation_releases_every_source) {
    const auto root = test_temp_dir("batch-cancellation-boundaries");
    const auto manifest = small_archive_batch_fixture(root);
    for (unsigned int phase = 0; phase < 4; ++phase) {
        flitzzip::ProgressState progress;
        progress.start(flitzzip::OperationKind::Compress, manifest.total_file_bytes, 2);
        if (phase == 0) {
            progress.request_cancel();
        }
        const auto observer = [&](const flitzzip::ProgressSnapshot& snapshot) {
            if ((phase == 1 && snapshot.processed_bytes != 0) ||
                (phase == 2 && snapshot.processed_bytes == manifest.total_file_bytes) ||
                (phase == 3 && snapshot.completed_entries == 1)) {
                progress.request_cancel();
            }
        };
        std::ostringstream output;
        flitzzip::ArchiveIndex index;
        flitzzip::OperationStats stats;
        std::uint64_t blocks = 0;
        bool cancelled = false;
        try {
            flitzzip::compress_manifest_batch(manifest.entries, {.gpu_required = false}, output, index, stats, blocks,
                                              progress, observer, {});
        } catch (const flitzzip::ArchiveError& error) {
            cancelled = std::string_view(error.what()) == "operation cancelled";
        }
        REQUIRE_TRUE(cancelled);
        REQUIRE_EQ(index.entries.size(), phase == 3 ? 1U : 0U);
        REQUIRE_EQ(blocks, index.entries.size());
        if (phase != 3) {
            REQUIRE_TRUE(output.str().empty());
        }
        require_batch_sources_released(manifest);
    }
}

// Purpose: Preserve publication accounting when position discovery or either file write fails.
// Inputs: A real two-source codec batch and independently failing in-memory output sinks.
// Outputs: Throws without indexing an incomplete file and releases all source locks, with stream exceptions too.
TEST_CASE(gpu_block_batch_sink_failures_release_sources) {
    const auto root = test_temp_dir("batch-sink-failure");
    const auto manifest = small_archive_batch_fixture(root);
    for (const bool reject_position : {false, true}) {
        for (const bool exceptions : {false, true}) {
            BatchFailureBuffer buffer(reject_position, 1);
            std::ostream output(&buffer);
            if (exceptions) {
                output.exceptions(std::ios::badbit | std::ios::failbit);
            }
            flitzzip::ArchiveIndex index;
            flitzzip::OperationStats stats;
            flitzzip::ProgressState progress;
            std::uint64_t blocks = 0;
            bool rejected = false;
            try {
                flitzzip::compress_manifest_batch(manifest.entries, {.gpu_required = false}, output, index, stats,
                                                  blocks, progress, {}, {});
            } catch (const std::exception&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
            REQUIRE_TRUE(index.entries.empty());
            REQUIRE_EQ(blocks, 0U);
            REQUIRE_TRUE(buffer.str().size() <= 1U);
            require_batch_sources_released(manifest);
        }
    }
}

namespace {

struct BatchTiming {
    double milliseconds = 0;
    std::size_t payload_bytes = 0;
    std::uint32_t checksum = 0;
    flitzzip::GpuRuntimeStats telemetry;
};

// Purpose: Time the production batch API or its unchanged per-file baseline with equal input-copy accounting.
// Inputs: input/lengths are RAM-only immutable fixtures; level and batched select equal-effort HIP submissions.
// Outputs: Returns wall time, exact payload size/combined CRC, and real HIP counters without writing payload files.
BatchTiming time_batch_encoding(const std::vector<std::byte>& input, std::span<const std::uint32_t> lengths, int level,
                                bool batched) {
    flitzzip::GpuCodecOptions options;
    options.compression_level = level;
    options.block_size = 256U * 1024U;
    options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
    BatchTiming timing;
    const auto start = std::chrono::steady_clock::now();
    if (batched) {
        const auto result = flitzzip::encode_owned_block_batch(input, lengths, options);
        REQUIRE_TRUE(result.encoded.gpu_used);
        timing.payload_bytes = result.encoded.payload.size();
        timing.checksum = result.encoded.source_crc32;
    } else {
        std::size_t offset = 0;
        for (const auto length : lengths) {
            const auto bytes = std::span(input).subspan(offset, length);
            const auto result =
                flitzzip::encode_owned_chunk(std::vector<std::byte>(bytes.begin(), bytes.end()), options);
            REQUIRE_TRUE(result.gpu_used);
            timing.payload_bytes += result.payload.size();
            timing.checksum = flitzzip::crc32_combine(timing.checksum, result.source_crc32, length);
            offset += length;
        }
    }
    timing.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    timing.telemetry = flitzzip::snapshot_gpu_telemetry(*options.telemetry);
    return timing;
}

// Purpose: Emit one parseable measured sample with explicit scope and real execution evidence.
// Inputs: timing, input size, profile, level, round, and mode identify one warmed equal-effort RAM run.
// Outputs: Writes one stdout record; does not claim filesystem throughput or alter codec payloads.
void print_batch_timing(const BatchTiming& timing, std::size_t input_size, const char* profile, int level, int round,
                        bool batched) {
    std::cout << "batch_benchmark mode=" << (batched ? "batch" : "separate") << " profile=" << profile
              << " level=" << level << " round=" << round << " files=64 block_size=262144 input_bytes=" << input_size
              << " payload_bytes=" << timing.payload_bytes
              << " ratio=" << static_cast<double>(input_size) / timing.payload_bytes
              << " encode_wall_ms=" << timing.milliseconds << " encode_chunks=" << timing.telemetry.encode_chunks
              << " kernel_launches=" << timing.telemetry.kernel_launches << " h2d_bytes=" << timing.telemetry.h2d_bytes
              << " d2h_bytes=" << timing.telemetry.d2h_bytes
              << " allocation_bytes=" << timing.telemetry.device_allocation_bytes
              << " kernel_ms=" << timing.telemetry.kernel_ms
              << " gpu_used=true timing_scope=host_encode memory_only=true disk_write_bytes=0\n";
}

}  // namespace

// Purpose: Compare the production batch API with separate encoding in repeated alternating RAM-only runs.
// Inputs: FLITZZIP_BATCH_BENCHMARK=1 explicitly opts in under an external host-resource monitor.
// Outputs: Prints six paired samples per profile/size/effort after warmup; requires exact sizes and CRC identity.
TEST_CASE(gpu_block_batch_benchmark_opt_in, TestExecutionRole::Measurement) {
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"FLITZZIP_BATCH_BENCHMARK", enabled, 2U) != 1U || enabled[0] != L'1') {
        return;
    }
    REQUIRE_TRUE(flitzzip::query_gpu_info().available);
    for (const auto size : {4096U, 65536U}) {
        std::vector<std::uint32_t> lengths(64);
        for (std::size_t i = 0; i < lengths.size(); ++i) {
            lengths[i] = size - 31U + static_cast<std::uint32_t>(i);
        }
        for (const auto profile : {0, 1, 2}) {
            auto input = batch_fixture(lengths);
            if (profile != 0) {
                std::uint32_t state = 0x6347218BU;
                for (auto& byte : input) {
                    state ^= state << 13U;
                    state ^= state >> 17U;
                    state ^= state << 5U;
                    byte = static_cast<std::byte>(profile == 1 ? state % 256U : 193U + state % 5U);
                }
            }
            const char* label = profile == 0 ? "Mixed" : profile == 1 ? "Random" : "ShiftedAlphabet";
            for (const int level : {5, 9}) {
                (void)time_batch_encoding(input, lengths, level, false);
                (void)time_batch_encoding(input, lengths, level, true);
                for (int round = 0; round < 6; ++round) {
                    std::array<BatchTiming, 2> results;
                    for (const bool second : {false, true}) {
                        const bool batched = second != (round % 2 != 0);
                        results[batched ? 1 : 0] = time_batch_encoding(input, lengths, level, batched);
                        print_batch_timing(results[batched ? 1 : 0], input.size(), label, level, round, batched);
                    }
                    REQUIRE_EQ(results[0].payload_bytes, results[1].payload_bytes);
                    REQUIRE_EQ(results[0].checksum, results[1].checksum);
                }
            }
        }
    }
}
