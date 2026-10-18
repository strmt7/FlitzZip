#include "gpu/dictionary_matcher.hpp"
#include "gpu/gpu_codec.hpp"
#include "core/checksum.hpp"
#include "core/dictionary_block.hpp"
#include "core/result.hpp"
#include "core/file_publish.hpp"
#include "test_util.hpp"
#include "lz4.h"

#if FLITZZIP_ENABLE_HIP
#include "gpu/hip_kernel_api.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <unordered_map>

namespace {

using namespace flitzzip::dictionary;

// Purpose: Validate every returned match against original bytes, independently of the device's index.
// Inputs: The immutable source, search result, and requested level.
// Outputs: Requires exact earlier-substring equality, segment locality, and all declared work/resource bounds.
void require_valid_matches(std::span<const std::byte> input, const MatchBatch& batch, int level) {
    const auto effort = effort_for_level(level);
    REQUIRE_EQ(batch.matches.size(), input.size());
    REQUIRE_TRUE(batch.device_workspace_bytes <= kMaxWorkspaceBytes);
    for (const auto duration : {batch.index_ms, batch.search_ms}) {
        REQUIRE_TRUE(!duration || (std::isfinite(*duration) && *duration >= 0.0));
    }
    if (batch.gpu_used) {
        REQUIRE_EQ(batch.h2d_bytes, input.size());
        REQUIRE_EQ(batch.d2h_bytes, input.size() * sizeof(Match));
        REQUIRE_TRUE(batch.primitive_version > 0U);
    }
    for (std::size_t position = 0; position < input.size(); ++position) {
        const auto& match = batch.matches[position];
        REQUIRE_TRUE(match.candidates_examined <= effort.max_candidates);
        REQUIRE_TRUE(match.bytes_compared <= effort.max_byte_comparisons);
        if (match.length == 0U) {
            REQUIRE_EQ(match.distance, 0U);
            continue;
        }
        const auto segment_start = (position / kSegmentBytes) * kSegmentBytes;
        const auto segment_end = std::min(input.size(), segment_start + kSegmentBytes);
        REQUIRE_TRUE(match.length >= kMinMatchBytes && match.length <= kMaxMatchBytes);
        REQUIRE_TRUE(match.length <= segment_end - position);
        REQUIRE_TRUE(match.distance > 0U && match.distance <= position - segment_start);
        const auto actual = input.subspan(position, match.length);
        const auto prior = input.subspan(position - match.distance, match.length);
        REQUIRE_TRUE(std::equal(actual.begin(), actual.end(), prior.begin()));
    }
}

// Purpose: Create exact-prefix repetitions whose useful extensions increase at successive search depths.
// Inputs: None; the final 64-byte record is the target and its 256 predecessors have controlled mismatch positions.
// Outputs: Returns a RAM-only corpus proving that every effort level can find a genuinely better dictionary match.
std::vector<std::byte> make_dictionary_depth_fixture() {
    std::vector<std::byte> input(257U * 64U);
    for (std::uint32_t depth = 1; depth <= 256U; ++depth) {
        const auto start = (256U - depth) * 64U;
        const auto length = 3U + std::bit_width(depth);
        for (std::uint32_t i = 0; i < length; ++i) {
            input[start + i] = static_cast<std::byte>(i + 1U);
        }
        input[start + length] = std::byte{0xFF};
    }
    for (std::uint32_t i = 0; i < 64U; ++i) {
        input[256U * 64U + i] = static_cast<std::byte>(i + 1U);
    }
    return input;
}

// Purpose: Exercise repeated records that require dictionary matches rather than a single periodic pattern.
// Inputs: A 16 KiB-aligned byte count, seeded full-byte record, and one varying byte per later record.
// Outputs: Returns deterministic nonperiodic bytes with long repeated substrings and no filesystem work.
std::vector<std::byte> make_near_identical_records(std::size_t size) {
    constexpr std::size_t record_bytes = 16384U;
    std::vector<std::byte> input(size);
    std::uint32_t state = 0x31674325U;
    for (std::size_t index = 0; index < input.size(); ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        input[index] = index < record_bytes ? static_cast<std::byte>(state >> 24U) : input[index % record_bytes];
        if (index >= record_bytes && index % record_bytes == 1024U) {
            input[index] = static_cast<std::byte>((index / record_bytes) & 255U);
        }
    }
    return input;
}

// Purpose: Exercise dictionary-local repetition without a block-wide periodic sparse pattern.
// Inputs: A whole number of 64 KiB segments, each with a separately seeded 12 KiB, 13,003-byte, or 16 KiB record.
// Outputs: Returns near-identical records per segment with no matching bases between adjacent segments.
std::vector<std::byte> make_segmented_records(std::size_t size, std::size_t record_bytes = 16U * 1024U) {
    constexpr std::size_t segment_bytes = kSegmentBytes;
    REQUIRE_TRUE(record_bytes == 12U * 1024U || record_bytes == 13003U || record_bytes == 16U * 1024U);
    std::vector<std::byte> input(size);
    for (std::size_t segment = 0; segment < size / segment_bytes; ++segment) {
        const auto base = segment * segment_bytes;
        std::uint32_t state = static_cast<std::uint32_t>(0x31674325U + segment * 0x9E3779B9U);
        for (std::size_t index = 0; index < record_bytes; ++index) {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            input[base + index] = static_cast<std::byte>(state >> 24U);
        }
        for (std::size_t index = record_bytes; index < segment_bytes; ++index) {
            input[base + index] = input[base + index % record_bytes];
            if (index % record_bytes == 1024U) {
                input[base + index] ^= static_cast<std::byte>(1U + (segment + index / record_bytes) % 255U);
            }
        }
    }
    return input;
}

// Purpose: Prove large-block HIP encoding actually selects dictionary blocks on locally repeated data.
// Inputs: Independently seeded 64 KiB groups with 12 or 16 KiB records, level-five HIP, and production block sizes.
// Outputs: Requires entropy rejection before launch, one bounded periodic dictionary batch, and exact decoding.
TEST_CASE(dictionary_segmented_records_large_block_roundtrip) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    for (const auto [block_bytes, record_bytes] :
         {std::pair{1024U * 1024U, 16U * 1024U}, std::pair{8U * 1024U * 1024U, 16U * 1024U},
          std::pair{16U * 1024U * 1024U, 16U * 1024U}, std::pair{16U * 1024U * 1024U, 12U * 1024U}}) {
        const auto input = make_segmented_records(block_bytes, record_bytes);
        flitzzip::GpuCodecOptions options;
        options.block_size = static_cast<std::uint32_t>(block_bytes);
        options.compression_level = 5;
        options.require_gpu = true;
        options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
        const auto encoded = flitzzip::encode_chunk(input, options);
        REQUIRE_TRUE(encoded.gpu_used);
        REQUIRE_EQ(encoded.blocks.size(), 1U);
        REQUIRE_EQ(encoded.blocks.front().kind, flitzzip::BlockKind::GpuDictionary);
        REQUIRE_TRUE(encoded.payload.size() < input.size());
        if (block_bytes >= 8U * 1024U * 1024U) {
            const auto telemetry = flitzzip::snapshot_gpu_telemetry(*options.telemetry);
            std::cout << "dictionary_segmented_launches block_bytes=" << block_bytes << " record_bytes=" << record_bytes
                      << " launches=" << telemetry.kernel_launches << '\n';
            REQUIRE_EQ(telemetry.kernel_launches, 4U);
            REQUIRE_EQ(telemetry.dictionary_blocks, 1U);
        }
        for (const bool hip : {false, true}) {
            auto decode_options = options;
            decode_options.require_gpu = hip;
            decode_options.force_cpu = !hip;
            std::vector<std::byte> decoded(input.size());
            REQUIRE_EQ(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, decode_options), hip);
            REQUIRE_EQ(decoded, input);
        }
    }
}

// Purpose: Catch dictionary screening that misses useful repeats when the record period is off the sampling grid.
// Inputs: Independently seeded 13,003-byte records within 64 KiB segments of a 1 MiB required-HIP block.
// Outputs: Requires a materially smaller dictionary block at low/default/high effort and exact CPU/HIP read-back.
TEST_CASE(dictionary_off_grid_segmented_records_roundtrip) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const auto input = make_segmented_records(1024U * 1024U, 13003U);
    for (const int level : {1, 5, 9}) {
        flitzzip::GpuCodecOptions options;
        options.block_size = static_cast<std::uint32_t>(input.size());
        options.compression_level = level;
        options.require_gpu = true;
        const auto encoded = flitzzip::encode_chunk(input, options);
        REQUIRE_EQ(encoded.blocks.size(), 1U);
        REQUIRE_EQ(encoded.blocks.front().kind, flitzzip::BlockKind::GpuDictionary);
        REQUIRE_TRUE(encoded.payload.size() < input.size() / 2U);
        std::cout << "dictionary_off_grid_case level=" << level << " input_bytes=" << input.size()
                  << " payload_bytes=" << encoded.payload.size() << " memory_only=true disk_write_bytes=0\n";
        for (const bool hip : {false, true}) {
            auto decode_options = options;
            decode_options.require_gpu = hip;
            decode_options.force_cpu = !hip;
            std::vector<std::byte> decoded(input.size());
            REQUIRE_EQ(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, decode_options), hip);
            REQUIRE_EQ(decoded, input);
        }
    }
}

// Purpose: Keep periodic HIP grouping transparent to independently framed native blocks.
// Inputs: Two adjacent 8 MiB segmented-record blocks at balanced and maximum effort.
// Outputs: Grouped payload bytes equal separate encodes, and both decoders restore the source exactly.
TEST_CASE(dictionary_adjacent_periodic_blocks_preserve_encoded_bytes) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    constexpr std::size_t block_bytes = 8U * 1024U * 1024U;
    const auto input = make_segmented_records(2U * block_bytes);
    for (const int level : {5, 9}) {
        flitzzip::GpuCodecOptions options;
        options.block_size = static_cast<std::uint32_t>(block_bytes);
        options.compression_level = level;
        options.require_gpu = true;
        const auto grouped = flitzzip::encode_chunk(input, options);
        REQUIRE_EQ(grouped.blocks.size(), 2U);
        std::vector<std::byte> separately_encoded;
        for (std::size_t block = 0U; block < 2U; ++block) {
            const auto source = std::span(input).subspan(block * block_bytes, block_bytes);
            const auto separate = flitzzip::encode_chunk(source, options);
            REQUIRE_EQ(separate.blocks.size(), 1U);
            REQUIRE_EQ(grouped.blocks[block].kind, separate.blocks.front().kind);
            REQUIRE_EQ(grouped.blocks[block].encoded_len, separate.blocks.front().encoded_len);
            separately_encoded.insert(separately_encoded.end(), separate.payload.begin(), separate.payload.end());
        }
        REQUIRE_EQ(grouped.payload, separately_encoded);
        for (const bool hip : {false, true}) {
            auto decode_options = options;
            decode_options.require_gpu = hip;
            decode_options.force_cpu = !hip;
            std::vector<std::byte> decoded(input.size());
            REQUIRE_EQ(flitzzip::decode_chunk(grouped.payload, grouped.blocks, decoded, decode_options), hip);
            REQUIRE_EQ(decoded, input);
        }
    }
}

// Purpose: Preserve independent encoding when one adjacent block fails the periodic admission probe.
// Inputs: Two 8 MiB dictionary-friendly blocks with a disrupted record in the second block.
// Outputs: Group-attempt fallback leaves complete per-block payload bytes unchanged and decodes exactly.
TEST_CASE(dictionary_periodic_group_probe_falls_back_without_byte_changes) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    constexpr std::size_t block_bytes = 8U * 1024U * 1024U;
    constexpr std::size_t segment_bytes = kSegmentBytes;
    auto input = make_segmented_records(2U * block_bytes);
    const auto disrupted = block_bytes + segment_bytes + 3U * 16384U;
    for (std::size_t offset = 0U; offset < 16384U; ++offset) {
        input[disrupted + offset] ^= static_cast<std::byte>((offset * 29U + 17U) & 0xFFU);
    }
    flitzzip::GpuCodecOptions options;
    options.block_size = static_cast<std::uint32_t>(block_bytes);
    options.compression_level = 5;
    options.require_gpu = true;
    const auto grouped = flitzzip::encode_chunk(input, options);
    REQUIRE_EQ(grouped.blocks.size(), 2U);
    std::vector<std::byte> separately_encoded;
    for (std::size_t block = 0U; block < 2U; ++block) {
        const auto source = std::span(input).subspan(block * block_bytes, block_bytes);
        const auto separate = flitzzip::encode_chunk(source, options);
        REQUIRE_EQ(separate.blocks.size(), 1U);
        REQUIRE_EQ(grouped.blocks[block].kind, separate.blocks.front().kind);
        REQUIRE_EQ(grouped.blocks[block].encoded_len, separate.blocks.front().encoded_len);
        separately_encoded.insert(separately_encoded.end(), separate.payload.begin(), separate.payload.end());
    }
    REQUIRE_EQ(grouped.payload, separately_encoded);
    std::vector<std::byte> decoded(input.size());
    REQUIRE_TRUE(flitzzip::decode_chunk(grouped.payload, grouped.blocks, decoded, options));
    REQUIRE_EQ(decoded, input);
}

