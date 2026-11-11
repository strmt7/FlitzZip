#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace flitzzip {

enum class GpuObservationMode : std::uint8_t { Measured, CountersOnly };

// NoSamples is an empty accumulator, not an observed zero. Incomplete means
// measured and unrequested observations were combined; neither is a full timing.
enum class GpuTimingStatus : std::uint8_t { NoSamples, NotRequested, Measured, Incomplete, Unavailable };

[[nodiscard]] constexpr GpuObservationMode validate_gpu_observation_mode(GpuObservationMode mode) {
    if (mode != GpuObservationMode::Measured && mode != GpuObservationMode::CountersOnly) {
        throw std::invalid_argument("unknown GPU observation mode");
    }
    return mode;
}

// The factory acquires an operation-owned pair whose default value owns no
// handles. Unrequested observation must not call the external event provider;
// requested acquisition failures propagate without a substitute measurement.
template <typename Create> [[nodiscard]] auto make_gpu_observation_events(GpuObservationMode mode, Create&& create) {
    using Events = std::invoke_result_t<Create>;
    if (validate_gpu_observation_mode(mode) == GpuObservationMode::CountersOnly) {
        return Events{};
    }
    return std::forward<Create>(create)();
}

[[nodiscard]] constexpr std::string_view gpu_timing_status_name(GpuTimingStatus status) {
    switch (status) {
    case GpuTimingStatus::NoSamples:
        return "no_samples";
    case GpuTimingStatus::NotRequested:
        return "not_requested";
    case GpuTimingStatus::Measured:
        return "measured";
    case GpuTimingStatus::Incomplete:
        return "incomplete";
    case GpuTimingStatus::Unavailable:
        return "unavailable";
    }
    throw std::invalid_argument("unknown GPU timing status");
}

// Empty accumulation is neutral. Missing coverage stays incomplete even when
// another component is unavailable; it must never become a fully requested
// profile. A measured zero cannot replace unrequested observation.
[[nodiscard]] constexpr GpuTimingStatus combine_gpu_timing_status(GpuTimingStatus lhs, GpuTimingStatus rhs) {
    if (lhs > GpuTimingStatus::Unavailable || rhs > GpuTimingStatus::Unavailable) {
        throw std::invalid_argument("unknown GPU timing status");
    }
    if (lhs == GpuTimingStatus::NoSamples) {
        return rhs;
    }
    if (rhs == GpuTimingStatus::NoSamples || lhs == rhs) {
        return lhs;
    }
    if (lhs == GpuTimingStatus::Incomplete || rhs == GpuTimingStatus::Incomplete ||
        lhs == GpuTimingStatus::NotRequested || rhs == GpuTimingStatus::NotRequested) {
        return GpuTimingStatus::Incomplete;
    }
    if (lhs == GpuTimingStatus::Unavailable || rhs == GpuTimingStatus::Unavailable) {
        return GpuTimingStatus::Unavailable;
    }
    return GpuTimingStatus::Incomplete;
}

}  // namespace flitzzip
