#pragma once

#include <cstdint>

namespace superzip {

// Archive chunk and block limits bound host allocations and GPU launch sizes.
constexpr std::uint64_t kMaxArchiveChunkBytes = 128ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kDefaultArchiveChunkBytes = kMaxArchiveChunkBytes;
constexpr std::uint32_t kDefaultArchiveBlockBytes = 8U * 1024U * 1024U;
constexpr std::uint32_t kMinArchiveBlockBytes = 4U * 1024U;
constexpr std::uint32_t kMaxArchiveBlockBytes = 16U * 1024U * 1024U;

// Concurrency limits cap CPU worker fan-out and queued archive chunks.
constexpr std::uint32_t kMaxArchiveWorkers = 64U;
constexpr std::uint32_t kMaxInflightArchiveChunks = 64U;

// Archive metadata limits prevent parser and in-memory index exhaustion.
constexpr std::uint32_t kMaxArchiveEntries = 250'000U;
constexpr std::uint32_t kMaxBlocksPerEntry = 4'000'000U;
constexpr std::uint32_t kMaxArchiveBlocks = 4'000'000U;
constexpr std::uint64_t kMaxArchiveIndexBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kMaxArchivePathBytes = 32U * 1024U;
constexpr std::uint32_t kMaxArchivePathComponentBytes = 1024U;
constexpr std::uint32_t kMaxArchivePathComponents = 256U;
constexpr std::uint64_t kMaxArchivePathMetadataBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kMaxSourceDirectoryDepth = 256U;

// Host and device memory policy keeps SuperZip below resource-exhaustion thresholds.
constexpr std::uint32_t kHostMemoryTargetUsagePercent = 80U;
constexpr std::uint64_t kMaxPipelineMemoryBytes = 64ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxExtractedOutputBytes = kMaxPipelineMemoryBytes;
constexpr std::uint64_t kDeviceMemoryReserveFloorBytes = 64ULL * 1024ULL * 1024ULL;

// Version four extends the bounded GPU pattern motif; older archives retain their original limit.
constexpr std::uint32_t kLegacyGpuPatternBytes = 256U;
constexpr std::uint32_t kMaxGpuPatternBytes = 16U * 1024U;
constexpr std::uint32_t kMaxGpuLongSparsePatternBytes = 1024U * 1024U;

// GPU prefix blocks encode fixed-size segments with static or adaptive HIP prefix codecs.
constexpr std::uint32_t kGpuPrefixSegmentBytes = 4U * 1024U;
constexpr std::uint32_t kGpuHuffmanLookupBits = 12U;
constexpr std::uint32_t kGpuHuffmanLookupEntries = 1U << kGpuHuffmanLookupBits;
constexpr std::uint32_t kGpuHuffmanLookupBytes = kGpuHuffmanLookupEntries * sizeof(std::uint16_t);
constexpr std::uint32_t kGpuAdaptivePrefixSmallSymbols = 4U;
constexpr std::uint32_t kGpuAdaptivePrefixMediumSymbols = 16U;
constexpr std::uint32_t kGpuAdaptivePrefixLargeSymbols = 64U;
constexpr std::uint32_t kGpuAdaptivePrefixCodebookBytes =
    kGpuAdaptivePrefixSmallSymbols + kGpuAdaptivePrefixMediumSymbols + kGpuAdaptivePrefixLargeSymbols;

// SUZIP CPU codec levels map to bounded Deflate or Zstandard effort without a store-only mode.
constexpr int kMinCompressionLevel = 1;
constexpr int kDefaultCompressionLevel = 5;
constexpr int kMaxCompressionLevel = 9;

}  // namespace superzip