// Purpose: Check that longer periodic matches preserve effort ordering on a realistic independent-segment block.
// Inputs: One deterministic 16 MiB block with a changing 16 KiB record in each segment.
// Outputs: Requires byte-exact CPU read-back and six distinct low/mid-effort sizes without high-effort growth.
TEST_CASE(dictionary_periodic_efforts_preserve_size_order) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const auto input = make_segmented_records(16U * 1024U * 1024U);
    std::size_t previous = input.size();
    for (int level = 1; level <= 9; ++level) {
        flitzzip::GpuCodecOptions options;
        options.block_size = static_cast<std::uint32_t>(input.size());
        options.compression_level = level;
        options.require_gpu = true;
        const auto encoded = flitzzip::encode_chunk(input, options);
        REQUIRE_EQ(encoded.blocks.size(), 1U);
        REQUIRE_EQ(encoded.blocks.front().kind, flitzzip::BlockKind::GpuDictionary);
        std::vector<std::byte> decoded(input.size());
        REQUIRE_TRUE(!flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded,
                                             {.require_gpu = false, .force_cpu = true}));
        REQUIRE_EQ(decoded, input);
        if (level <= 6) {
            REQUIRE_TRUE(encoded.payload.size() < previous);
        } else {
            REQUIRE_TRUE(encoded.payload.size() <= previous);
        }
        previous = encoded.payload.size();
    }
}

// Purpose: Bound a fixed-width sparse-pattern candidate and prove its reconstruction byte for byte.
// Inputs: One block and a repeated motif length; every mismatch needs a 32-bit position and one literal byte.
// Outputs: Returns complete header/motif/patch bytes only if smaller than raw, otherwise no candidate.
std::optional<std::size_t> sparse_pattern_reference_bytes(std::span<const std::byte> block, std::size_t period) {
    if (period == 0U || period >= block.size() || block.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    constexpr std::size_t header_bytes = 2U * sizeof(std::uint32_t);
    constexpr std::size_t patch_bytes = sizeof(std::uint32_t) + sizeof(std::byte);
    if (header_bytes + period >= block.size()) {
        return std::nullopt;
    }
    const auto max_patches = (block.size() - header_bytes - period - 1U) / patch_bytes;
    std::vector<std::pair<std::uint32_t, std::byte>> patches;
    patches.reserve(std::min(max_patches, block.size() / period));
    for (std::size_t index = period; index < block.size(); ++index) {
        if (block[index] != block[index % period]) {
            if (patches.size() == max_patches) {
                return std::nullopt;
            }
            patches.emplace_back(static_cast<std::uint32_t>(index), block[index]);
        }
    }
    std::vector<std::byte> reconstructed(block.size());
    for (std::size_t index = 0U; index < block.size(); ++index) {
        reconstructed[index] = block[index % period];
    }
    for (const auto& [position, value] : patches) {
        reconstructed[position] = value;
    }
    REQUIRE_TRUE(std::equal(reconstructed.begin(), reconstructed.end(), block.begin()));
    return header_bytes + period + patches.size() * patch_bytes;
}

// Purpose: Find a short input's best match independently, without keys, sorting, chains, or GPU search pruning.
// Inputs: At most 128 source bytes and one valid position.
// Outputs: Returns the longest earlier equal substring, preferring the nearest distance on ties.
Match exhaustive_short_match(std::span<const std::byte> input, std::size_t position) {
    Match best;
    for (auto candidate = position; candidate > 0U;) {
        --candidate;
        std::size_t length = 0;
        while (length < input.size() - position && input[candidate + length] == input[position + length]) {
            ++length;
        }
        if (length >= kMinMatchBytes && length > best.length) {
            best.length = static_cast<std::uint16_t>(length);
            best.distance = static_cast<std::uint16_t>(position - candidate);
        }
    }
    return best;
}

// Purpose: Decode a GPU-produced block with independent, byte-at-a-time LZ4 sequence logic.
// Inputs: Encoded payload and its exact decoded size, both bounded by the experimental segment contract.
// Outputs: Returns decoded bytes and asserts complete consumption, valid offsets, and LZ4 end-of-block rules.
std::vector<std::byte> decode_reference_block(const EncodedSegment& segment) {
    const auto& encoded = segment.payload;
    std::vector<std::byte> decoded;
    std::size_t position = 0;
    std::size_t last_match_start = 0;
    bool had_match = false;
    const auto read_length = [&](std::size_t base) {
        if (base == 15U) {
            unsigned int extension = 255;
            while (extension == 255U) {
                REQUIRE_TRUE(position < encoded.size());
                extension = std::to_integer<unsigned int>(encoded[position++]);
                REQUIRE_TRUE(base <= kSegmentBytes && extension <= kSegmentBytes - base);
                base += extension;
            }
        }
        return base;
    };
    REQUIRE_TRUE(!encoded.empty());
    while (position < encoded.size()) {
        const auto token = std::to_integer<unsigned int>(encoded[position++]);
        const auto literals = read_length(token >> 4U);
        REQUIRE_TRUE(literals <= encoded.size() - position);
        REQUIRE_TRUE(literals <= segment.input_bytes - decoded.size());
        for (std::size_t index = 0; index < literals; ++index) {
            decoded.push_back(encoded[position++]);
        }
        if (position == encoded.size()) {
            REQUIRE_EQ(token & 15U, 0U);
            REQUIRE_TRUE(!had_match || (literals >= 5U && segment.input_bytes - last_match_start >= 12U));
            REQUIRE_EQ(decoded.size(), segment.input_bytes);
            return decoded;
        }
        REQUIRE_TRUE(encoded.size() - position >= 2U);
        const auto distance = std::to_integer<unsigned int>(encoded[position]) |
                              (std::to_integer<unsigned int>(encoded[position + 1U]) << 8U);
        position += 2U;
        const auto length = read_length(token & 15U) + 4U;
        REQUIRE_TRUE(distance > 0U && distance <= decoded.size());
        REQUIRE_TRUE(length <= segment.input_bytes - decoded.size());
        last_match_start = decoded.size();
        had_match = true;
        for (std::size_t index = 0; index < length; ++index) {
            decoded.push_back(decoded[decoded.size() - distance]);
        }
    }
    throw std::runtime_error("dictionary output has no final literal sequence");
}

// Purpose: Describe the first LZ4 sequence without relying on the HIP decoder.
// Inputs: One nonempty independently encoded dictionary segment.
// Outputs: Returns literal count and match distance/length, or rejects an incomplete sequence.
std::string describe_first_dictionary_sequence(const EncodedSegment& segment) {
    if (segment.payload.empty()) {
        throw std::runtime_error("dictionary segment is empty");
    }
    auto length = std::to_integer<unsigned int>(segment.payload.front()) >> 4U;
    std::size_t offset = 1U;
    if (length == 15U) {
        unsigned int extension = 255U;
        while (extension == 255U) {
            if (offset == segment.payload.size()) {
                throw std::runtime_error("dictionary literal extension is truncated");
            }
            extension = std::to_integer<unsigned int>(segment.payload[offset++]);
            length += extension;
        }
    }
    std::string result = "literals=" + std::to_string(length);
    if (length > segment.payload.size() - offset) {
        throw std::runtime_error("dictionary literals exceed their payload");
    }
    offset += length;
    if (offset == segment.payload.size()) {
        return result + " final=true";
    }
    if (segment.payload.size() - offset < 2U) {
        throw std::runtime_error("dictionary offset is truncated");
    }
    const auto distance = std::to_integer<unsigned int>(segment.payload[offset]) |
                          (std::to_integer<unsigned int>(segment.payload[offset + 1U]) << 8U);
    offset += 2U;
    auto match = std::to_integer<unsigned int>(segment.payload.front()) & 15U;
    if (match == 15U) {
        unsigned int extension = 255U;
        while (extension == 255U) {
            if (offset == segment.payload.size()) {
                throw std::runtime_error("dictionary match extension is truncated");
            }
            extension = std::to_integer<unsigned int>(segment.payload[offset++]);
            match += extension;
        }
    }
    return result + " distance=" + std::to_string(distance) + " match=" + std::to_string(match + 4U);
}

// Purpose: Preserve a complete bounded failed payload without depending on a later rerun or filesystem export.
// Inputs: One independently decoded segment and the exact source bytes it should reproduce.
// Outputs: Returns decoder diagnostics and hexadecimal payload bytes, including when the sequence is malformed.
std::string describe_dictionary_payload_failure(const EncodedSegment& segment, std::span<const std::byte> source) {
    REQUIRE_TRUE(segment.payload.size() <= kEncodedSegmentCapacity);
    std::string result = "input_bytes=" + std::to_string(segment.input_bytes) +
                         " payload_bytes=" + std::to_string(segment.payload.size());
    try {
        result += " " + describe_first_dictionary_sequence(segment);
        const auto decoded = decode_reference_block(segment);
        const auto first_bad = std::mismatch(decoded.begin(), decoded.end(), source.begin(), source.end());
        result += " first_bad=" + std::to_string(first_bad.first - decoded.begin());
    } catch (const std::exception& error) {
        result += " decoder_error=" + std::string(error.what());
    }
    constexpr std::string_view digits = "0123456789abcdef";
    result += " payload_hex=";
    for (const auto value : segment.payload) {
        const auto byte = std::to_integer<unsigned int>(value);
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 15U]);
    }
    return result;
}

// Purpose: Write one independently verified dictionary fixture without replacing an existing output.
// Inputs: `path` belongs to the opt-in export root and `bytes` holds the complete bounded fixture.
// Outputs: Publishes complete bytes atomically without following reparse parents or replacing an existing file.
void write_dictionary_interop_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    flitzzip::FilePublishTransaction transaction(path);
    std::ofstream file(transaction.staging_path(), std::ios::binary);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    if (!bytes.empty()) {
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    file.close();
    transaction.commit(false);
}

// Purpose: Export already verified raw blocks for an independent external LZ4 decoder when explicitly requested.
// Inputs: A bounded segment, its decoded bytes, and an optional harness-owned environment export directory.
// Outputs: Ordinary tests write nothing; opt-in runs emit numbered block/raw pairs, capped at 64 MiB per process.
void export_dictionary_interop_fixture(const EncodedSegment& segment, std::span<const std::byte> decoded) {
    const DWORD needed = GetEnvironmentVariableW(L"FLITZZIP_DICTIONARY_INTEROP_EXPORT", nullptr, 0);
    if (needed == 0U) {
        return;
    }
    REQUIRE_TRUE(needed <= 32768U);
    std::wstring root(needed, L'\0');
    const auto copied = GetEnvironmentVariableW(L"FLITZZIP_DICTIONARY_INTEROP_EXPORT", root.data(), needed);
    REQUIRE_TRUE(copied > 0U && copied < needed);
    root.resize(copied);
    static std::size_t exported_bytes = 0;
    static std::size_t ordinal = 0;
    const auto next_bytes = segment.payload.size() + decoded.size();
    REQUIRE_TRUE(next_bytes <= 64U * 1024U * 1024U - exported_bytes);
    const auto directory = std::filesystem::path(root);
    const auto name = std::to_string(ordinal++);
    write_dictionary_interop_file(directory / (name + ".lz4block"), segment.payload);
    write_dictionary_interop_file(directory / (name + ".raw"), decoded);
    exported_bytes += next_bytes;
}

// Purpose: Verify actual encoded bytes independently and account for compact transfers, including size metadata.
// Inputs: Original bytes and the HIP block encoder's result.
// Outputs: Requires independent CPU and HIP restoration plus bounded workspace; returns encoded payload bytes.
std::size_t require_valid_encoded_batch(std::span<const std::byte> input, const EncodedBatch& batch) {
    REQUIRE_TRUE(batch.gpu_used);
    REQUIRE_EQ(batch.segments.size(), (input.size() + kSegmentBytes - 1U) / kSegmentBytes);
    REQUIRE_TRUE(batch.device_workspace_bytes <= kMaxWorkspaceBytes);
    REQUIRE_EQ(batch.h2d_bytes, input.size());
    std::size_t payload_bytes = 0;
    std::size_t restored = 0;
    for (const auto& segment : batch.segments) {
        REQUIRE_TRUE(segment.payload.size() <= kEncodedSegmentCapacity);
        REQUIRE_EQ(segment.input_bytes, std::min(input.size() - restored, std::size_t{kSegmentBytes}));
        const auto decoded = decode_reference_block(segment);
        REQUIRE_TRUE(std::equal(decoded.begin(), decoded.end(), input.begin() + restored));
        export_dictionary_interop_fixture(segment, decoded);
        restored += decoded.size();
        payload_bytes += segment.payload.size();
    }
    REQUIRE_EQ(restored, input.size());
    REQUIRE_EQ(batch.d2h_bytes, payload_bytes + batch.segments.size() * sizeof(std::uint32_t));
    const auto gpu_decoded = decode_segments(batch.segments);
    REQUIRE_TRUE(gpu_decoded.gpu_used);
    REQUIRE_EQ(gpu_decoded.bytes.size(), input.size());
    REQUIRE_TRUE(std::equal(input.begin(), input.end(), gpu_decoded.bytes.begin()));
    REQUIRE_TRUE(gpu_decoded.device_workspace_bytes < 9U * 1024U * 1024U);
    REQUIRE_EQ(gpu_decoded.device_workspace_bytes,
               payload_bytes + input.size() + batch.segments.size() * (16U + sizeof(std::uint32_t)));
    REQUIRE_EQ(gpu_decoded.h2d_bytes, payload_bytes + batch.segments.size() * 16U);
    REQUIRE_EQ(gpu_decoded.d2h_bytes, input.size() + batch.segments.size() * sizeof(std::uint32_t));
    REQUIRE_TRUE(!gpu_decoded.decode_ms || (std::isfinite(*gpu_decoded.decode_ms) && *gpu_decoded.decode_ms >= 0.0));
    return payload_bytes;
}

