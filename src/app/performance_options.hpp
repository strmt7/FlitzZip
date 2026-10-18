#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <span>
#include <string>

namespace flitzzip::app {

inline constexpr std::array<int, 4> kPerformanceUpdateSecondsOptions{1, 3, 5, 10};

// Purpose: Bind a selected drive index to the exact inventory that will be indexed.
// Inputs: One captured drive list and any signed UI index; no host query occurs here.
// Outputs: An in-range index, or no selection for an empty inventory; INT_MIN is safe.
inline std::optional<std::size_t> fixed_drive_index(std::span<const std::wstring> drives, int index) {
    if (drives.empty()) {
        return std::nullopt;
    }
    const auto signed_index = static_cast<std::int64_t>(index);
    const auto magnitude = static_cast<std::size_t>(signed_index < 0 ? -signed_index : signed_index);
    const auto remainder = magnitude % drives.size();
    return index < 0 && remainder != 0U ? drives.size() - remainder : remainder;
}

// Purpose: Choose the nearest supported performance-monitor interval without signed overflow.
// Inputs: `seconds` may be any int; equal distances prefer the earlier, shorter interval.
// Outputs: Returns a valid row index in the nonempty interval table.
inline int performance_update_index_for_seconds(int seconds) {
    const auto closest = std::min_element(kPerformanceUpdateSecondsOptions.begin(),
                                          kPerformanceUpdateSecondsOptions.end(), [seconds](int left, int right) {
                                              return std::abs(static_cast<std::int64_t>(left) - seconds) <
                                                     std::abs(static_cast<std::int64_t>(right) - seconds);
                                          });
    return static_cast<int>(std::distance(kPerformanceUpdateSecondsOptions.begin(), closest));
}

// Purpose: Normalize a persisted or in-memory performance interval to a supported duration.
// Inputs: `seconds` may be any int. Outputs: Returns 1, 3, 5 or 10 seconds, preferring shorter ties.
inline int normalize_performance_update_seconds(int seconds) {
    return kPerformanceUpdateSecondsOptions[static_cast<std::size_t>(performance_update_index_for_seconds(seconds))];
}

}  // namespace flitzzip::app
