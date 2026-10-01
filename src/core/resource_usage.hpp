#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace superzip {

struct VramUsage {
    std::uint64_t total_capacity_bytes = 0;
    std::uint64_t total_used_bytes = 0;
    std::uint64_t process_dedicated_bytes = 0;
};

struct GpuEngineSample {
    std::wstring_view instance;
    double utilization_percent = 0.0;
};

struct GpuMemorySample {
    std::wstring_view instance;
    std::int64_t dedicated_bytes = 0;
};

// Purpose: Sum dedicated memory counters only for the selected Windows adapter and optional process.
// Inputs: `samples` borrows PDH adapter/process memory instances; `adapter_luid` identifies the HIP adapter.
// `process_id` selects process counters when present, otherwise adapter-wide counters.
// Outputs: Returns the matching byte total, or no value for absent, negative, or overflowing matching data.
[[nodiscard]] std::optional<std::uint64_t>
selected_gpu_memory_usage(std::span<const GpuMemorySample> samples, std::uint64_t adapter_luid,
                          std::optional<std::uint32_t> process_id = std::nullopt);

// Purpose: Aggregate Windows per-process GPU counters into total-system busiest-engine utilization.
// Inputs: `samples` borrows valid PDH instance names and their utilization percentages for one query interval.
// Outputs: Returns the busiest physical engine across GPUs, or no value for absent or inconsistent data.
[[nodiscard]] std::optional<double> system_gpu_utilization(std::span<const GpuEngineSample> samples);

// Purpose: Reconcile independent VRAM counters into one display-safe snapshot.
// Inputs: All counters describe the same active GPU: HIP capacity/free memory, Windows adapter-wide
// dedicated usage, and SuperZip process dedicated usage.
// Outputs: Returns bounded totals where process-dedicated usage never exceeds displayed total used VRAM.
[[nodiscard]] VramUsage reconcile_vram_usage(std::uint64_t total_capacity_bytes, std::uint64_t free_bytes,
                                             std::uint64_t adapter_dedicated_used_bytes,
                                             std::uint64_t process_dedicated_bytes) noexcept;

}  // namespace superzip
