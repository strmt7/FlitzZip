#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iterator>

namespace flitzzip::app {

inline constexpr std::array<int, 4> kPerformanceUpdateSecondsOptions{1, 3, 5, 10};

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
