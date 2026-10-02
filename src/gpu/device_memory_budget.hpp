#pragma once

#include "core/resource_limits.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <mutex>

namespace superzip {

// Process-owned reservations are independent for each runtime device ordinal.
// This is admission accounting, not a physical VRAM reservation or a driver guarantee.
class DeviceMemoryBudget {
  public:
    // Purpose: Admit one allocation plan against its own device's live and aggregate limits.
    // Inputs: Nonnegative device ordinal, positive bytes, and a validated current-device memory snapshot.
    // Outputs: Reserves bytes atomically or returns false without changing accounting; metadata allocation may throw.
    [[nodiscard]] bool try_reserve(int device, std::size_t bytes, std::size_t free_bytes, std::size_t total_bytes) {
        if (device < 0 || bytes == 0U || total_bytes == 0U || free_bytes > total_bytes) {
            return false;
        }
        const auto reserve_target =
            std::max<std::size_t>(static_cast<std::size_t>(kDeviceMemoryReserveFloorBytes), total_bytes / 20U);
        const auto usable = free_bytes - std::min(free_bytes / 4U, reserve_target);
        if (bytes > usable) {
            return false;
        }
        std::scoped_lock lock(mutex_);
        const auto found = devices_.find(device);
        if (found == devices_.end()) {
            devices_.emplace(device, State{bytes, usable});
            return true;
        }
        auto& state = found->second;
        if (state.reserved_bytes > state.capacity_bytes || bytes > state.capacity_bytes - state.reserved_bytes) {
            return false;
        }
        state.reserved_bytes += bytes;
        return true;
    }

    // Purpose: Return an acquired plan to its original device, independent of the calling thread's selection.
    // Inputs: Device and positive byte extent from one live owner, released exactly once after buffer cleanup.
    // Outputs: Returns false for invalid releases without forgiving live reservations; removes idle device metadata.
    [[nodiscard]] bool release(int device, std::size_t bytes) noexcept {
        std::scoped_lock lock(mutex_);
        const auto found = devices_.find(device);
        if (bytes == 0U || found == devices_.end() || bytes > found->second.reserved_bytes) {
            return false;
        }
        found->second.reserved_bytes -= bytes;
        if (found->second.reserved_bytes == 0U) {
            devices_.erase(found);
        }
        return true;
    }

    // Purpose: Observe the outstanding plans for a specific device without querying or selecting a GPU.
    // Inputs: Runtime device ordinal; unknown devices have no reservations.
    // Outputs: Returns a synchronized byte snapshot, not measured resident memory.
    [[nodiscard]] std::size_t reserved_bytes(int device) const noexcept {
        std::scoped_lock lock(mutex_);
        const auto found = devices_.find(device);
        return found == devices_.end() ? 0U : found->second.reserved_bytes;
    }

  private:
    struct State {
        std::size_t reserved_bytes;
        std::size_t capacity_bytes;
    };

    mutable std::mutex mutex_;
    std::map<int, State> devices_;
};

}  // namespace superzip
