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
    REQUIRE_EQ(gpu_decoded.h2d_bytes, payload_bytes + batch.segments.size() * 16U);
    REQUIRE_EQ(gpu_decoded.d2h_bytes, input.size() + batch.segments.size() * sizeof(std::uint32_t));
    REQUIRE_TRUE(!gpu_decoded.decode_ms || (std::isfinite(*gpu_decoded.decode_ms) && *gpu_decoded.decode_ms >= 0.0));
    return payload_bytes;
}

}  // namespace

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
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    if (!superzip::query_gpu_info().available) {
        bool rejected = false;
        try {
            (void)decode_segments(std::span{&valid, 1U});
        } catch (const superzip::GpuError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Decode a golden block produced by python-lz4 4.4.5, not the SuperZip encoder.
// Inputs: The raw block for (b'abcdefg' * 137) + bytes(range(32)), including a 952-byte distance-seven copy.
// Outputs: Requires byte-exact HIP output across the overlapping period and final literal extension.
TEST_CASE(dictionary_decoder_independent_writer_fixture) {
    if (!superzip::query_gpu_info().available) {
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
    if (!superzip::query_gpu_info().available) {
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
    if (!superzip::query_gpu_info().available) {
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
    if (!superzip::query_gpu_info().available) {
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
        } catch (const superzip::ArchiveError&) {
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
