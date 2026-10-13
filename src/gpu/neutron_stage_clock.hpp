#pragma once

#include "core/result.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace flitzzip::dictionary {

// Thread-affine ownership and completion ordering; the HIP adapter supplies the actual dispatch and wait.
template <typename Events> class NeutronStageClock {
  public:
    // Purpose: Own one event pair for the entire synchronous Neutron operation.
    // Inputs: A successfully created event owner on this operation's calling thread.
    // Outputs: Takes ownership without creating per-stage handles; destruction releases it on this thread.
    explicit NeutronStageClock(Events events) : events_(std::move(events)) {}

    NeutronStageClock(const NeutronStageClock&) = delete;
    NeutronStageClock& operator=(const NeutronStageClock&) = delete;
    NeutronStageClock(NeutronStageClock&&) = delete;
    NeutronStageClock& operator=(NeutronStageClock&&) = delete;

    template <typename Dispatch, typename Complete>
    // Purpose: Reuse timing handles only after dispatch completion and timestamp collection succeed.
    // Inputs: Dispatch borrows events for kernel_count ordered kernels; complete synchronizes and returns timing.
    // Outputs: Counts completed kernels and exact available timing; an exception poisons further dispatches.
    void measure(Dispatch&& dispatch, Complete&& complete, std::uint32_t kernel_count = 1U) {
        if (!reusable_ || kernel_count == 0U || kernel_count > std::numeric_limits<std::uint32_t>::max() - launches_) {
            throw GpuError("Neutron stage timing cannot reuse an incomplete or failed operation");
        }
        reusable_ = false;
        std::forward<Dispatch>(dispatch)(events_);
        const auto elapsed = std::forward<Complete>(complete)(events_);
        if (!elapsed || !std::isfinite(*elapsed) || *elapsed < 0.0 || !milliseconds_) {
            milliseconds_ = std::nullopt;
        } else {
            const auto total = *milliseconds_ + *elapsed;
            milliseconds_ = std::isfinite(total) ? std::optional<double>{total} : std::nullopt;
        }
        launches_ += kernel_count;
        reusable_ = true;
    }

    // Purpose: Observe only kernels whose completion callback returned successfully.
    // Inputs: None; the clock remains on its owning thread.
    // Outputs: Returns the completed launch count without querying a device.
    [[nodiscard]] std::uint32_t launches() const noexcept {
        return launches_;
    }

    // Purpose: Preserve unavailable timing rather than replace it with a fabricated measurement.
    // Inputs: None; the clock remains on its owning thread.
    // Outputs: Returns the accumulated finite milliseconds, or nullopt after any unavailable stage.
    [[nodiscard]] std::optional<double> milliseconds() const noexcept {
        return milliseconds_;
    }

  private:
    Events events_;
    std::optional<double> milliseconds_ = 0.0;
    std::uint32_t launches_ = 0U;
    bool reusable_ = true;
};

}  // namespace flitzzip::dictionary