// Purpose: Catch unavailable dictionary timing while independently verifying the indexed encoder's output.
// Inputs: A bounded, nonperiodic RAM corpus at low, balanced, and high effort on an available HIP device.
// Outputs: Requires finite stage measurements, exact diagnostic matches, and independent LZ4/HIP read-back.
TEST_CASE(dictionary_dispatch_bound_stage_timing) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const auto input = make_near_identical_records(4U * kSegmentBytes);
    const auto matches = find_matches(input, 5);
    require_valid_matches(input, matches, 5);
    for (const auto duration : {matches.index_ms, matches.search_ms}) {
        REQUIRE_TRUE(duration && std::isfinite(*duration) && *duration >= 0.0);
    }
    for (const int level : {1, 5, 9}) {
        const auto encoded = encode_segments(input, level);
        for (const auto duration : {encoded.index_ms, encoded.encode_ms, encoded.compact_ms, encoded.device_ms}) {
            REQUIRE_TRUE(duration && std::isfinite(*duration) && *duration >= 0.0);
        }
        REQUIRE_TRUE(require_valid_encoded_batch(input, encoded) < input.size());
        const auto stage_total = *encoded.index_ms + *encoded.encode_ms + *encoded.compact_ms;
        const auto rounding_bound = std::numeric_limits<double>::epsilon() * std::max(1.0, stage_total) * 4.0;
        REQUIRE_TRUE(std::abs(*encoded.device_ms - stage_total) <= rounding_bound);
        std::cout << "dictionary_stage_timing level=" << level << " index_ms=" << *encoded.index_ms
                  << " encode_ms=" << *encoded.encode_ms << " compact_ms=" << *encoded.compact_ms << '\n';
    }
}

// Purpose: Verify production periodic-index output with an independent LZ4 block reader.
// Inputs: Periodic 8/16 MiB and non-power-of-two 1 MiB sources at low, middle, and high required-HIP efforts.
// Outputs: Requires independently decodable dictionary blocks within the four/five-launch effort policy;
// level-five export supports an external reader.
TEST_CASE(dictionary_periodic_candidate_independent_block_decode) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    for (const auto [record_bytes, input_bytes] :
         {std::pair{16U * 1024U, 8U * 1024U * 1024U}, std::pair{12U * 1024U, 1024U * 1024U},
          std::pair{16U * 1024U, 16U * 1024U * 1024U}}) {
        const auto input = make_segmented_records(input_bytes, record_bytes);
        for (const int level : {1, 5, 9}) {
            flitzzip::GpuCodecOptions options;
            options.block_size = static_cast<std::uint32_t>(input.size());
            options.compression_level = level;
            options.require_gpu = true;
            options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
            const auto encoded = flitzzip::encode_chunk(input, options);
            const auto telemetry = flitzzip::snapshot_gpu_telemetry(*options.telemetry);
            REQUIRE_TRUE(std::isfinite(telemetry.kernel_ms) && telemetry.kernel_ms > 0.0);
            std::cout << "dictionary_periodic_launches input_bytes=" << input_bytes << " record_bytes=" << record_bytes
                      << " level=" << level << " launches=" << telemetry.kernel_launches << '\n';
            const bool measures_entropy = level == 9;
            REQUIRE_EQ(telemetry.kernel_launches, measures_entropy ? 5U : 4U);
            REQUIRE_EQ(encoded.blocks.size(), 1U);
            REQUIRE_EQ(encoded.blocks.front().kind, flitzzip::BlockKind::GpuDictionary);
            REQUIRE_EQ(encoded.blocks.front().encoded_offset, 0U);
            REQUIRE_EQ(encoded.blocks.front().encoded_len, encoded.payload.size());
            const auto spans =
                flitzzip::parse_dictionary_segments(encoded.payload, static_cast<std::uint32_t>(input.size()));
            for (const auto& span : spans) {
                EncodedSegment segment;
                segment.input_bytes = span.decoded_size;
                segment.payload.assign(encoded.payload.begin() + span.encoded_offset,
                                       encoded.payload.begin() + span.encoded_offset + span.encoded_size);
                const auto decoded = decode_reference_block(segment);
                REQUIRE_TRUE(std::equal(decoded.begin(), decoded.end(), input.begin() + span.decoded_offset));
                if (level == 5) {
                    export_dictionary_interop_fixture(segment, decoded);
                }
            }
        }
    }
}

// Purpose: Verify that one HIP batch can use different sampled distances in adjacent dictionary segments.
// Inputs: Alternating 12 and 16 KiB records in independently seeded 64 KiB segments.
// Outputs: Requires the periodic path and exact independent LZ4, CPU, and HIP decoding.
TEST_CASE(dictionary_mixed_periodic_distances_roundtrip) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    constexpr std::size_t kInputBytes = 1024U * 1024U;
    auto input = make_segmented_records(kInputBytes, 16U * 1024U);
    const auto non_power_input = make_segmented_records(kInputBytes, 12U * 1024U);
    for (std::size_t segment = 1U; segment < kInputBytes / kSegmentBytes; segment += 2U) {
        const auto offset = segment * kSegmentBytes;
        std::copy_n(non_power_input.begin() + static_cast<std::ptrdiff_t>(offset), kSegmentBytes,
                    input.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    flitzzip::GpuCodecOptions options;
    options.block_size = static_cast<std::uint32_t>(kInputBytes);
    options.compression_level = 5;
    options.require_gpu = true;
    options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
    const auto encoded = flitzzip::encode_chunk(input, options);
    REQUIRE_EQ(encoded.blocks.size(), 1U);
    REQUIRE_EQ(encoded.blocks.front().kind, flitzzip::BlockKind::GpuDictionary);
    REQUIRE_EQ(flitzzip::snapshot_gpu_telemetry(*options.telemetry).kernel_launches, 4U);
    const auto spans = flitzzip::parse_dictionary_segments(encoded.payload, static_cast<std::uint32_t>(input.size()));
    for (const auto& span : spans) {
        EncodedSegment segment;
        segment.input_bytes = span.decoded_size;
        segment.payload.assign(encoded.payload.begin() + span.encoded_offset,
                               encoded.payload.begin() + span.encoded_offset + span.encoded_size);
        const auto decoded = decode_reference_block(segment);
        REQUIRE_TRUE(std::equal(decoded.begin(), decoded.end(), input.begin() + span.decoded_offset));
    }
    for (const bool hip : {false, true}) {
        auto decode_options = options;
        decode_options.require_gpu = hip;
        decode_options.force_cpu = !hip;
        std::vector<std::byte> decoded(input.size());
        REQUIRE_EQ(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, decode_options), hip);
        REQUIRE_EQ(decoded, input);
    }
}

// Purpose: Pack diagnostic matches with independent serial selection to check cooperative encoder equivalence.
// Inputs: One source segment and its position-aligned, previously validated diagnostic matches.
// Outputs: Returns exact LZ4 payload bytes without using GPU packing or cache selection logic.
std::vector<std::byte> encode_reference_matches(std::span<const std::byte> input, std::span<const Match> matches) {
    REQUIRE_EQ(input.size(), matches.size());
    std::vector<std::byte> output;
    const auto extend = [&](std::size_t length) {
        if (length < 15U) {
            return;
        }
        length -= 15U;
        while (length >= 255U) {
            output.push_back(std::byte{255});
            length -= 255U;
        }
        output.push_back(static_cast<std::byte>(length));
    };
    std::size_t cursor = 0;
    while (true) {
        auto next = cursor;
        while (next < input.size() && input.size() - next >= 12U && matches[next].length < kMinMatchBytes) {
            ++next;
        }
        const bool last = input.size() - next < 12U;
        if (last) {
            next = input.size();
        }
        const auto literals = next - cursor;
        const auto length = last ? 0U : std::min<std::size_t>(matches[next].length, input.size() - next - 5U);
        const auto code = last ? 0U : length - kMinMatchBytes;
        output.push_back(
            static_cast<std::byte>((std::min<std::size_t>(literals, 15U) << 4U) | std::min<std::size_t>(code, 15U)));
        extend(literals);
        output.insert(output.end(), input.begin() + cursor, input.begin() + next);
        if (last) {
            return output;
        }
        output.push_back(static_cast<std::byte>(matches[next].distance & 255U));
        output.push_back(static_cast<std::byte>(matches[next].distance >> 8U));
        extend(code);
        cursor = next + length;
    }
}

#if FLITZZIP_ENABLE_HIP
// Purpose: Separate changed device inputs from encoder failures while the failed call still owns its buffers.
// Inputs: An initialized device allocation and its immutable CPU oracle of the same bounded extent.
// Outputs: Reports a transfer error or first differing record without replacing the original payload diagnostic.
template <typename T> std::string describe_dictionary_device_difference(const T* device, std::span<const T> expected) {
    std::vector<T> actual(expected.size());
    const auto status =
        flitzzip::hip_detail::copy_on_codec_stream(actual.data(), device, expected.size_bytes(), hipMemcpyDeviceToHost);
    if (status != hipSuccess) {
        return "copy_error=" + std::to_string(static_cast<int>(status));
    }
    const auto mismatch = std::mismatch(actual.begin(), actual.end(), expected.begin(), expected.end());
    if (mismatch.first == actual.end()) {
        return "unchanged";
    }
    return "first_bad=" + std::to_string(mismatch.first - actual.begin()) +
           " expected=" + std::to_string(static_cast<std::uint64_t>(*mismatch.second)) +
           " actual=" + std::to_string(static_cast<std::uint64_t>(*mismatch.first));
}

// Purpose: Build exact-prefix predecessor links independently of the GPU sort and link kernels.
// Inputs: A bounded source split into the production segment extent.
// Outputs: The nearest earlier equal four-byte prefix, or a sentinel including each segment's final three bytes.
std::vector<std::uint32_t> dictionary_predecessor_oracle(std::span<const std::byte> input) {
    std::vector<std::uint32_t> result(input.size(), std::numeric_limits<std::uint32_t>::max());
    for (std::size_t start = 0; start < input.size(); start += kSegmentBytes) {
        std::unordered_map<std::uint32_t, std::uint32_t> latest;
        const auto end = std::min(input.size(), start + kSegmentBytes);
        for (auto position = start; end - position >= kMinMatchBytes; ++position) {
            std::uint32_t key = 0U;
            for (unsigned byte = 0; byte < kMinMatchBytes; ++byte) {
                key |= std::to_integer<std::uint32_t>(input[position + byte]) << (8U * byte);
            }
            const auto [entry, inserted] = latest.try_emplace(key, static_cast<std::uint32_t>(position));
            if (!inserted) {
                result[position] = entry->second;
                entry->second = static_cast<std::uint32_t>(position);
            }
        }
    }
    return result;
}

// Purpose: Isolate production GPU indexing from the encoder with exact independent links and output guards.
// Inputs: Immutable source and independently computed predecessor records; each call owns its stream's buffers.
// Outputs: Requires every initialized link to match and every omitted final-byte record and guard to stay untouched.
void require_dictionary_device_index(std::span<const std::byte> input, std::span<const std::uint32_t> oracle) {
    using namespace flitzzip::hip_detail;
    const auto& api = flitzzip::hip_kernel_api();
    const auto size = static_cast<std::uint32_t>(input.size());
    const auto grid = (size + kDictionaryKernelThreads - 1U) / kDictionaryKernelThreads;
    const auto keys_bytes = input.size() * sizeof(std::uint64_t);
    const auto previous_bytes = (input.size() + 32U) * sizeof(std::uint32_t);
    std::size_t scratch_bytes = 0;
    check_hip(api.sort_dictionary_keys(nullptr, &scratch_bytes, nullptr, nullptr, size, hipStreamPerThread),
              "query index oracle workspace");
    HipDeviceMemoryReservation reservation(input.size() + 2U * keys_bytes + previous_bytes + scratch_bytes,
                                           "dictionary index oracle");
    HipDeviceBuffer<std::byte> source(input.size(), "allocate index oracle source");
    HipDeviceBuffer<std::uint64_t> keys(keys_bytes, "allocate index oracle keys");
    HipDeviceBuffer<std::uint64_t> sorted(keys_bytes, "allocate index oracle sorted keys");
    HipDeviceBuffer<std::uint32_t> previous(previous_bytes, "allocate index oracle links");
    HipDeviceBuffer<std::byte> scratch(std::max<std::size_t>(scratch_bytes, 1U), "allocate index oracle workspace");
    check_hip(copy_on_codec_stream(source.get(), input.data(), input.size(), hipMemcpyHostToDevice),
              "upload index oracle source");
    check_hip(hipMemsetAsync(previous.get(), 0xA5, previous_bytes, hipStreamPerThread), "guard index oracle links");
    auto events = make_hip_event_pair("create index oracle events");
    launch_kernel_with_timing(api.build_dictionary_keys, grid, kDictionaryKernelThreads, 0U, hipStreamPerThread,
                              events.start, nullptr, "build index oracle keys", source.get(), size, keys.get());
    check_hip(
        api.sort_dictionary_keys(scratch.get(), &scratch_bytes, keys.get(), sorted.get(), size, hipStreamPerThread),
        "sort index oracle keys");
    launch_kernel_with_timing(api.link_dictionary_predecessors, grid, kDictionaryKernelThreads, 0U, hipStreamPerThread,
                              nullptr, events.stop, "link index oracle keys", sorted.get(), size, previous.get());
    check_hip(hipEventSynchronize(events.stop), "complete index oracle kernels");
    std::vector<std::uint32_t> actual(input.size() + 32U);
    check_hip(copy_on_codec_stream(actual.data(), previous.get(), previous_bytes, hipMemcpyDeviceToHost),
              "download index oracle links");
    for (std::size_t position = 0; position < actual.size(); ++position) {
        const auto end = std::min(input.size(), (position / kSegmentBytes + 1U) * kSegmentBytes);
        const auto expected = position < end && end - position >= kMinMatchBytes ? oracle[position] : 0xA5A5A5A5U;
        if (actual[position] != expected) {
            throw std::runtime_error("dictionary index differs at " + std::to_string(position) + " expected " +
                                     std::to_string(expected) + " actual " + std::to_string(actual[position]));
        }
    }
}

