#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace superzip {

// Process-wide pin admission; reservations include any allocation whose release failed.
class PinnedHostBudget {
  public:
    static constexpr std::uint64_t maximum_bytes = 512ULL * 1024ULL * 1024ULL;

    // Purpose: Derive the aggregate pin ceiling from a live host RAM snapshot.
    // Inputs: available_bytes is available physical RAM, not total installed RAM.
    // Outputs: Returns at most 512 MiB and at most one-sixteenth of that snapshot.
    [[nodiscard]] static std::uint64_t allowance(std::uint64_t available_bytes) noexcept {
        return std::min(maximum_bytes, available_bytes / 16U);
    }

    // Purpose: Reserve pin bytes without exceeding a fixed ceiling or a live host-relative allowance.
    // Inputs: bytes is a positive allocation extent; available_bytes is current available physical RAM.
    // Outputs: Returns admission and atomically reserves bytes; failed admission leaves accounting unchanged.
    [[nodiscard]] bool try_reserve(std::uint64_t bytes, std::uint64_t available_bytes) noexcept {
        const auto limit = allowance(available_bytes);
        if (bytes == 0U || bytes > limit) {
            return false;
        }
        auto current = reserved_.load(std::memory_order_relaxed);
        while (current <= limit - bytes) {
            if (reserved_.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    // Purpose: Return a reservation only after its matching allocation failed or was successfully freed.
    // Inputs: bytes belongs to one live reservation, returned exactly once by its owner.
    // Outputs: Decreases process pin accounting without throwing.
    void release(std::uint64_t bytes) noexcept {
        reserved_.fetch_sub(bytes, std::memory_order_relaxed);
    }

    // Purpose: Observe outstanding reservations without claiming an OS resident-memory measurement.
    // Inputs: None; concurrent allocations or releases may change the snapshot immediately.
    // Outputs: Returns reserved bytes, including conservatively retained failed-release reservations.
    [[nodiscard]] std::uint64_t reserved_bytes() const noexcept {
        return reserved_.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<std::uint64_t> reserved_{0};
};

}  // namespace superzip
