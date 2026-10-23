#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace flitzzip::app {

inline constexpr std::array<int, 4> kQueueColumnMinimums{90, 70, 70, 120};
inline constexpr std::array<int, 5> kHistoryColumnMinimums{82, 78, 140, 160, 74};

// Purpose: Convert a finite nonnegative design width into an exactly rounded physical extent.
// Inputs: Width is in 96-DPI units; dpi is positive and the result must fit a Windows coordinate.
// Outputs: Returns physical pixels or throws invalid_argument before a numeric conversion can overflow.
inline int table_column_pixels(double width, int dpi) {
    const auto pixels = width * static_cast<double>(dpi) / 96.0;
    if (dpi <= 0 || !std::isfinite(pixels) || pixels < 0.0 ||
        pixels > static_cast<double>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("invalid table column extent");
    }
    return static_cast<int>(std::lround(pixels));
}

// Purpose: Fit proportional columns without shrinking a constrained column below its readable minimum.
// Inputs: Positive design widths/minimums, positive DPI and the physical space available for all columns.
// Outputs: Returns exact integer extents summing to the available space, or the minimum sum if space is too small.
template <std::size_t N>
std::array<int, N> fit_table_columns(const std::array<double, N>& design, const std::array<int, N>& minimum_design,
                                     int available, int dpi) {
    static_assert(N >= 2);
    std::array<int, N> minimum{}, requested{}, result{};
    std::array<bool, N> fixed{};
    std::int64_t minimum_total = 0;
    double weight = 0.0;
    for (std::size_t i = 0; i < N; ++i) {
        minimum[i] = table_column_pixels(minimum_design[i], dpi);
        requested[i] = std::max(minimum[i], table_column_pixels(design[i], dpi));
        if (minimum[i] <= 0) {
            throw std::invalid_argument("table column minimum must be positive");
        }
        minimum_total += minimum[i];
        weight += requested[i];
    }
    if (available < 0 || minimum_total > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("invalid table column capacity");
    }
    int remaining = std::max(available, static_cast<int>(minimum_total));
    // Fix undersized proportional shares and redistribute their cost only among unconstrained columns.
    for (std::size_t pass = 0; pass < N; ++pass) {
        if (weight == 0.0) {
            break;
        }
        bool changed = false;
        const double ratio = static_cast<double>(remaining) / weight;
        for (std::size_t i = 0; i < N; ++i) {
            if (!fixed[i] && requested[i] * ratio < minimum[i]) {
                fixed[i] = true;
                result[i] = minimum[i];
                remaining -= minimum[i];
                weight -= requested[i];
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }
    auto unfixed_minimum = minimum_total;
    for (std::size_t i = 0; i < N; ++i) {
        if (fixed[i]) {
            unfixed_minimum -= minimum[i];
        }
    }
    for (std::size_t i = 0; i < N; ++i) {
        if (fixed[i]) {
            continue;
        }
        unfixed_minimum -= minimum[i];
        const auto share = std::lround(static_cast<double>(remaining) * requested[i] / weight);
        result[i] = static_cast<int>(std::clamp<std::int64_t>(share, minimum[i], remaining - unfixed_minimum));
        remaining -= result[i];
        weight -= requested[i];
    }
    return result;
}

// Purpose: Resize one displayed adjacent pair in physical pixels, without rescaling other columns a second time.
// Inputs: The fitted physical baseline/minimums, valid separator, signed pointer displacement and positive DPI.
// Outputs: Returns design widths preserving the pair total and all other physical extents; invalid geometry throws.
template <std::size_t N>
std::array<double, N> resize_table_columns(const std::array<int, N>& baseline, const std::array<int, N>& minimum_design,
                                           std::size_t separator, std::int64_t displacement, int dpi) {
    static_assert(N >= 2);
    if (separator >= N - 1) {
        throw std::invalid_argument("invalid table separator");
    }
    std::array<double, N> result{};
    std::array<int, N> minimum{};
    for (std::size_t i = 0; i < N; ++i) {
        minimum[i] = table_column_pixels(minimum_design[i], dpi);
        if (minimum[i] <= 0 || baseline[i] < minimum[i]) {
            throw std::invalid_argument("invalid table resize baseline");
        }
        result[i] = static_cast<double>(baseline[i]) * 96.0 / dpi;
    }
    const auto total = static_cast<std::int64_t>(baseline[separator]) + baseline[separator + 1];
    const auto lower = static_cast<std::int64_t>(minimum[separator]) - baseline[separator];
    const auto upper = static_cast<std::int64_t>(baseline[separator + 1]) - minimum[separator + 1];
    const auto left = baseline[separator] + std::clamp(displacement, lower, upper);
    result[separator] = static_cast<double>(left) * 96.0 / dpi;
    result[separator + 1] = static_cast<double>(total - left) * 96.0 / dpi;
    return result;
}

}  // namespace flitzzip::app