// Purpose: Isolate the production dictionary encoder from GPU indexing with a CPU-built exact prefix table.
// Inputs: Immutable source, independent predecessor records and an ordinary effort; each call owns its allocations.
// Outputs: Independent decoding must reproduce every byte and writes must remain inside each declared payload.
void require_dictionary_encoder_from_oracle(std::span<const std::byte> input, std::span<const std::uint32_t> oracle,
                                            int level) {
    using namespace flitzzip::hip_detail;
    const auto& api = flitzzip::hip_kernel_api();
    const auto count = (input.size() + kSegmentBytes - 1U) / kSegmentBytes;
    const auto output_bytes = count * kEncodedSegmentCapacity + 128U;
    const auto sizes_bytes = (count + 1U) * sizeof(std::uint32_t);
    HipDeviceMemoryReservation reservation(input.size() + oracle.size_bytes() + output_bytes + sizes_bytes,
                                           "dictionary encoder oracle");
    HipDeviceBuffer<std::byte> source(input.size(), "allocate encoder oracle source");
    HipDeviceBuffer<std::uint32_t> previous(oracle.size_bytes(), "allocate encoder oracle links");
    HipDeviceBuffer<std::byte> output(output_bytes, "allocate encoder oracle output");
    HipDeviceBuffer<std::uint32_t> sizes(sizes_bytes, "allocate encoder oracle sizes");
    check_hip(copy_on_codec_stream(source.get(), input.data(), input.size(), hipMemcpyHostToDevice),
              "upload encoder oracle source");
    check_hip(copy_on_codec_stream(previous.get(), oracle.data(), oracle.size_bytes(), hipMemcpyHostToDevice),
              "upload encoder oracle links");
    check_hip(hipMemsetAsync(output.get(), 0xA5, output_bytes, hipStreamPerThread), "guard encoder oracle output");
    check_hip(hipMemsetAsync(sizes.get(), 0xA5, sizes_bytes, hipStreamPerThread), "guard encoder oracle sizes");
    auto events = make_hip_event_pair("create encoder oracle events");
    const auto kernel = level <= 3 ? api.encode_dictionary_cached : api.encode_dictionary_deferred;
    launch_measured_kernel(kernel, static_cast<unsigned>(count), kDictionaryKernelThreads, 0U, hipStreamPerThread,
                           events, "encode from oracle links", source.get(), static_cast<std::uint32_t>(input.size()),
                           previous.get(), nullptr, effort_for_level(level), output.get(), sizes.get());
    check_hip(hipEventSynchronize(events.stop), "complete encoder oracle kernel");
    std::vector<std::byte> payload(output_bytes);
    std::vector<std::uint32_t> lengths(count + 1U);
    check_hip(copy_on_codec_stream(payload.data(), output.get(), payload.size(), hipMemcpyDeviceToHost),
              "download encoder oracle payload");
    check_hip(copy_on_codec_stream(lengths.data(), sizes.get(), sizes_bytes, hipMemcpyDeviceToHost),
              "download encoder oracle sizes");
    REQUIRE_EQ(lengths.back(), 0xA5A5A5A5U);
    const auto guarded = [](std::byte value) { return value == std::byte{0xA5}; };
    for (std::size_t segment = 0; segment < count; ++segment) {
        REQUIRE_TRUE(lengths[segment] > 0U && lengths[segment] <= kEncodedSegmentCapacity);
        const auto begin = payload.begin() + segment * kEncodedSegmentCapacity;
        const auto input_bytes = std::min(input.size() - segment * kSegmentBytes, std::size_t{kSegmentBytes});
        EncodedSegment encoded{{begin, begin + lengths[segment]}, static_cast<std::uint32_t>(input_bytes)};
        const auto source_bytes = input.subspan(segment * kSegmentBytes, input_bytes);
        bool agrees = false;
        try {
            const auto decoded = decode_reference_block(encoded);
            agrees = std::equal(decoded.begin(), decoded.end(), source_bytes.begin(), source_bytes.end());
        } catch (const std::exception&) {
            // The failure report below retains malformed bytes and the live device boundaries as well.
        }
        if (!agrees) {
            throw std::runtime_error("dictionary encoder differs with CPU links at segment " + std::to_string(segment) +
                                     " level " + std::to_string(level) + " device_input " +
                                     describe_dictionary_device_difference(source.get(), input) + " device_links " +
                                     describe_dictionary_device_difference(previous.get(), oracle) + " " +
                                     describe_dictionary_payload_failure(encoded, source_bytes));
        }
        REQUIRE_TRUE(std::all_of(begin + lengths[segment], begin + kEncodedSegmentCapacity, guarded));
    }
    REQUIRE_TRUE(std::all_of(payload.end() - 128U, payload.end(), guarded));
}
#endif

// Purpose: Separate exact-prefix indexing and LZ4 encoding failures under overlapping production HIP streams.
// Inputs: Four callers share immutable 4 MiB source/oracle bytes while owning their device work and small output
// guards. Outputs: Every GPU link and independently decoded cached/deferred payload agrees with the CPU oracle.
TEST_CASE(dictionary_index_and_encoder_independent_concurrent_oracles) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> input(kMaxBatchBytes);
    for (std::size_t index = 0; index < input.size(); ++index) {
        std::uint64_t value = index % 16384U + 0x9E3779B97F4A7C15ULL;
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        input[index] = static_cast<std::byte>((value ^ (value >> 31U)) >> 56U);
    }
    const auto oracle = dictionary_predecessor_oracle(input);
    std::vector<std::future<void>> pending;
    pending.reserve(4U);
    std::promise<void> start;
    const auto ready = start.get_future().share();
    for (unsigned worker = 0; worker < 4U; ++worker) {
        pending.push_back(std::async(std::launch::async, [&input, &oracle, ready] {
            ready.get();
            for (unsigned iteration = 0; iteration < 4U; ++iteration) {
                require_dictionary_device_index(input, oracle);
                for (const int level : {1, 5, 9}) {
                    require_dictionary_encoder_from_oracle(input, oracle, level);
                }
            }
        }));
    }
    start.set_value();
    for (auto& task : pending) {
        task.get();
    }
#endif
}

}  // namespace

// Purpose: Reject export roots redirected through a directory junction before creating fixture files.
// Inputs: A harness-owned export junction targeting a separate harness-owned directory.
// Outputs: Requires rejection and no file in the redirected directory; removes the junction on every exit.
TEST_CASE(dictionary_interop_export_rejects_reparse_parent) {
    const auto root = test_temp_dir("dictionary-export-root");
    const auto outside = test_temp_dir("dictionary-export-outside");
    const auto junction = root / "linked";
    if (!flitzzip_test::try_create_test_directory_junction(junction, outside)) {
        std::cout << "[SKIP] directory junction creation is unavailable on this filesystem\n";
        return;
    }
    struct JunctionCleanup {
        std::filesystem::path path;
        // Purpose: Remove the owned junction; Inputs: saved junction path; Outputs: never follows its target.
        ~JunctionCleanup() {
            flitzzip_test::remove_test_directory_junction(path);
        }
    } cleanup{junction};
    const std::array<std::byte, 4> bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    bool rejected = false;
    try {
        write_dictionary_interop_file(junction / "nested" / "fixture.raw", bytes);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(std::filesystem::is_empty(outside));
}

// Purpose: Preserve complete fixture bytes, reject replacement, and remove private staging after each outcome.
// Inputs: Binary and empty fixtures in a new nested directory, then a conflicting write to the binary path.
// Outputs: Requires exact original bytes, an empty fixture, overwrite refusal, and no remaining staging entries.
TEST_CASE(dictionary_interop_export_preserves_bytes_and_refuses_overwrite) {
    const auto root = test_temp_dir("dictionary-export-publication");
    const auto path = root / "nested" / "fixture.raw";
    constexpr std::string_view contents{"fixture\0bytes", sizeof("fixture\0bytes") - 1U};
    write_dictionary_interop_file(path, std::as_bytes(std::span(contents.data(), contents.size())));
    bool rejected = false;
    try {
        write_dictionary_interop_file(path, {});
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::ifstream input(path, std::ios::binary);
    const std::string actual(std::istreambuf_iterator<char>(input), {});
    REQUIRE_EQ(actual, contents);
    write_dictionary_interop_file(path.parent_path() / "empty.raw", {});
    REQUIRE_EQ(std::filesystem::file_size(path.parent_path() / "empty.raw"), 0U);
    REQUIRE_EQ(
        std::distance(std::filesystem::directory_iterator(path.parent_path()), std::filesystem::directory_iterator()),
        2);
}

// Purpose: Ensure two exporting callers can never both replace the same numbered fixture.
// Inputs: Four pairs of concurrent writes with distinct complete payloads and one shared final target per pair.
// Outputs: Requires exactly one winner per target, one explicit refusal, intact winner bytes, and staging cleanup.
TEST_CASE(dictionary_interop_export_concurrent_writers_do_not_overwrite) {
    const auto root = test_temp_dir("dictionary-export-concurrent");
    constexpr std::array<std::string_view, 2> payloads{"first verified fixture", "second verified fixture"};
    for (unsigned int iteration = 0; iteration < 4U; ++iteration) {
        const auto path = root / std::to_string(iteration) / "fixture.raw";
        std::atomic<unsigned int> committed{0};
        std::atomic<unsigned int> refused{0};
        std::array<std::exception_ptr, 2> unexpected{};
        const auto attempt = [&](std::size_t index) {
            try {
                const auto payload = payloads[index];
                write_dictionary_interop_file(path, std::as_bytes(std::span(payload.data(), payload.size())));
                ++committed;
            } catch (const flitzzip::SecurityError&) {
                ++refused;
            } catch (...) {
                unexpected[index] = std::current_exception();
            }
        };
        std::jthread first(attempt, 0U);
        std::jthread second(attempt, 1U);
        first.join();
        second.join();
        for (const auto& error : unexpected) {
            if (error) {
                std::rethrow_exception(error);
            }
        }
        REQUIRE_EQ(committed.load(), 1U);
        REQUIRE_EQ(refused.load(), 1U);
        std::ifstream input(path, std::ios::binary);
        const std::string actual(std::istreambuf_iterator<char>(input), {});
        REQUIRE_TRUE(actual == payloads[0] || actual == payloads[1]);
        REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(path.parent_path()),
                                 std::filesystem::directory_iterator()),
                   1);
    }
}

// Purpose: Preserve reference encoding semantics without a global match table in the production encoder.
// Inputs: All nine efforts and seeded mixed data across cache and segment boundaries, plus the depth fixture.
// Outputs: Requires byte-exact reference packing, CPU/HIP roundtrips, and exact workspace savings.
TEST_CASE(dictionary_encoder_agrees_with_dense_reference) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::vector<std::byte>> fixtures{make_dictionary_depth_fixture()};
    fixtures.emplace_back(2U * kSegmentBytes + 269U);
    auto& mixed = fixtures.back();
    std::uint32_t state = 0x179BC341U;
    for (std::size_t index = 0; index < mixed.size(); ++index) {
        state = state * 1664525U + 1013904223U;
        mixed[index] = static_cast<std::byte>(index % 8191U < 4501U ? state >> 24U : index % 13U);
    }
    for (const auto& input : fixtures) {
        for (int level = 1; level <= 9; ++level) {
            const auto dense = find_matches(input, level);
            require_valid_matches(input, dense, level);
            const auto encoded = encode_segments(input, level);
            (void)require_valid_encoded_batch(input, encoded);
            for (std::size_t index = 0; index < encoded.segments.size(); ++index) {
                const auto offset = index * kSegmentBytes;
                const auto size = encoded.segments[index].input_bytes;
                const auto expected = encode_reference_matches(std::span{input}.subspan(offset, size),
                                                               std::span{dense.matches}.subspan(offset, size));
                REQUIRE_TRUE(encoded.segments[index].payload == expected);
            }
            const auto output_workspace = encoded.segments.size() * (kEncodedSegmentCapacity + sizeof(std::uint32_t));
            REQUIRE_EQ(encoded.device_workspace_bytes,
                       dense.device_workspace_bytes - input.size() * sizeof(Match) + output_workspace);
        }
    }
}

// Purpose: Preserve independent encoding at short-tail, cache, and segment boundaries.
// Inputs: Deterministic small-alphabet and full-alphabet fixtures at three search efforts, entirely in RAM.
// Outputs: Requires exact independent CPU/HIP decoding of each result.
TEST_CASE(dictionary_encoder_preserves_edge_cases) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    for (const std::size_t size : {1U, 3U, 4U, 11U, 12U, 13U, 255U, 256U, 257U, 65535U, 65536U, 65537U}) {
        for (const unsigned int alphabet : {3U, 256U}) {
            std::vector<std::byte> input(size);
            std::uint32_t state = 0xF7A123C9U;
            for (auto& byte : input) {
                state = state * 1664525U + 1013904223U;
                byte = static_cast<std::byte>((state >> 24U) % alphabet);
            }
            for (const int level : {1, 5, 9}) {
                (void)require_valid_encoded_batch(input, encode_segments(input, level));
            }
        }
    }
}

