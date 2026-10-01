#include "core/resource_usage.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <limits>

namespace superzip {

namespace {

// Purpose: Parse a bounded unsigned counter-instance field without locale or allocation.
// Inputs: `text` is a nonempty decimal or hexadecimal field and `base` is 10 or 16.
// Outputs: Returns the 32-bit value, or no value for invalid digits or overflow.
std::optional<std::uint32_t> counter_number(std::wstring_view text, std::uint32_t base) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint32_t result = 0;
    for (const auto value : text) {
        std::uint32_t digit = 16;
        if (value >= L'0' && value <= L'9') {
            digit = static_cast<std::uint32_t>(value - L'0');
        } else if (value >= L'a' && value <= L'f') {
            digit = static_cast<std::uint32_t>(value - L'a') + 10U;
        } else if (value >= L'A' && value <= L'F') {
            digit = static_cast<std::uint32_t>(value - L'A') + 10U;
        }
        if (digit >= base || result > (std::numeric_limits<std::uint32_t>::max() - digit) / base) {
            return std::nullopt;
        }
        result = result * base + digit;
    }
    return result;
}

// Purpose: Match a Windows adapter/process memory instance to an exact adapter and process identity.
// Inputs: `instance` is a PDH memory instance; `adapter_luid` and optional `process_id` select its owner.
// Outputs: Returns true only for a complete, well-formed matching instance.
bool matches_memory_instance(std::wstring_view instance, std::uint64_t adapter_luid,
                             std::optional<std::uint32_t> process_id) {
    if (process_id) {
        if (!instance.starts_with(L"pid_")) {
            return false;
        }
        const auto end = instance.find(L"_luid_", 4);
        if (end == std::wstring_view::npos || counter_number(instance.substr(4, end - 4), 10) != process_id) {
            return false;
        }
        instance.remove_prefix(end + 1);
    }
    constexpr auto prefix = std::wstring_view(L"luid_0x");
    if (!instance.starts_with(prefix)) {
        return false;
    }
    instance.remove_prefix(prefix.size());
    const auto split = instance.find(L"_0x");
    const auto physical = instance.find(L"_phys_", split == std::wstring_view::npos ? instance.size() : split + 3);
    if (split == std::wstring_view::npos || physical == std::wstring_view::npos) {
        return false;
    }
    const auto high = counter_number(instance.substr(0, split), 16);
    const auto low = counter_number(instance.substr(split + 3, physical - split - 3), 16);
    const auto node = counter_number(instance.substr(physical + 6), 10);
    return high && low && node && ((static_cast<std::uint64_t>(*high) << 32U) | *low) == adapter_luid;
}

// Purpose: Identify a physical GPU engine independently of its owning process and display label.
// Inputs: `instance` is a Windows GPU Engine counter instance using the pid/luid/phys/eng layout.
// Outputs: Returns a borrowed engine key, or an empty view for an unrecognized instance.
std::wstring_view gpu_engine_key(std::wstring_view instance) {
    if (!instance.starts_with(L"pid_")) {
        return {};
    }
    const auto process_end = instance.find(L"_luid_", 4);
    if (process_end == std::wstring_view::npos || process_end == 4) {
        return {};
    }
    const auto process = instance.substr(4, process_end - 4);
    if (!std::all_of(process.begin(), process.end(), [](wchar_t value) { return value >= L'0' && value <= L'9'; })) {
        return {};
    }
    const auto physical = instance.find(L"_phys_", process_end + 6);
    const auto engine = instance.find(L"_eng_", physical == std::wstring_view::npos ? instance.size() : physical + 6);
    const auto label = instance.find(L"_engtype_", engine == std::wstring_view::npos ? instance.size() : engine + 5);
    if (physical == std::wstring_view::npos || engine == std::wstring_view::npos || label == std::wstring_view::npos ||
        physical == process_end + 6 || engine == physical + 6 || label == engine + 5) {
        return {};
    }
    return instance.substr(process_end + 1, label - process_end - 1);
}

}  // namespace

// Purpose: Sum dedicated memory only within the selected adapter/process scope.
// Inputs: `samples`, `adapter_luid`, and optional `process_id` identify one PDH memory scope.
// Outputs: Returns its byte total, or no value for unavailable or invalid matching counters.
std::optional<std::uint64_t> selected_gpu_memory_usage(std::span<const GpuMemorySample> samples,
                                                       std::uint64_t adapter_luid,
                                                       std::optional<std::uint32_t> process_id) {
    std::optional<std::uint64_t> total;
    for (const auto& sample : samples) {
        if (!matches_memory_instance(sample.instance, adapter_luid, process_id)) {
            continue;
        }
        if (sample.dedicated_bytes < 0) {
            return std::nullopt;
        }
        const auto bytes = static_cast<std::uint64_t>(sample.dedicated_bytes);
        if (total.value_or(0U) > std::numeric_limits<std::uint64_t>::max() - bytes) {
            return std::nullopt;
        }
        total = total.value_or(0U) + bytes;
    }
    return total;
}

// Purpose: Aggregate Windows per-process GPU counters into total-system busiest-engine utilization.
// Inputs: `samples` borrows valid PDH instance names and their utilization percentages for one query interval.
// Outputs: Returns the busiest physical engine across GPUs, or no value for absent or inconsistent data.
std::optional<double> system_gpu_utilization(std::span<const GpuEngineSample> samples) {
    std::map<std::wstring_view, double> engines;
    for (const auto& sample : samples) {
        const auto key = gpu_engine_key(sample.instance);
        if (key.empty() || !std::isfinite(sample.utilization_percent) || sample.utilization_percent < 0.0 ||
            sample.utilization_percent > 100.0) {
            continue;
        }
        engines[key] += sample.utilization_percent;
    }
    if (engines.empty()) {
        return std::nullopt;
    }
    double busiest = 0.0;
    for (const auto& [key, utilization] : engines) {
        if (utilization > 100.0) {
            return std::nullopt;
        }
        busiest = std::max(busiest, utilization);
    }
    return busiest;
}

// Purpose: Reconcile independent VRAM counters into one display-safe snapshot.
// Inputs: All counters describe the same active GPU: HIP capacity/free memory, Windows adapter-wide
// dedicated usage, and SuperZip process dedicated usage.
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
