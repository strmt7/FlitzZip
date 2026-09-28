#pragma once

#include <cstdint>

namespace superzip {

enum class BlockKind : std::uint8_t {
    Raw = 0,
    Fill = 1,
    Deflate = 2,
    Pattern = 3,
    GpuPrefix = 4,
    GpuAdaptivePrefix = 5,
    GpuDictionary = 6,
    GpuSparsePattern = 7,
    CpuZstd = 8,
    GpuLongSparsePattern = 9,
};

// Purpose: Group the two versioned sparse GPU encodings without conflating their period limits.
// Inputs: One decoded native block kind.
// Outputs: True only for the short or long sparse pattern kind.
inline constexpr bool is_gpu_sparse_pattern_kind(BlockKind kind) {
    return kind == BlockKind::GpuSparsePattern || kind == BlockKind::GpuLongSparsePattern;
}

inline constexpr std::uint32_t kGpuDictionarySegmentBytes = 65536U;
inline constexpr std::uint32_t kGpuDictionaryEncodedSegmentCapacity =
    kGpuDictionarySegmentBytes + kGpuDictionarySegmentBytes / 255U + 16U;

struct BlockDescriptor {
    BlockKind kind = BlockKind::Raw;
    std::uint8_t fill_value = 0;
    std::uint32_t uncompressed_len = 0;
    std::uint64_t encoded_offset = 0;
    std::uint32_t encoded_len = 0;
};

}  // namespace superzip