// Purpose: Preserve bounded workspace and byte correctness at former search-size boundaries.
// Inputs: Former crossover boundaries at levels seven through nine, within the 4 MiB batch limit.
// Outputs: Every result decodes exactly and stays below the declared GPU workspace cap.
TEST_CASE(dictionary_encoder_workspace_boundaries) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    for (const int level : {7, 8, 9}) {
        const std::size_t threshold = level == 7 ? 4194304U : level == 8 ? 2097152U : 1048576U;
        for (const auto size : {threshold - 1U, threshold}) {
            std::vector<std::byte> input(size);
            for (std::size_t index = 0; index < size; ++index) {
                input[index] = static_cast<std::byte>(index % 13U);
            }
            (void)require_valid_encoded_batch(input, encode_segments(input, level));
        }
    }
}

// Purpose: Measure the actual experimental encoder only when a resource-monitoring harness explicitly opts in.
// Inputs: FLITZZIP_DICTIONARY_BENCHMARK=1; deterministic RAM-only profiles, efforts, and batch sizes.
// Outputs: Prints host-wall encoding time and real resource/size counters after independent CPU/HIP verification.
TEST_CASE(dictionary_encoder_benchmark_opt_in) {
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"FLITZZIP_DICTIONARY_BENCHMARK", enabled, 2U) != 1U || enabled[0] != L'1') {
        return;
    }
    REQUIRE_EQ(GetEnvironmentVariableW(L"FLITZZIP_DICTIONARY_INTEROP_EXPORT", nullptr, 0U), 0U);
    REQUIRE_TRUE(flitzzip::query_gpu_info().available);
    const bool extended =
        GetEnvironmentVariableW(L"FLITZZIP_DICTIONARY_BENCHMARK_EXTENDED", enabled, 2U) == 1U && enabled[0] == L'1';
    const std::vector<std::size_t> sizes =
        extended ? std::vector<std::size_t>{65536U, 131072U, 524288U, 1048576U, 2097152U, 4194304U}
                 : std::vector<std::size_t>{kSegmentBytes, kMaxBatchBytes};
    const std::vector<int> levels = extended ? std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9} : std::vector<int>{1, 5, 9};
    for (const auto size : sizes) {
        for (const auto profile : {0, 1, 2, 3}) {
            std::vector<std::byte> input(size);
            if (profile == 3) {
                input = make_segmented_records(size);
            }
            std::uint32_t state = 0x5B913C27U;
            for (std::size_t index = 0; profile != 3 && index < size; ++index) {
                state ^= state << 13U;
                state ^= state >> 17U;
                state ^= state << 5U;
                input[index] =
                    profile == 2 ? static_cast<std::byte>(index % 13U) : static_cast<std::byte>(state & 0xFFU);
                if (profile == 1 && index >= 16384U) {
                    input[index] = input[index % 16384U];
                }
            }
            const char* label = profile == 0   ? "Random"
                                : profile == 1 ? "RepeatedRecord"
                                : profile == 2 ? "Periodic13"
                                               : "SegmentedRecords";
            for (const int level : levels) {
                (void)encode_segments(input, level);
                const auto started = std::chrono::steady_clock::now();
                const auto encoded = encode_segments(input, level);
                const auto milliseconds =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                const auto bytes = require_valid_encoded_batch(input, encoded);
                std::cout << "dictionary_benchmark profile=" << label << " input_bytes=" << input.size()
                          << " level=" << level << " payload_bytes=" << bytes << " encode_wall_ms=" << milliseconds
                          << " workspace_bytes=" << encoded.device_workspace_bytes << " h2d_bytes=" << encoded.h2d_bytes
                          << " d2h_bytes=" << encoded.d2h_bytes
                          << " index_ms=" << (encoded.index_ms ? std::to_string(*encoded.index_ms) : "unavailable")
                          << " encode_ms=" << (encoded.encode_ms ? std::to_string(*encoded.encode_ms) : "unavailable")
                          << " compact_ms="
                          << (encoded.compact_ms ? std::to_string(*encoded.compact_ms) : "unavailable")
                          << " search=tiled"
                          << " gpu_used=true timing_scope=host_encode memory_only=true disk_write_bytes=0\n";
            }
        }
    }
}

// Purpose: Compare production CPU/HIP encoding on the same dictionary-friendly RAM-only source.
// Inputs: Explicit opt-in, near-identical seeded 16 KiB records, equal effort/block settings, and alternating lanes.
// Outputs: Prints median host times and exact sizes after byte-exact decode and GPU-native selection.
TEST_CASE(dictionary_production_benchmark_opt_in) {
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"FLITZZIP_DICTIONARY_PRODUCTION_BENCHMARK", enabled, 2U) != 1U || enabled[0] != L'1') {
        return;
    }
    REQUIRE_TRUE(flitzzip::query_gpu_info().available);
    const auto input = make_near_identical_records(kMaxBatchBytes);
    std::uint32_t state = 0x31674325U;
    for (const int level : {1, 5, 9}) {
        std::array<std::array<double, 3>, 2> times{};
        std::array<std::size_t, 2> sizes{};
        std::size_t dictionary_blocks = 0U;
        std::size_t sparse_blocks = 0U;
        for (int iteration = -1; iteration < 3; ++iteration) {
            for (int order = 0; order < 2; ++order) {
                const bool hip = (order + std::max(iteration, 0)) % 2 != 0;
                flitzzip::GpuCodecOptions options;
                options.block_size = 1024U * 1024U;
                options.compression_level = level;
                options.force_cpu = !hip;
                options.require_gpu = hip;
                const auto started = std::chrono::steady_clock::now();
                const auto encoded = flitzzip::encode_chunk(input, options);
                const auto milliseconds =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                std::vector<std::byte> decoded(input.size());
                REQUIRE_EQ(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options), hip);
                REQUIRE_EQ(decoded, input);
                if (iteration >= 0) {
                    times[hip][iteration] = milliseconds;
                    sizes[hip] = encoded.payload.size();
                    if (hip) {
                        dictionary_blocks = static_cast<std::size_t>(
                            std::ranges::count_if(encoded.blocks, [](const flitzzip::BlockDescriptor& block) {
                                return block.kind == flitzzip::BlockKind::GpuDictionary;
                            }));
                        sparse_blocks = static_cast<std::size_t>(
                            std::ranges::count_if(encoded.blocks, [](const flitzzip::BlockDescriptor& block) {
                                return block.kind == flitzzip::BlockKind::GpuSparsePattern;
                            }));
                        REQUIRE_TRUE(dictionary_blocks + sparse_blocks > 0U);
                    }
                }
            }
        }
        for (auto& lane : times) {
            std::sort(lane.begin(), lane.end());
        }
        std::cout << "dictionary_production_case level=" << level << " input_bytes=" << input.size()
                  << " cpu_payload_bytes=" << sizes[0] << " gpu_payload_bytes=" << sizes[1]
                  << " gpu_dictionary_blocks=" << dictionary_blocks << " gpu_sparse_pattern_blocks=" << sparse_blocks
                  << " cpu_encode_median_ms=" << times[0][1] << " gpu_encode_median_ms=" << times[1][1]
                  << " memory_only=true disk_write_bytes=0\n";
        const auto sample_started = std::chrono::steady_clock::now();
        const auto sample = encode_segments(input, level);
        const auto sample_wall_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sample_started).count();
        std::cout << "dictionary_stage_case level=" << level << " sample_bytes=" << input.size()
                  << " wall_ms=" << sample_wall_ms
                  << " index_ms=" << (sample.index_ms ? std::to_string(*sample.index_ms) : "unavailable")
                  << " encode_ms=" << (sample.encode_ms ? std::to_string(*sample.encode_ms) : "unavailable")
                  << " compact_ms=" << (sample.compact_ms ? std::to_string(*sample.compact_ms) : "unavailable")
                  << " memory_only=true disk_write_bytes=0\n";
    }
    std::vector<std::byte> nonrepeating(input.size());
    for (std::size_t index = 0; index < nonrepeating.size(); ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        nonrepeating[index] = static_cast<std::byte>(state >> 24U);
    }
    flitzzip::GpuCodecOptions baseline_options;
    baseline_options.compression_level = 5;
    baseline_options.block_size = 1024U * 1024U;
    std::array<double, 3> baseline_times{};
    for (int iteration = -1; iteration < 3; ++iteration) {
        const auto started = std::chrono::steady_clock::now();
        const auto encoded = flitzzip::encode_chunk(nonrepeating, baseline_options);
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started);
        REQUIRE_EQ(encoded.payload, nonrepeating);
        if (iteration >= 0) {
            baseline_times[iteration] = elapsed.count();
        }
    }
    std::sort(baseline_times.begin(), baseline_times.end());
    std::cout << "dictionary_pipeline_control input_bytes=" << nonrepeating.size()
              << " gpu_nonrepeating_median_ms=" << baseline_times[1] << " memory_only=true disk_write_bytes=0\n";
}

// Purpose: Measure the independent-block ratio floor before choosing a future GPU dictionary segment geometry.
// Inputs: Opt-in 4 MiB near-identical records and pinned LZ4 blocks from 64 KiB through 1 MiB.
// Outputs: Prints exact payload/table sizes only after independent LZ4 roundtrips; changes no archive behavior.
TEST_CASE(dictionary_segment_size_reference_opt_in) {
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"FLITZZIP_DICTIONARY_SEGMENT_RESEARCH", enabled, 2U) != 1U || enabled[0] != L'1') {
        return;
    }
    constexpr std::size_t block_bytes = 1024U * 1024U;
    const auto input = make_near_identical_records(kMaxBatchBytes);
    for (const std::size_t segment_bytes : {65536U, 131072U, 262144U, 524288U, 1048576U}) {
        const auto segment_size = static_cast<int>(segment_bytes);
        std::vector<char> compressed(static_cast<std::size_t>(LZ4_compressBound(segment_size)));
        std::vector<char> decoded(segment_bytes);
        std::size_t payload_bytes = 0U;
        for (std::size_t offset = 0; offset < input.size(); offset += segment_bytes) {
            const auto encoded =
                LZ4_compress_default(reinterpret_cast<const char*>(input.data() + offset), compressed.data(),
                                     segment_size, static_cast<int>(compressed.size()));
            REQUIRE_TRUE(encoded > 0);
            REQUIRE_EQ(LZ4_decompress_safe(compressed.data(), decoded.data(), encoded, segment_size), segment_size);
            REQUIRE_EQ(std::memcmp(decoded.data(), input.data() + offset, segment_bytes), 0);
            payload_bytes += static_cast<std::size_t>(encoded);
        }
        const auto table_bytes =
            (input.size() / block_bytes) * (block_bytes / segment_bytes + 1U) * sizeof(std::uint32_t);
        std::cout << "dictionary_segment_reference segment_bytes=" << segment_bytes << " input_bytes=" << input.size()
                  << " payload_bytes=" << payload_bytes << " table_bytes=" << table_bytes
                  << " total_bytes=" << payload_bytes + table_bytes << " memory_only=true disk_write_bytes=0\n";
    }
}

// Purpose: Screen sparse repeated-record and wide-LZ4 layouts against patch density and random controls.
// Inputs: Explicit opt-in, four 1 MiB blocks of seeded 16 KiB records, and deterministic extra mutations.
// Outputs: Prints candidate and actual CPU/HIP sizes after byte-exact reconstruction; writes no files.
TEST_CASE(dictionary_sparse_pattern_reference_opt_in) {
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"FLITZZIP_DICTIONARY_SPARSE_RESEARCH", enabled, 2U) != 1U || enabled[0] != L'1') {
        return;
    }
    REQUIRE_TRUE(flitzzip::query_gpu_info().available);
    constexpr std::size_t record_bytes = 16384U;
    constexpr std::size_t block_bytes = 1024U * 1024U;
    for (const auto extra_patches : {0U, 15U, 255U}) {
        auto input = make_near_identical_records(kMaxBatchBytes);
        for (std::size_t record = 1U; record < input.size() / record_bytes; ++record) {
            for (std::uint32_t patch = 0U; patch < extra_patches; ++patch) {
                const auto offset = record * record_bytes + 2048U + patch * 31U;
                input[offset] = static_cast<std::byte>((record + patch) & 255U);
            }
        }
        std::size_t total_bytes = 0U;
        std::size_t actual_patches = 0U;
        std::size_t wide_lz4_bytes = 0U;
        std::vector<char> wide_compressed(static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(block_bytes))));
        std::vector<char> wide_decoded(block_bytes);
        for (std::size_t offset = 0U; offset < input.size(); offset += block_bytes) {
            const auto candidate =
                sparse_pattern_reference_bytes(std::span(input).subspan(offset, block_bytes), record_bytes);
            REQUIRE_TRUE(candidate.has_value());
            total_bytes += *candidate;
            actual_patches +=
                (*candidate - 2U * sizeof(std::uint32_t) - record_bytes) / (sizeof(std::uint32_t) + sizeof(std::byte));
            const auto lz4_size =
                LZ4_compress_default(reinterpret_cast<const char*>(input.data() + offset), wide_compressed.data(),
                                     static_cast<int>(block_bytes), static_cast<int>(wide_compressed.size()));
            REQUIRE_TRUE(lz4_size > 0);
            REQUIRE_EQ(LZ4_decompress_safe(wide_compressed.data(), wide_decoded.data(), lz4_size,
                                           static_cast<int>(block_bytes)),
                       static_cast<int>(block_bytes));
            REQUIRE_EQ(std::memcmp(wide_decoded.data(), input.data() + offset, block_bytes), 0);
            wide_lz4_bytes += static_cast<std::size_t>(lz4_size) + 2U * sizeof(std::uint32_t);
        }
        std::array<std::size_t, 2> production_bytes{};
        for (const bool hip : {false, true}) {
            flitzzip::GpuCodecOptions options;
            options.block_size = static_cast<std::uint32_t>(block_bytes);
            options.compression_level = 5;
            options.force_cpu = !hip;
            options.require_gpu = hip;
            const auto encoded = flitzzip::encode_chunk(input, options);
            std::vector<std::byte> decoded(input.size());
            REQUIRE_EQ(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options), hip);
            REQUIRE_EQ(decoded, input);
            production_bytes[hip] = encoded.payload.size();
            if (hip) {
                REQUIRE_EQ(encoded.blocks.size(), input.size() / block_bytes);
                for (const auto& block : encoded.blocks) {
                    REQUIRE_EQ(block.kind, flitzzip::BlockKind::GpuSparsePattern);
                }
                REQUIRE_EQ(production_bytes[hip], total_bytes);
            }
        }
        std::cout << "dictionary_sparse_reference mutation_sites_per_record=" << extra_patches + 1U
                  << " actual_patches=" << actual_patches << " input_bytes=" << input.size()
                  << " candidate_bytes=" << total_bytes << " wide_lz4_bytes=" << wide_lz4_bytes
                  << " cpu_payload_bytes=" << production_bytes[0] << " gpu_payload_bytes=" << production_bytes[1]
                  << " memory_only=true disk_write_bytes=0\n";
    }
    std::vector<std::byte> random(block_bytes);
    std::uint32_t state = 0x83B71AC9U;
    for (auto& byte : random) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        byte = static_cast<std::byte>(state >> 24U);
    }
    REQUIRE_TRUE(!sparse_pattern_reference_bytes(random, record_bytes).has_value());
}

