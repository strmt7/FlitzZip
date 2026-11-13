#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace flitzzip {

// Compare bounded address regions without adding an extent to a pointer or
// ordering unrelated C++ pointers. Empty regions never overlap. This checks
// aliasing only: callers still own admission of each live region and its size.
[[nodiscard]] inline bool byte_regions_overlap(std::span<const std::byte> lhs,
                                               std::span<const std::byte> rhs) noexcept {
    if (lhs.empty() || rhs.empty()) {
        return false;
    }
    const auto a = reinterpret_cast<std::uintptr_t>(lhs.data());
    const auto b = reinterpret_cast<std::uintptr_t>(rhs.data());
    return a <= b ? b - a < lhs.size() : a - b < rhs.size();
}

}  // namespace flitzzip
