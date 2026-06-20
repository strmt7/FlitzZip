#include "core/resource_usage.hpp"

#include <algorithm>

namespace superzip {

// Purpose: Reconcile independent VRAM counters into one display-safe snapshot.
// Inputs: `total_capacity_bytes`/`free_bytes` come from the active GPU, `adapter_dedicated_used_bytes` is OS-wide
// dedicated VRAM usage, and `process_dedicated_bytes` is SuperZip process dedicated VRAM.
// Outputs: Returns bounded totals where process-dedicated usage never exceeds displayed total used VRAM.
VramUsage reconcile_vram_usage(std::uint64_t total_capacity_bytes, std::uint64_t free_bytes,
                               std::uint64_t adapter_dedicated_used_bytes,
                               std::uint64_t process_dedicated_bytes) noexcept {
    const auto hip_used_bytes = total_capacity_bytes >= free_bytes ? total_capacity_bytes - free_bytes : 0U;
    const auto observed_used_bytes = std::max({hip_used_bytes, adapter_dedicated_used_bytes, process_dedicated_bytes});
    const auto total_used_bytes =
        total_capacity_bytes == 0U ? observed_used_bytes : std::min(total_capacity_bytes, observed_used_bytes);
    const auto bounded_process_bytes =
        total_used_bytes == 0U ? process_dedicated_bytes : std::min(process_dedicated_bytes, total_used_bytes);
    return VramUsage{total_capacity_bytes, total_used_bytes, bounded_process_bytes};
}

}  // namespace superzip