// Purpose: Quantify the ratio ceiling for periodic records beyond the current GPU motif bound.
// Inputs: Explicit opt-in, deterministic 256 KiB or 1 MiB records, and one mutation per later record.
// Outputs: Prints a byte-exact sparse reference and verified production CPU/HIP payload sizes in RAM.
TEST_CASE(dictionary_long_period_reference_opt_in) {
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"FLITZZIP_DICTIONARY_LONG_PERIOD_RESEARCH", enabled, 2U) != 1U || enabled[0] != L'1') {
        return;
    }
    REQUIRE_TRUE(flitzzip::query_gpu_info().available);
    constexpr std::size_t block_bytes = 4U * 1024U * 1024U;
    for (const auto record_bytes : {256U * 1024U, 1024U * 1024U}) {
        std::vector<std::byte> input(block_bytes);
        std::uint32_t state = 0xC67A349DU;
        for (std::size_t index = 0U; index < record_bytes; ++index) {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            input[index] = static_cast<std::byte>(state >> 24U);
        }
        for (std::size_t index = record_bytes; index < input.size(); ++index) {
            input[index] = input[index % record_bytes];
        }
        for (std::size_t record = 1U; record < input.size() / record_bytes; ++record) {
            const auto position = record * record_bytes + 1024U;
            input[position] = static_cast<std::byte>(static_cast<unsigned char>(input[position]) ^ 0xFFU);
        }
        const auto reference_bytes = sparse_pattern_reference_bytes(input, record_bytes);
        REQUIRE_TRUE(reference_bytes.has_value());
        std::array<std::size_t, 2> production_bytes{};
        for (const bool hip : {false, true}) {
            flitzzip::GpuCodecOptions options;
            options.block_size = static_cast<std::uint32_t>(block_bytes);
            options.compression_level = 5;
            options.force_cpu = !hip;
            options.require_gpu = hip;
            const auto encoded = flitzzip::encode_chunk(input, options);
            std::vector<std::byte> decoded(input.size());
            REQUIRE_EQ(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options), hip);
            REQUIRE_EQ(decoded, input);
            production_bytes[hip] = encoded.payload.size();
            if (hip) {
                REQUIRE_EQ(encoded.blocks.size(), 1U);
                REQUIRE_EQ(encoded.blocks[0].kind, flitzzip::BlockKind::GpuLongSparsePattern);
                REQUIRE_EQ(production_bytes[hip], *reference_bytes);
            }
        }
        std::cout << "dictionary_long_period_reference period_bytes=" << record_bytes << " input_bytes=" << input.size()
                  << " candidate_bytes=" << *reference_bytes << " cpu_payload_bytes=" << production_bytes[0]
                  << " gpu_payload_bytes=" << production_bytes[1] << " memory_only=true disk_write_bytes=0\n";
    }
}

// Purpose: Localize native dictionary CRC and decode divergence at real archive-chunk sizes.
// Inputs: Repeated records across the 8 MiB CRC policy boundary and up to the 128 MiB chunk limit.
// Outputs: CPU/HIP CRC and decoded bytes match source bytes for every encoded chunk.
TEST_CASE(dictionary_production_chunk_crc_and_decode) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    for (const std::size_t size : {4U * 1024U * 1024U, 8U * 1024U * 1024U - 1U, 8U * 1024U * 1024U,
                                   8U * 1024U * 1024U + 1U, 16U * 1024U * 1024U, 128U * 1024U * 1024U}) {
        for (const bool benchmark_record : {false, true}) {
            std::vector<std::byte> input(size);
            std::uint32_t state = 0x31674325U;
            for (std::size_t index = 0; index < 16384U; ++index) {
                state ^= state << 13U;
                state ^= state >> 17U;
                state ^= state << 5U;
                std::uint64_t value = index + 0x9E3779B97F4A7C15ULL;
                value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
                value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
                value ^= value >> 31U;
                input[index] =
                    benchmark_record ? static_cast<std::byte>(value >> 56U) : static_cast<std::byte>(state >> 24U);
            }
            for (std::size_t index = 16384U; index < input.size(); ++index) {
                input[index] = input[index % 16384U];
            }
            flitzzip::GpuCodecOptions gpu;
            gpu.block_size = 1024U * 1024U;
            gpu.compression_level = 5;
            gpu.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
            const auto expected_crc = flitzzip::crc32(input);
            const auto encoded = flitzzip::encode_owned_chunk(std::vector<std::byte>(input), gpu);
            const auto encode_stats = flitzzip::snapshot_gpu_telemetry(*gpu.telemetry);
            REQUIRE_TRUE(encode_stats.kernel_launches > 0U);
            REQUIRE_TRUE(encoded.gpu_used && encoded.source_crc32_available);
            REQUIRE_EQ(encoded.source_crc32, expected_crc);
            flitzzip::GpuCodecOptions cpu;
            cpu.require_gpu = false;
            cpu.force_cpu = true;
            const auto cpu_crc = flitzzip::crc_decoded_chunk(encoded.payload, encoded.blocks, size, cpu);
            const auto gpu_crc = flitzzip::crc_decoded_chunk(encoded.payload, encoded.blocks, size, gpu);
            std::cout << "dictionary_chunk_crc_case size=" << size << " benchmark_record=" << benchmark_record
                      << " source=" << expected_crc << " cpu=" << cpu_crc.crc32 << " gpu=" << gpu_crc.crc32 << "\n";
            REQUIRE_EQ(cpu_crc.crc32, expected_crc);
            REQUIRE_EQ(gpu_crc.crc32, expected_crc);
            std::vector<std::byte> decoded(size);
            REQUIRE_TRUE(!flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, cpu));
            REQUIRE_EQ(decoded, input);
            REQUIRE_TRUE(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, gpu));
            REQUIRE_EQ(decoded, input);
        }
    }
}

// Purpose: Detect nondeterministic payloads when independent HIP callers encode equal source chunks.
// Inputs: Opt-in sixteen concurrent 128 MiB owned-chunk calls at the production effort level.
// Outputs: Every complete encoded payload is byte-identical to the serial reference.
TEST_CASE(dictionary_concurrent_equal_input_encoding) {
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"FLITZZIP_DICTIONARY_STRESS", enabled, 2U) != 1U || enabled[0] != L'1' ||
        !flitzzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> input(128U * 1024U * 1024U);
    for (std::size_t index = 0; index < input.size(); ++index) {
        std::uint64_t value = index % 16384U;
        value += 0x9E3779B97F4A7C15ULL;
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        value ^= value >> 31U;
        input[index] = static_cast<std::byte>(value >> 56U);
    }
    flitzzip::GpuCodecOptions gpu;
    gpu.block_size = 1024U * 1024U;
    gpu.compression_level = 5;
    const auto reference = flitzzip::encode_owned_chunk(std::vector<std::byte>(input), gpu);
    std::vector<std::future<flitzzip::EncodedChunk>> pending;
    pending.reserve(16U);
    for (std::size_t index = 0; index < 16U; ++index) {
        pending.push_back(std::async(std::launch::async, [&input, gpu] {
            return flitzzip::encode_owned_chunk(std::vector<std::byte>(input), gpu);
        }));
    }
    for (auto& task : pending) {
        const auto encoded = task.get();
        if (encoded.payload != reference.payload) {
            std::cout << "dictionary_concurrent_diff reference_bytes=" << reference.payload.size()
                      << " candidate_bytes=" << encoded.payload.size() << "\n";
            for (std::size_t block = 0; block < reference.blocks.size(); ++block) {
                if (reference.blocks[block].encoded_len != encoded.blocks[block].encoded_len) {
                    std::cout << "first_changed_block=" << block
                              << " reference_len=" << reference.blocks[block].encoded_len
                              << " candidate_len=" << encoded.blocks[block].encoded_len << "\n";
                    break;
                }
            }
        }
        REQUIRE_EQ(encoded.payload, reference.payload);
    }
}

// Purpose: Keep the original corruption fixture reproducible while distinguishing concurrent callers' data.
// Inputs: A caller identity; zero reproduces the original 16 KiB-periodic 4 MiB fixture exactly.
// Outputs: Returns immutable deterministic bytes; different identities have separately seeded records.
std::vector<std::byte> dictionary_concurrent_input(std::size_t identity) {
    std::vector<std::byte> input(4U * 1024U * 1024U);
    for (std::size_t index = 0; index < input.size(); ++index) {
        std::uint64_t value = index % 16384U;
        value += 0x9E3779B97F4A7C15ULL * (identity + 1U);
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        value ^= value >> 31U;
        input[index] = static_cast<std::byte>(value >> 56U);
    }
    return input;
}

// Purpose: Exercise both naturally staggered and gated HIP callers without assuming one schedule covers another.
// Inputs: Fixed bounded caller/repetition counts and explicit start/input policies; every caller owns its work.
// Outputs: Validates serial sources independently and requires exact concurrent results, retaining failed bytes.
void require_concurrent_dictionary_batches(std::size_t workers, std::size_t repetitions, bool synchronized,
                                           bool distinct_inputs) {
    std::vector<std::vector<std::byte>> inputs;
    std::vector<EncodedBatch> references;
    const auto sources = distinct_inputs ? workers : 1U;
    for (std::size_t identity = 0; identity < sources; ++identity) {
        inputs.push_back(dictionary_concurrent_input(identity));
        const auto& input = inputs.back();
        references.push_back(encode_segments(input, 5));
        const auto& reference = references.back();
        REQUIRE_TRUE(reference.gpu_used);
        REQUIRE_EQ(reference.segments.size(), input.size() / kSegmentBytes);
        for (std::size_t segment = 0; segment < reference.segments.size(); ++segment) {
            const auto decoded = decode_reference_block(reference.segments[segment]);
            const auto source = std::span<const std::byte>(input).subspan(segment * kSegmentBytes, kSegmentBytes);
            REQUIRE_TRUE(std::equal(decoded.begin(), decoded.end(), source.begin(), source.end()));
        }
    }
    std::vector<std::future<void>> pending;
    pending.reserve(workers);
    // Destroying this promise first also unblocks admitted workers if a later async launch throws.
    std::promise<void> start;
    const auto ready = start.get_future().share();
    for (std::size_t worker = 0; worker < workers; ++worker) {
        pending.push_back(std::async(std::launch::async, [&, worker, ready] {
            if (synchronized) {
                ready.get();
            }
            const auto identity = distinct_inputs ? worker : 0U;
            const auto& input = inputs[identity];
            const auto& reference = references[identity];
            for (std::size_t iteration = 0; iteration < repetitions; ++iteration) {
                const auto candidate = encode_segments(input, 5);
                REQUIRE_TRUE(candidate.gpu_used);
                REQUIRE_EQ(candidate.segments.size(), reference.segments.size());
                for (std::size_t segment = 0; segment < reference.segments.size(); ++segment) {
                    if (candidate.segments[segment].payload != reference.segments[segment].payload ||
                        candidate.segments[segment].input_bytes != reference.segments[segment].input_bytes) {
                        const auto source =
                            std::span<const std::byte>(input).subspan(segment * kSegmentBytes, kSegmentBytes);
                        throw std::runtime_error(
                            "dictionary batch differs at worker " + std::to_string(worker) + " iteration " +
                            std::to_string(iteration) + " segment " + std::to_string(segment) + " identity " +
                            std::to_string(identity) + " synchronized " + std::to_string(synchronized) +
                            " reference_sequence " + describe_first_dictionary_sequence(reference.segments[segment]) +
                            " candidate " + describe_dictionary_payload_failure(candidate.segments[segment], source));
                    }
                }
            }
        }));
    }
    start.set_value();
    for (auto& task : pending) {
        task.get();
    }
}

