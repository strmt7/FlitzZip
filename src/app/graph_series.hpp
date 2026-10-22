#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <windows.h>

namespace flitzzip::app {

// Purpose: Preserve missing or invalid percentage measurements as graph gaps rather than invented utilization.
// Inputs: A percentage and its independent availability flag. Outputs: Normalized finite [0,1], or NaN for a gap.
inline double graph_percent_sample(double percent, bool available) {
    return available && std::isfinite(percent) && percent >= 0.0 && percent <= 100.0
               ? percent / 100.0
               : std::numeric_limits<double>::quiet_NaN();
}

// Purpose: Partition a history into drawable contiguous runs without dropping missing samples from its timeline.
// Inputs: Borrowed normalized history and a consumer of original first-index/count pairs.
// Outputs: Calls the consumer once per nonempty finite run; missing and infinite samples never reach drawing.
template <class Consumer> void for_each_graph_segment(std::span<const double> values, Consumer&& consume) {
    std::size_t next = 0U;
    while (next < values.size()) {
        if (!std::isfinite(values[next])) {
            ++next;
            continue;
        }
        const auto first = next++;
        while (next < values.size() && std::isfinite(values[next])) {
            ++next;
        }
        consume(first, next - first);
    }
}

// Purpose: Retain the existing right-aligned 96-sample horizontal scale across gaps and short histories.
// Inputs: A positive-width plot, the complete nonempty history count, and an original index within that count.
// Outputs: Returns the original rounded/clamped physical x coordinate; rejects invalid geometry or indices.
inline int graph_sample_x(const RECT& rect, std::size_t sample_count, std::size_t index) {
    if (rect.right <= rect.left || sample_count == 0U || index >= sample_count) {
        throw std::invalid_argument("invalid graph sample geometry");
    }
    const int width = std::max(1, static_cast<int>(rect.right - rect.left - 1));
    constexpr std::size_t kFullGraphSampleCapacity = 96U;
    const auto capacity = std::max(sample_count, kFullGraphSampleCapacity);
    const double step = static_cast<double>(width) / static_cast<double>(capacity - 1U);
    const double first_x = static_cast<double>(rect.right - 1) - static_cast<double>(sample_count - 1U) * step;
    return std::clamp(static_cast<int>(std::round(first_x + static_cast<double>(index) * step)),
                      static_cast<int>(rect.left), static_cast<int>(rect.right));
}

}  // namespace flitzzip::app
