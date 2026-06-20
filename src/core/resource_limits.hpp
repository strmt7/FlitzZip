#pragma once

#include <cstdint>

namespace superzip {

// Archive chunk and block limits bound host allocations and GPU launch sizes.
constexpr std::uint64_t kMaxArchiveChunkBytes = 128ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kDefaultArchiveChunkBytes = kMaxArchiveChunkBytes;
constexpr std::uint32_t kDefaultArchiveBlockBytes = 1024U * 1024U;
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

// Host and device memory policy keeps SuperZip below resource-exhaustion thresholds.
constexpr std::uint32_t kHostMemoryTargetUsagePercent = 80U;
constexpr std::uint64_t kMaxPipelineMemoryBytes = 64ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kDeviceMemoryReserveFloorBytes = 64ULL * 1024ULL * 1024ULL;

// GPU pattern blocks store only short repeating motifs found by the HIP classifier.
constexpr std::uint32_t kMaxGpuPatternBytes = 256U;

// GPU prefix blocks encode fixed-size segments with static or adaptive HIP prefix codecs.
constexpr std::uint32_t kGpuPrefixSegmentBytes = 4U * 1024U;
constexpr std::uint32_t kGpuAdaptivePrefixSmallSymbols = 4U;
constexpr std::uint32_t kGpuAdaptivePrefixMediumSymbols = 16U;
constexpr std::uint32_t kGpuAdaptivePrefixLargeSymbols = 64U;
constexpr std::uint32_t kGpuAdaptivePrefixCodebookBytes =
    kGpuAdaptivePrefixSmallSymbols + kGpuAdaptivePrefixMediumSymbols + kGpuAdaptivePrefixLargeSymbols;

// SUZIP deflate levels mirror zlib/miniz levels while avoiding store-only mode.
constexpr int kMinCompressionLevel = 1;
constexpr int kDefaultCompressionLevel = 5;
constexpr int kMaxCompressionLevel = 9;

}  // namespace superzip