// Purpose: Isolate dictionary batch state under both original staggered and synchronized HIP start schedules.
// Inputs: Four callers by default, or thirty-two opt-in callers, encode the original equal 4 MiB source repeatedly.
// Outputs: Independently decoded reference bytes and every concurrent segment match the immutable source.
TEST_CASE(dictionary_concurrent_repeated_batch_encoding) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    wchar_t enabled[2]{};
    const bool stress = GetEnvironmentVariableW(L"FLITZZIP_DICTIONARY_STRESS", enabled, 2U) == 1U && enabled[0] == L'1';
    for (const bool synchronized : {false, true}) {
        require_concurrent_dictionary_batches(stress ? 32U : 4U, stress ? 32U : 16U, synchronized, false);
    }
}

// Purpose: Expose cross-call input contamination that equal-input concurrency can conceal.
// Inputs: Four separately seeded 4 MiB sources, four repetitions, and both start schedules at normal effort five.
// Outputs: Every required-HIP result matches its own independently decoded reference without filesystem work.
TEST_CASE(dictionary_concurrent_distinct_input_encoding) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    for (const bool synchronized : {false, true}) {
        require_concurrent_dictionary_batches(4U, 4U, synchronized, true);
    }
}

// Purpose: Separate dense match-table instability from the dictionary segment encoder.
// Inputs: Four concurrent repeated 4 MiB searches by default, or thirty-two callers for opt-in stress.
// Outputs: Every dense GPU match record matches the independently validated serial reference exactly.
TEST_CASE(dictionary_concurrent_dense_match_table) {
    wchar_t enabled[2]{};
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> input(4U * 1024U * 1024U);
    for (std::size_t index = 0; index < input.size(); ++index) {
        std::uint64_t value = index % 16384U;
        value += 0x9E3779B97F4A7C15ULL;
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        value ^= value >> 31U;
        input[index] = static_cast<std::byte>(value >> 56U);
    }
    const auto reference = find_matches(input, 5);
    require_valid_matches(input, reference, 5);
    const bool stress = GetEnvironmentVariableW(L"FLITZZIP_DICTIONARY_STRESS", enabled, 2U) == 1U && enabled[0] == L'1';
    const std::size_t workers = stress ? 32U : 4U;
    const std::size_t repetitions = stress ? 8U : 2U;
    std::vector<std::future<void>> pending;
    pending.reserve(workers);
    std::promise<void> start;
    const auto ready = start.get_future().share();
    for (std::size_t worker = 0; worker < workers; ++worker) {
        pending.push_back(std::async(std::launch::async, [&input, &reference, repetitions, ready] {
            ready.get();
            for (std::size_t iteration = 0; iteration < repetitions; ++iteration) {
                const auto candidate = find_matches(input, 5);
                const auto difference =
                    std::mismatch(reference.matches.begin(), reference.matches.end(), candidate.matches.begin());
                if (difference.first != reference.matches.end()) {
                    throw std::runtime_error("dictionary dense match table differs at position " +
                                             std::to_string(difference.first - reference.matches.begin()));
                }
            }
        }));
    }
    start.set_value();
    for (auto& task : pending) {
        task.get();
    }
}

