#pragma once

#include "core/resource_limits.hpp"

#include <cstdint>

namespace flitzzip::hip_detail {

// Private scheduling and workspace bounds, independent of device wave size.
inline constexpr std::uint32_t kRansCountWords = 256U * 256U;
static_assert(kMaxArchiveBlockBytes <= 0xFFFFFFFFULL);
inline constexpr std::uint32_t kRansCountMaxBytes = static_cast<std::uint32_t>(kMaxArchiveBlockBytes);
inline constexpr std::uint32_t kRansCountTileBytes = 4096U;
inline constexpr std::uint32_t kRansCountTilesPerDispatch = 16U;
inline constexpr std::uint32_t kRansCountBucketCount = 16U;
inline constexpr std::uint32_t kRansCorrectionBatch = 256U;
inline constexpr std::uint32_t kRansProjectionBatch = 4096U;

enum class RansCountStatus : std::uint32_t {
    success = 1U,
    invalid_plan,
    invalid_counts,
};

// All table extents below are logical u32 counts. The host owns disjoint
// allocations and zeroes accumulating tables once before disjoint work ranges.
// Source and base statistics remain immutable through completion. A successful
// status does not admit partial batches or replace full-source mass validation.
// Launches are one-dimensional; initialize every active status to non-success.
// Invalid launch dimensions leave those statuses and all data unchanged.

}  // namespace flitzzip::hip_detail
