#pragma once

#include "core/archive_block_types.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace superzip {

struct DictionarySegmentSpan {
    std::uint32_t encoded_offset = 0;
    std::uint32_t encoded_size = 0;
    std::uint32_t decoded_offset = 0;
    std::uint32_t decoded_size = 0;
};

// Purpose: Read a little-endian offset from a bounded dictionary segment table.
// Inputs: `table` contains serialized offsets and `index` selects one 32-bit entry.
// Outputs: Returns the offset or throws when its bytes are missing.
inline std::uint32_t read_dictionary_offset(std::span<const std::byte> table, std::size_t index) {
    if (index >= table.size() / sizeof(std::uint32_t)) {
        throw ArchiveError("GPU dictionary block table is truncated");
    }
    const auto offset = index * sizeof(std::uint32_t);
    std::uint32_t value = 0;
    for (std::size_t byte = 0; byte < sizeof(std::uint32_t); ++byte) {
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(table[offset + byte])) << (byte * 8U);
    }
    return value;
}

// Purpose: Validate a version-four dictionary block before either CPU or HIP decoding.
// Inputs: `payload` contains cumulative segment offsets followed by independent LZ4 blocks; `decoded_size` is exact.
// Outputs: Returns disjoint bounded segment spans or throws on invalid framing and resource extents.
inline std::vector<DictionarySegmentSpan> parse_dictionary_segments(std::span<const std::byte> payload,
                                                                    std::uint32_t decoded_size) {
    if (decoded_size == 0U || decoded_size > kMaxArchiveBlockBytes) {
        throw ArchiveError("GPU dictionary block decoded size is invalid");
    }
    const auto segment_count =
        (static_cast<std::size_t>(decoded_size) + kGpuDictionarySegmentBytes - 1U) / kGpuDictionarySegmentBytes;
    const auto table_bytes = (segment_count + 1U) * sizeof(std::uint32_t);
    if (payload.size() <= table_bytes || payload.size() >= decoded_size) {
        throw ArchiveError("GPU dictionary block metadata is invalid");
    }
    const auto table = payload.first(table_bytes);
    const auto encoded_size = payload.size() - table_bytes;
    auto previous = read_dictionary_offset(table, 0U);
    if (previous != 0U) {
        throw ArchiveError("GPU dictionary block table must start at zero");
    }
    std::vector<DictionarySegmentSpan> spans;
    spans.reserve(segment_count);
    std::size_t decoded_offset = 0U;
    for (std::size_t segment = 0U; segment < segment_count; ++segment) {
        const auto next = read_dictionary_offset(table, segment + 1U);
        const auto decoded_len = std::min<std::size_t>(kGpuDictionarySegmentBytes, decoded_size - decoded_offset);
        const auto capacity = decoded_len + decoded_len / 255U + 16U;
        if (next <= previous || next > encoded_size || next - previous > capacity) {
            throw ArchiveError("GPU dictionary block segment extent is invalid");
        }
        spans.push_back(DictionarySegmentSpan{
            .encoded_offset = static_cast<std::uint32_t>(table_bytes + previous),
            .encoded_size = next - previous,
            .decoded_offset = static_cast<std::uint32_t>(decoded_offset),
            .decoded_size = static_cast<std::uint32_t>(decoded_len),
        });
        decoded_offset += decoded_len;
        previous = next;
    }
    if (previous != encoded_size) {
        throw ArchiveError("GPU dictionary block payload has trailing bytes");
    }
    return spans;
}

}  // namespace superzip