// Purpose: Admit only bounded segment extents and preserve required-HIP semantics before decoding.
// Inputs: Empty, oversized, invalid-length, and missing-backend cases with genuine host allocations.
// Outputs: Requires explicit admission errors and no GPU work for an empty batch.
TEST_CASE(dictionary_decoder_admission) {
    const auto empty = decode_segments({});
    REQUIRE_TRUE(empty.bytes.empty() && !empty.gpu_used && !empty.decode_ms);
    const EncodedSegment valid{{std::byte{0x10}, std::byte{'A'}}, 1U};
    std::vector<std::vector<EncodedSegment>> invalid{
        std::vector<EncodedSegment>(65U, valid),
        {EncodedSegment{{}, 1U}},
        {EncodedSegment{valid.payload, 0U}},
        {EncodedSegment{valid.payload, kSegmentBytes + 1U}},
        {EncodedSegment{std::vector<std::byte>(kEncodedSegmentCapacity + 1U), kSegmentBytes}},
    };
    for (const auto& segments : invalid) {
        bool rejected = false;
        try {
            (void)decode_segments(segments);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    if (!flitzzip::query_gpu_info().available) {
        bool rejected = false;
        try {
            (void)decode_segments(std::span{&valid, 1U});
        } catch (const flitzzip::GpuError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Decode a golden block produced by python-lz4 4.4.5, not the FlitzZip encoder.
// Inputs: The raw block for (b'abcdefg' * 137) + bytes(range(32)), including a 952-byte distance-seven copy.
// Outputs: Requires byte-exact HIP output across the overlapping period and final literal extension.
TEST_CASE(dictionary_decoder_independent_writer_fixture) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const std::array<unsigned char, 48> encoded{0x7f, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x07, 0x00, 0xff, 0xff,
                                                0xff, 0xa8, 0xf0, 0x11, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                                0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13,
                                                0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
    EncodedSegment segment;
    segment.payload.assign(std::as_bytes(std::span{encoded}).begin(), std::as_bytes(std::span{encoded}).end());
    segment.input_bytes = 991U;
    std::vector<std::byte> expected;
    for (std::size_t index = 0; index < 959U; ++index) {
        expected.push_back(static_cast<std::byte>('a' + index % 7U));
    }
    for (unsigned int value = 0; value < 32U; ++value) {
        expected.push_back(static_cast<std::byte>(value));
    }
    const auto decoded = decode_segments(std::span{&segment, 1U});
    REQUIRE_TRUE(decoded.gpu_used && decoded.bytes == expected);
}

// Purpose: Accept unused final-token match bits, matching independent LZ4 readers.
// Inputs: All sixteen low-nibble values on literal-only and match-following final sequences.
// Outputs: Requires identical decoded bytes without consuming nonexistent offset or match-extension fields.
TEST_CASE(dictionary_decoder_final_token_unused_bits) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    for (unsigned int nibble = 0; nibble < 16U; ++nibble) {
        const EncodedSegment literal{{static_cast<std::byte>(0x10U | nibble), std::byte{'A'}}, 1U};
        EncodedSegment matched{
            {std::byte{0x13}, std::byte{'A'}, std::byte{1}, std::byte{0}, static_cast<std::byte>(0x50U | nibble)}, 13U};
        matched.payload.insert(matched.payload.end(), 5U, std::byte{'A'});
        const std::array batch{literal, matched};
        REQUIRE_TRUE(decode_segments(batch).bytes == std::vector<std::byte>(14U, std::byte{'A'}));
    }
}

// Purpose: Decode legal block extremes independently of the current encoder's smaller match-length budget.
// Inputs: A python-lz4 full-segment run and a hand-derived maximum-distance block with exact expected bytes.
// Outputs: Requires 65530-byte overlapping matches and distance 65524 without cross-segment reads or output changes.
TEST_CASE(dictionary_decoder_long_match_and_maximum_distance) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    EncodedSegment repeated{{std::byte{0x1f}, std::byte{'A'}, std::byte{1}, std::byte{0}}, kSegmentBytes};
    repeated.payload.insert(repeated.payload.end(), 256U, std::byte{0xff});
    repeated.payload.push_back(std::byte{0xe7});
    repeated.payload.push_back(std::byte{0x50});
    repeated.payload.insert(repeated.payload.end(), 5U, std::byte{'A'});
    REQUIRE_EQ(repeated.payload.size(), 267U);
    const auto run = decode_segments(std::span{&repeated, 1U});
    REQUIRE_TRUE(run.bytes == std::vector<std::byte>(kSegmentBytes, std::byte{'A'}));

    constexpr std::uint32_t distance = kSegmentBytes - 12U;
    EncodedSegment distant{{std::byte{0xf3}}, kSegmentBytes};
    distant.payload.insert(distant.payload.end(), 256U, std::byte{0xff});
    distant.payload.push_back(std::byte{0xe5});
    std::vector<std::byte> expected;
    for (std::uint32_t index = 0; index < distance; ++index) {
        const auto value = static_cast<std::byte>((index * 97U + index / 7U) & 255U);
        expected.push_back(value);
        distant.payload.push_back(value);
    }
    distant.payload.push_back(std::byte{0xf4});
    distant.payload.push_back(std::byte{0xff});
    for (std::uint32_t index = 0; index < 7U; ++index) {
        expected.push_back(expected[index]);
    }
    distant.payload.push_back(std::byte{0x50});
    for (std::uint32_t index = 0; index < 5U; ++index) {
        const auto value = static_cast<std::byte>(index + 1U);
        expected.push_back(value);
        distant.payload.push_back(value);
    }
    const std::array batch{repeated, distant, repeated};
    const auto decoded = decode_segments(batch);
    REQUIRE_EQ(decoded.bytes.size(), 3U * kSegmentBytes);
    REQUIRE_TRUE(std::equal(expected.begin(), expected.end(), decoded.bytes.begin() + kSegmentBytes));
    REQUIRE_TRUE(std::all_of(decoded.bytes.begin(), decoded.bytes.begin() + kSegmentBytes,
                             [](auto value) { return value == std::byte{'A'}; }));
    REQUIRE_TRUE(std::all_of(decoded.bytes.begin() + 2U * kSegmentBytes, decoded.bytes.end(),
                             [](auto value) { return value == std::byte{'A'}; }));
}

// Purpose: Reject incomplete sequences and inconsistent extents before exposing any partial decoded batch.
// Inputs: Invalid tokens, extensions, distances, tail rules, and declared sizes between valid neighboring blocks.
// Outputs: Requires explicit decode errors and a succeeding valid decode after every rejected batch.
TEST_CASE(dictionary_decoder_rejects_incomplete_sequences) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const EncodedSegment valid{{std::byte{0x10}, std::byte{'A'}}, 1U};
    const std::vector<std::pair<std::vector<unsigned char>, std::uint32_t>> cases{
        {{0x10}, 1U},
        {{0xf0}, 15U},
        {{0xf0, 0xff}, 65536U},
        {{0xf0, 0xff, 0xff}, 16U},
        {{0x10, 'A'}, 2U},
        {{0x20, 'A', 'B'}, 1U},
        {{0x00, 0, 0, 0x50, 'A', 'A', 'A', 'A', 'A'}, 9U},
        {{0x10, 'A', 2, 0, 0x50, 'A', 'A', 'A', 'A', 'A'}, 10U},
        {{0x10, 'A', 1}, 13U},
        {{0x1f, 'A', 1, 0}, 65536U},
        {{0x1f, 'A', 1, 0, 0xff, 0xff}, 32U},
        {{0x13, 'A', 1, 0, 0x40, 'A', 'A', 'A', 'A'}, 12U},
        {{0x10, 'A', 1, 0, 0x50, 'A', 'A', 'A', 'A', 'A'}, 10U},
        {{0x13, 'A', 1, 0}, 8U},
        {{0x13, 'A', 1, 0, 0x50, 'A', 'A', 'A', 'A', 'A', 0}, 13U},
    };
    for (const auto& [payload, expected_bytes] : cases) {
        EncodedSegment invalid;
        const auto bytes = std::as_bytes(std::span{payload});
        invalid.payload.assign(bytes.begin(), bytes.end());
        invalid.input_bytes = expected_bytes;
        const std::array batch{valid, invalid, valid};
        bool rejected = false;
        try {
            (void)decode_segments(batch);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        const std::array good_batch{valid, valid, valid};
        REQUIRE_TRUE(decode_segments(good_batch).bytes == std::vector<std::byte>(3U, std::byte{'A'}));
    }
}

// Purpose: Enforce nine real, increasing effort budgets before any GPU allocation.
// Inputs: Valid levels, invalid signed extremes, short inputs, and one oversized but genuinely allocated batch.
// Outputs: Requires bounded policies, no search for short inputs, and explicit invalid-argument rejection.
TEST_CASE(dictionary_effort_and_resource_contracts) {
    constexpr std::array<std::uint32_t, 9> expected_budgets{192U, 256U, 384U, 512U, 768U, 1536U, 3072U, 6144U, 12288U};
    std::uint32_t last_depth = 0;
    std::uint32_t last_bytes = 0;
    for (int level = 1; level <= 9; ++level) {
        const auto effort = effort_for_level(level);
        REQUIRE_TRUE(effort.max_candidates > last_depth);
        REQUIRE_TRUE(effort.max_byte_comparisons > last_bytes);
        REQUIRE_EQ(effort.max_byte_comparisons, expected_budgets[static_cast<std::size_t>(level - 1)]);
        last_depth = effort.max_candidates;
        last_bytes = effort.max_byte_comparisons;
    }
    for (const int level : {-1, 0, 10, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
        bool rejected = false;
        try {
            (void)find_matches({}, level);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    for (const std::size_t size : {0U, 1U, 2U, 3U}) {
        const std::vector<std::byte> input(size);
        const auto result = find_matches(input, 5);
        REQUIRE_TRUE(!result.gpu_used);
        REQUIRE_TRUE(!result.index_ms && !result.search_ms);
        require_valid_matches(input, result, 5);
    }
    bool rejected = false;
    try {
        const std::vector<std::byte> oversized(kMaxBatchBytes + 1U);
        (void)find_matches(oversized, 9);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Prevent invalid event timing from becoming a claimed duration or fabricated zero.
// Inputs: Negative observed durations, non-finite values, and valid zero/positive durations; no GPU work.
// Outputs: Requires explicit absence for invalid values and exact preservation of valid measurements.
TEST_CASE(dictionary_stage_timing_validity) {
    for (const double value : {-0.03942, -0.01693, -0.09821, -1.0, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        REQUIRE_TRUE(!validated_stage_milliseconds(value));
    }
    for (const double value : {0.0, 0.125, 1234.5}) {
        const auto valid = validated_stage_milliseconds(value);
        REQUIRE_TRUE(valid.has_value());
        REQUIRE_EQ(std::bit_cast<std::uint64_t>(*valid), std::bit_cast<std::uint64_t>(value));
    }
}

// Purpose: Reject unavailable GPU execution instead of silently doing dictionary search on the CPU.
// Inputs: A searchable four-byte input on a build or host without usable HIP.
// Outputs: Requires GpuError when HIP is unavailable; hardware-backed execution is covered separately.
TEST_CASE(dictionary_search_has_no_cpu_fallback) {
    if (flitzzip::query_gpu_info().available) {
        return;
    }
    bool rejected = false;
    try {
        const std::array<std::byte, 4> input{};
        (void)find_matches(input, 1);
    } catch (const flitzzip::GpuError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Prove all nine effort settings affect real GPU search and preserve deterministic results.
// Inputs: The controlled depth corpus, encoded twice at each level without filesystem writes.
// Outputs: Requires strictly improving target matches, nearest ties, repeatability, and nondecreasing match lengths.
TEST_CASE(dictionary_gpu_nine_efforts_find_longer_matches) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const auto input = make_dictionary_depth_fixture();
    std::vector<Match> previous;
    for (int level = 1; level <= 9; ++level) {
        const auto result = find_matches(input, level);
        REQUIRE_TRUE(result.gpu_used);
        require_valid_matches(input, result, level);
        const auto& target = result.matches[256U * 64U];
        REQUIRE_EQ(target.length, static_cast<std::uint16_t>(level + 3));
        REQUIRE_EQ(target.distance, static_cast<std::uint16_t>((1U << (level - 1)) * 64U));
        REQUIRE_TRUE(result.matches == find_matches(input, level).matches);
        for (std::size_t i = 0; i < previous.size(); ++i) {
            REQUIRE_TRUE(result.matches[i].length >= previous[i].length);
        }
        previous = result.matches;
        std::cout << "dictionary_depth_case level=" << level << " match_bytes=" << target.length
                  << " distance=" << target.distance << " memory_only=true disk_write_bytes=0\n";
    }
}

// Purpose: Exercise tiny inputs, segment boundaries, overlapping matches, and partial final segments.
// Inputs: RAM-only seeded bytes mixed with fill and repeated nontrivial records at three effort levels.
// Outputs: Requires all matches to stay inside source segments and every returned byte range to agree exactly.
TEST_CASE(dictionary_gpu_matches_respect_segment_boundaries) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    for (const std::size_t size : {4U, 5U, 31U, 65535U, 65536U, 65537U, 131085U}) {
        std::vector<std::byte> input(size);
        std::uint32_t state = 0xF174C253U;
        for (std::size_t i = 0; i < input.size(); ++i) {
            state = state * 1664525U + 1013904223U;
            input[i] = static_cast<std::byte>(i < 256U ? 0U : i % 131U < 100U ? i % 37U : state >> 24U);
        }
        for (const int level : {1, 5, 9}) {
            const auto result = find_matches(input, level);
            REQUIRE_TRUE(result.gpu_used);
            require_valid_matches(input, result, level);
            for (std::size_t start = 0; start < input.size(); start += kSegmentBytes) {
                REQUIRE_EQ(result.matches[start].length, 0U);
            }
        }
    }
}

// Purpose: Detect missing or suboptimal matches, not just validate the matches that HIP happens to return.
// Inputs: Short seeded alphabets and overlapping repeats whose exhaustive work fits strictly inside level-9 budgets.
// Outputs: Requires every GPU match length and nearest reference to equal an independent exhaustive CPU oracle.
TEST_CASE(dictionary_gpu_matches_agree_with_exhaustive_reference) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    for (std::uint32_t seed = 1; seed <= 16U; ++seed) {
        std::vector<std::byte> input(128U);
        auto state = seed;
        for (auto& byte : input) {
            state = state * 1664525U + 1013904223U;
            byte = static_cast<std::byte>((state >> 24U) % (seed % 5U + 1U));
        }
        const auto result = find_matches(input, 9);
        require_valid_matches(input, result, 9);
        for (std::size_t position = 0; position < input.size(); ++position) {
            const auto expected = exhaustive_short_match(input, position);
            REQUIRE_EQ(result.matches[position].length, expected.length);
            REQUIRE_EQ(result.matches[position].distance, expected.distance);
        }
    }
}

// Purpose: Validate the largest supported batch independently of host load and event-timing availability.
// Inputs: Four MiB of deterministic incompressible-shaped bytes followed by overlapping repetitions.
// Outputs: Requires bounded workspace and exact matches; does not publish performance measurements.
TEST_CASE(dictionary_gpu_maximum_batch_is_bounded) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> input(kMaxBatchBytes);
    std::uint32_t state = 0x2416F35AU;
    for (auto& byte : input) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        byte = static_cast<std::byte>(state >> 24U);
    }
    const auto result = find_matches(input, 9);
    REQUIRE_TRUE(result.gpu_used);
    require_valid_matches(input, result, 9);
    std::cout << "dictionary_stage_case input_bytes=" << input.size()
              << " workspace_bytes=" << result.device_workspace_bytes << " rocprim_version=" << result.primitive_version
              << " memory_only=true disk_write_bytes=0\n";
    std::fill(input.begin(), input.end(), std::byte{0});
    const auto repetitive = find_matches(input, 9);
    require_valid_matches(input, repetitive, 9);
    REQUIRE_EQ(repetitive.matches[1].length, kMaxMatchBytes);
    REQUIRE_EQ(repetitive.matches[1].distance, 1U);
    std::cout << "dictionary_repetitive_stage_case input_bytes=" << input.size()
              << " workspace_bytes=" << repetitive.device_workspace_bytes << " memory_only=true disk_write_bytes=0\n";
}

// Purpose: Prove stable prefix sorting retains nearest ties and orders sentinels after maximum valid keys.
// Inputs: Almost four MiB of 0xFF bytes spanning all 64 segments, including a partial final segment.
// Outputs: Requires exact level-one match length/distance at segment boundaries and independent encoded read-back.
TEST_CASE(dictionary_gpu_stable_prefix_order_with_maximum_sentinels) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const std::vector<std::byte> input(kMaxBatchBytes - 2U, std::byte{0xFF});
    const auto matches = find_matches(input, 1);
    REQUIRE_TRUE(matches.gpu_used);
    REQUIRE_EQ(matches.matches.size(), input.size());
    REQUIRE_TRUE(matches.device_workspace_bytes <= kMaxWorkspaceBytes);
    const auto maximum_length = kMinMatchBytes + effort_for_level(1).max_byte_comparisons;
    for (std::size_t start = 0; start < input.size(); start += kSegmentBytes) {
        const auto bytes = std::min<std::size_t>(kSegmentBytes, input.size() - start);
        for (const auto offset : {0U, 1U, 2U, 15U, 255U, 4095U, static_cast<unsigned int>(bytes - 12U),
                                  static_cast<unsigned int>(bytes - 4U), static_cast<unsigned int>(bytes - 3U)}) {
            const auto& match = matches.matches[start + offset];
            const bool has_match = offset != 0U && bytes - offset >= kMinMatchBytes;
            REQUIRE_EQ(match.distance, has_match ? 1U : 0U);
            REQUIRE_EQ(match.length, has_match ? std::min<std::size_t>(maximum_length, bytes - offset) : 0U);
        }
    }
    const auto encoded = encode_segments(input, 1);
    (void)require_valid_encoded_batch(input, encoded);
}

// Purpose: Preserve required-HIP semantics and argument admission for the encoded-byte path.
// Inputs: Empty, invalid-level, oversized, and unavailable-device requests.
// Outputs: Requires explicit errors before work and an empty, GPU-free result for empty input.
TEST_CASE(dictionary_encoder_admission) {
    REQUIRE_TRUE(encode_segments({}, 1).segments.empty());
    REQUIRE_TRUE(!encode_segments({}, 9).gpu_used);
    for (const int level : {0, 10}) {
        bool rejected = false;
        try {
            (void)encode_segments({}, level);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    bool rejected = false;
    try {
        const std::vector<std::byte> oversized(kMaxBatchBytes + 1U);
        (void)encode_segments(oversized, 5);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    if (!flitzzip::query_gpu_info().available) {
        rejected = false;
        try {
            const std::array<std::byte, 1> input{};
            (void)encode_segments(input, 5);
        } catch (const flitzzip::GpuError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Check block bytes against hand-derived sequences, not only a matching decoder implementation.
// Inputs: A one-byte literal and the smallest independently compressible repeated-byte input.
// Outputs: Requires exact LZ4 token, offset, final-literal bytes and independent roundtrips.
TEST_CASE(dictionary_encoder_golden_blocks) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    for (const std::size_t length : {1U, 13U}) {
        const std::vector<std::byte> input(length, std::byte{'A'});
        const auto encoded = encode_segments(input, 9);
        (void)require_valid_encoded_batch(input, encoded);
        const std::vector<std::byte> expected =
            length == 1U ? std::vector<std::byte>{std::byte{0x10}, std::byte{'A'}}
                         : std::vector<std::byte>{std::byte{0x13}, std::byte{'A'}, std::byte{1},   std::byte{0},
                                                  std::byte{0x50}, std::byte{'A'}, std::byte{'A'}, std::byte{'A'},
                                                  std::byte{'A'},  std::byte{'A'}};
        REQUIRE_TRUE(encoded.segments.front().payload == expected);
    }
}

// Purpose: Exercise literal extensions, final match restrictions, segment tails, and incompressible output bounds.
// Inputs: RAM-only seeded and repeated-byte data at boundary lengths and the largest batch, without timing runs.
// Outputs: Requires independent exact decoding and compact transfer accounting at low and high efforts.
TEST_CASE(dictionary_encoder_boundaries_and_maximum_batch) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    for (const std::size_t size : {4U, 5U, 12U, 14U, 15U, 16U, 270U, 65535U, 65536U, 65537U, 4194304U}) {
        std::vector<std::byte> input(size);
        std::uint32_t state = 0x21415367U;
        for (auto& byte : input) {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            byte = static_cast<std::byte>(state >> 24U);
        }
        for (const int level : {1, 9}) {
            (void)require_valid_encoded_batch(input, encode_segments(input, level));
        }
        std::fill(input.begin(), input.end(), std::byte{0xA7});
        (void)require_valid_encoded_batch(input, encode_segments(input, 9));
    }
}

// Purpose: Exercise sparse matches across cooperative search tiles and independent segment boundaries.
// Inputs: Seeded full-alphabet bytes with eight-byte copies placed at changing tile offsets.
// Outputs: Requires exact decoding at low/high effort without assuming sparse matches improve every block's size.
TEST_CASE(dictionary_encoder_sparse_tile_matches) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> input(131085U);
    std::uint32_t state = 0x52163745U;
    for (auto& byte : input) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        byte = static_cast<std::byte>(state >> 24U);
    }
    for (std::size_t offset = 300U; offset + 8U < input.size(); offset += 503U) {
        std::copy_n(input.begin() + offset - 257U, 8U, input.begin() + offset);
    }
    for (const int level : {1, 9}) {
        (void)require_valid_encoded_batch(input, encode_segments(input, level));
    }
}

// Purpose: Prove real dictionary size gains for long nontrivial repeats and increasing effort, without timing claims.
// Inputs: A 64 KiB full-alphabet corpus formed by repeating a seeded 16 KiB record four times.
// Outputs: Requires exact decoding, nine distinct effort sizes on this fixture, and fewer transfers than raw input.
TEST_CASE(dictionary_encoder_long_repeat_size_gains) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> input(kSegmentBytes);
    std::uint32_t state = 0x31674325U;
    for (std::size_t index = 0; index < input.size(); ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        input[index] = index < 16384U ? static_cast<std::byte>(state >> 24U) : input[index % 16384U];
    }
    std::size_t prior_size = input.size();
    const auto baseline = flitzzip::encode_chunk(input, {.require_gpu = true, .compression_level = 9});
    REQUIRE_TRUE(baseline.gpu_used);
    if (GetEnvironmentVariableW(L"FLITZZIP_DICTIONARY_INTEROP_EXPORT", nullptr, 0) == 0U) {
        std::cout << "dictionary_existing_codec_case input_bytes=" << input.size()
                  << " payload_bytes=" << baseline.payload.size()
                  << " memory_only=true disk_write_bytes=0 timing_not_evaluated=true\n";
    }
    for (int level = 1; level <= 9; ++level) {
        const auto result = encode_segments(input, level);
        const auto bytes = require_valid_encoded_batch(input, result);
        REQUIRE_TRUE(bytes < prior_size);
        REQUIRE_TRUE(result.d2h_bytes < input.size());
        prior_size = bytes;
        if (GetEnvironmentVariableW(L"FLITZZIP_DICTIONARY_INTEROP_EXPORT", nullptr, 0) == 0U) {
            std::cout << "dictionary_encoded_case level=" << level << " input_bytes=" << input.size()
                      << " payload_bytes=" << bytes << " d2h_bytes=" << result.d2h_bytes
                      << " memory_only=true disk_write_bytes=0 timing_not_evaluated=true\n";
        }
    }
    REQUIRE_TRUE(prior_size < input.size() / 3U);
}
