#pragma once

#include <cstdint>

namespace superzip {

struct VramUsage {
    std::uint64_t total_capacity_bytes = 0;
    std::uint64_t total_used_bytes = 0;
    std::uint64_t process_dedicated_bytes = 0;
};

// Purpose: Reconcile independent VRAM counters into one display-safe snapshot.
// Inputs: `total_capacity_bytes`/`free_bytes` come from the active GPU, `adapter_dedicated_used_bytes` is OS-wide
// dedicated VRAM usage, and `process_dedicated_bytes` is SuperZip process dedicated VRAM.
// Outputs: Returns bounded totals where process-dedicated usage never exceeds displayed total used VRAM.
[[nodiscard]] VramUsage reconcile_vram_usage(std::uint64_t total_capacity_bytes, std::uint64_t free_bytes,
                                             std::uint64_t adapter_dedicated_used_bytes,
                                             std::uint64_t process_dedicated_bytes) noexcept;

}  // namespace superzip
