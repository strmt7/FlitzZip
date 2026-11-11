#pragma once

#include "gpu/gpu_codec.hpp"

#include <limits>
#include <utility>

namespace flitzzip {

// Completion must return only after all dispatched work completes or throw.
// Its boolean selects event or stream completion; measure is called only for a
// requested profile. Failures propagate, while completed work remains counted
// even if the subsequent timing query fails. No callback escapes this scope.
template <typename Complete, typename Measure>
void observe_gpu_kernel_work(GpuTelemetry* telemetry, std::uint32_t launches, Complete&& complete, Measure&& measure) {
    const bool measured = gpu_timing_requested(telemetry);
    try {
        std::forward<Complete>(complete)(measured);
    } catch (...) {
        if (measured) {
            record_gpu_kernel_work(telemetry, 0U, std::numeric_limits<double>::quiet_NaN());
        }
        throw;
    }
    if (!measured) {
        record_gpu_completed_kernels(telemetry, launches);
        return;
    }
    double milliseconds;
    try {
        milliseconds = std::forward<Measure>(measure)();
    } catch (...) {
        record_gpu_kernel_work(telemetry, launches, std::numeric_limits<double>::quiet_NaN());
        throw;
    }
    record_gpu_kernel_work(telemetry, launches, milliseconds);
}

}  // namespace flitzzip
