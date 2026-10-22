#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

namespace flitzzip::app {

struct CpuTimes {
    std::uint64_t kernel = 0;
    std::uint64_t user = 0;
};

class ProcessCpuSampler {
  public:
    // Purpose: Measure process CPU over the exact interval owned by two successful counter reads.
    // Inputs: Optional cumulative 100-nanosecond CPU ticks, their steady-clock timestamp and processor count.
    // Outputs: A capacity-normalized percentage, or no value while establishing or recovering a valid baseline.
    std::optional<double> sample(std::optional<CpuTimes> current, std::chrono::steady_clock::time_point now,
                                 std::uint32_t processors) noexcept {
        if (!current || processors == 0U) {
            baseline_.reset();
            return std::nullopt;
        }
        const auto previous = std::exchange(baseline_, Baseline{*current, now, processors});
        if (!previous || previous->processors != processors || now <= previous->time ||
            current->kernel < previous->ticks.kernel || current->user < previous->ticks.user) {
            return std::nullopt;
        }
        const double seconds = std::chrono::duration<double>(now - previous->time).count();
        const double ticks = static_cast<double>(current->kernel - previous->ticks.kernel) +
                             static_cast<double>(current->user - previous->ticks.user);
        return std::clamp((ticks / 10000000.0 / seconds / static_cast<double>(processors)) * 100.0, 0.0, 100.0);
    }

  private:
    struct Baseline {
        CpuTimes ticks;
        std::chrono::steady_clock::time_point time;
        std::uint32_t processors;
    };
    std::optional<Baseline> baseline_;
};

}  // namespace flitzzip::app
