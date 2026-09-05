#include "gpu/dictionary_matcher.hpp"
#include "gpu/gpu_codec.hpp"
#include "core/result.hpp"
#include "test_util.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>

namespace {

using namespace superzip::dictionary;

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

// Purpose: Export already verified raw blocks for an independent external LZ4 decoder when explicitly requested.
// Inputs: A bounded segment, its decoded bytes, and an optional harness-owned environment export directory.
// Outputs: Ordinary tests write nothing; opt-in runs emit numbered block/raw pairs, capped at 64 MiB per process.
void export_dictionary_interop_fixture(const EncodedSegment& segment, std::span<const std::byte> decoded) {
    const DWORD needed = GetEnvironmentVariableW(L"SUPERZIP_DICTIONARY_INTEROP_EXPORT", nullptr, 0);
    if (needed == 0U) {
        return;
    }
    REQUIRE_TRUE(needed <= 32768U);
    std::wstring root(needed, L'\0');
    const auto copied = GetEnvironmentVariableW(L"SUPERZIP_DICTIONARY_INTEROP_EXPORT", root.data(), needed);
    REQUIRE_TRUE(copied > 0U && copied < needed);
    root.resize(copied);
    static std::size_t exported_bytes = 0;
    static std::size_t ordinal = 0;
    const auto next_bytes = segment.payload.size() + decoded.size();
    REQUIRE_TRUE(next_bytes <= 64U * 1024U * 1024U - exported_bytes);
    const auto directory = std::filesystem::path(root);
    std::filesystem::create_directories(directory);
    const auto name = std::to_string(ordinal++);
    const auto write_bytes = [&](const char* extension, std::span<const std::byte> bytes) {
        const auto path = directory / (name + extension);
        REQUIRE_TRUE(!std::filesystem::exists(path));
        std::ofstream file(path, std::ios::binary);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.close();
    };
    write_bytes(".lz4block", segment.payload);
    write_bytes(".raw", decoded);
    exported_bytes += next_bytes;
}

// Purpose: Verify actual encoded bytes independently and account for compact transfers, including size metadata.
// Inputs: Original bytes and the HIP block encoder's result.
// Outputs: Requires byte-exact restoration and bounded workspace; returns total encoded payload bytes.
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
    return payload_bytes;
}

}  // namespace

// Purpose: Enforce nine real, increasing effort budgets before any GPU allocation.
// Inputs: Valid levels, invalid signed extremes, short inputs, and one oversized but genuinely allocated batch.
// Outputs: Requires bounded policies, no search for short inputs, and explicit invalid-argument rejection.
TEST_CASE(dictionary_effort_and_resource_contracts) {
    std::uint32_t last_depth = 0;
    std::uint32_t last_bytes = 0;
    for (int level = 1; level <= 9; ++level) {
        const auto effort = effort_for_level(level);
        REQUIRE_TRUE(effort.max_candidates > last_depth);
        REQUIRE_TRUE(effort.max_byte_comparisons > last_bytes);
        last_depth = effort.max_candidates;
        last_bytes = effort.max_byte_comparisons;
    }
    for (const int level : {-1, 0, 10, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
        bool rejected = false;
        try {
            (void)find_matches({}, level);
        } catch (const superzip::ArchiveError&) {
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
    } catch (const superzip::ArchiveError&) {
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
        REQUIRE_EQ(*valid, value);
    }
}

// Purpose: Reject unavailable GPU execution instead of silently doing dictionary search on the CPU.
// Inputs: A searchable four-byte input on a build or host without usable HIP.
// Outputs: Requires GpuError when HIP is unavailable; hardware-backed execution is covered separately.
TEST_CASE(dictionary_search_has_no_cpu_fallback) {
    if (superzip::query_gpu_info().available) {
        return;
    }
    bool rejected = false;
    try {
        const std::array<std::byte, 4> input{};
        (void)find_matches(input, 1);
    } catch (const superzip::GpuError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Prove all nine effort settings affect real GPU search and preserve deterministic results.
// Inputs: The controlled depth corpus, encoded twice at each level without filesystem writes.
// Outputs: Requires strictly improving target matches, nearest ties, repeatability, and nondecreasing match lengths.
TEST_CASE(dictionary_gpu_nine_efforts_find_longer_matches) {
    if (!superzip::query_gpu_info().available) {
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
    if (!superzip::query_gpu_info().available) {
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
    if (!superzip::query_gpu_info().available) {
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
    if (!superzip::query_gpu_info().available) {
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
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    bool rejected = false;
    try {
        const std::vector<std::byte> oversized(kMaxBatchBytes + 1U);
        (void)encode_segments(oversized, 5);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    if (!superzip::query_gpu_info().available) {
        rejected = false;
        try {
            const std::array<std::byte, 1> input{};
            (void)encode_segments(input, 5);
        } catch (const superzip::GpuError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Check block bytes against hand-derived sequences, not only a matching decoder implementation.
// Inputs: A one-byte literal and the smallest independently compressible repeated-byte input.
// Outputs: Requires exact LZ4 token, offset, final-literal bytes and independent roundtrips.
TEST_CASE(dictionary_encoder_golden_blocks) {
    if (!superzip::query_gpu_info().available) {
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
    if (!superzip::query_gpu_info().available) {
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
    if (!superzip::query_gpu_info().available) {
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
    if (!superzip::query_gpu_info().available) {
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
    const auto baseline = superzip::encode_chunk(input, {.require_gpu = true, .compression_level = 9});
    REQUIRE_TRUE(baseline.gpu_used);
    if (GetEnvironmentVariableW(L"SUPERZIP_DICTIONARY_INTEROP_EXPORT", nullptr, 0) == 0U) {
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
        if (GetEnvironmentVariableW(L"SUPERZIP_DICTIONARY_INTEROP_EXPORT", nullptr, 0) == 0U) {
            std::cout << "dictionary_encoded_case level=" << level << " input_bytes=" << input.size()
                      << " payload_bytes=" << bytes << " d2h_bytes=" << result.d2h_bytes
                      << " memory_only=true disk_write_bytes=0 timing_not_evaluated=true\n";
        }
    }
    REQUIRE_TRUE(prior_size < input.size() / 3U);
}
