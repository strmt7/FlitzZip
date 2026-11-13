#pragma once

#include "gpu/rans_count_plan.hpp"

#include <cstdint>
#include <span>
#include <type_traits>

namespace flitzzip::hip_detail {

inline constexpr std::uint32_t kRansModelSymbols = 256U;
inline constexpr std::uint32_t kRansModelCdfWords = 257U;
inline constexpr std::uint32_t kRansEncodeBatchSegments = 128U;

enum class RansModelStatus : std::uint32_t { success = 1U, invalid_plan, invalid_counts, invalid_model };

// Preparation produces one complete immutable row and a count/symbol ordered
// list of exactly alphabet_size positive symbols. Empty contexts are explicit.
// The host must admit every row status and the sum of source_mass against the
// original source length before normalizing or retaining any model result.
struct RansModelRow {
    std::uint32_t source_mass;
    std::uint32_t alphabet_size;
    RansModelStatus status;
};

// Admit completed GPU row metadata before model construction. Context zero is
// necessarily present because every nonempty source starts a segment there.
// This validates complete mass, not merely a per-entry or per-row upper bound;
// callers still keep the associated counts/order immutable and source-bound.
[[nodiscard]] inline bool rans_model_rows_admitted(std::span<const RansModelRow> rows, std::uint32_t source_bytes,
                                                   std::uint32_t context_bits) noexcept {
    if (source_bytes == 0U || source_bytes > kRansCountMaxBytes || context_bits > 8U ||
        rows.size() != (std::size_t{1U} << context_bits) || rows[0].source_mass == 0U) {
        return false;
    }
    std::uint64_t mass = 0U;
    for (const auto& row : rows) {
        if (row.status != RansModelStatus::success || row.source_mass > source_bytes || row.alphabet_size > 256U ||
            row.alphabet_size > row.source_mass || ((row.source_mass == 0U) != (row.alphabet_size == 0U))) {
            return false;
        }
        mass += row.source_mass;
    }
    return mass == source_bytes;
}

// A completed encoder result describes bytes inside its independently derived
// slot of at most 2*segment_bytes+4 bytes. Failed results never authorize copying
// partial streams. Host-admitted dense offsets describe their compacted layout.
struct RansEncodeResult {
    std::uint32_t stream_offset;
    std::uint32_t stream_bytes;
    RansModelStatus status;
};

// Kernel owners provide exact logical extents and disjoint live allocations on
// the current device, keep all prepared inputs immutable through completion,
// reset active statuses before each reuse, and dispatch one-dimensional grids.
// Model kernels require exactly 256 threads per block; one block owns one row.
// Source mass is bounded by the canonical statistics limit. Full-width u32 CDFs
// preserve singleton mass 65536 and zero offsets for missing contexts.
static_assert(std::is_trivially_copyable_v<RansModelRow> && sizeof(RansModelRow) == 12U);
static_assert(std::is_trivially_copyable_v<RansEncodeResult> && sizeof(RansEncodeResult) == 12U);

}  // namespace flitzzip::hip_detail
