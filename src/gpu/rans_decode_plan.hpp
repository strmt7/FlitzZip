#pragma once

#include <cstdint>
#include <type_traits>

namespace flitzzip::hip_detail {

// Private kernel input, built only from an admitted GpuRansBlockView. Offsets
// are bytes into separately owned input/output allocations; active output
// ranges must be disjoint. Models are the view's immutable full-width CDF
// workspace. Owners survive actual stream completion, even after failure.
struct RansDecodeSegment {
    std::uint32_t stream_offset;
    std::uint32_t stream_bytes;
    std::uint32_t output_offset;
    std::uint32_t decoded_bytes;
};

// One initialized result per active segment. Success requires exact stream
// consumption and terminal state L; it does not replace decoded-data CRC.
enum class RansDecodeStatus : std::uint32_t {
    success = 1U,
    invalid_plan,
    invalid_state,
    missing_model,
    truncated_stream,
    invalid_final_state,
};

static_assert(std::is_standard_layout_v<RansDecodeSegment> && std::is_trivially_copyable_v<RansDecodeSegment> &&
              sizeof(RansDecodeSegment) == 16U);

}  // namespace flitzzip::hip_detail
