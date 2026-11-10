#pragma once

#include <cstdint>

namespace flitzzip {

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
    GpuHuffman = 10,
    GpuCompound = 11,
    GpuBytePlane = 12,
    GpuBytePlaneContexts = 13,
};

// First native version defining a wire kind; zero means unknown, never version one.
// This capability query does not validate a descriptor's extents or codec payload.
inline constexpr std::uint32_t minimum_archive_version_for_kind(BlockKind kind) noexcept {
    switch (kind) {
    case BlockKind::Raw:
    case BlockKind::Fill:
    case BlockKind::Deflate:
    case BlockKind::Pattern:
        return 1U;
    case BlockKind::GpuPrefix:
        return 2U;
    case BlockKind::GpuAdaptivePrefix:
        return 3U;
    case BlockKind::GpuDictionary:
        return 4U;
    case BlockKind::GpuSparsePattern:
        return 5U;
    case BlockKind::CpuZstd:
        return 6U;
    case BlockKind::GpuLongSparsePattern:
        return 7U;
    case BlockKind::GpuHuffman:
        return 8U;
    case BlockKind::GpuCompound:
        return 9U;
    case BlockKind::GpuBytePlane:
        return 10U;
    case BlockKind::GpuBytePlaneContexts:
        return 11U;
    }
    return 0U;
}

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

}  // namespace flitzzip
