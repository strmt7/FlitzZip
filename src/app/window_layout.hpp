#pragma once

#include <algorithm>
#include <optional>
#include <windows.h>

namespace superzip::app {

inline constexpr int kDesignClientWidth = 1200;
inline constexpr int kDesignClientHeight = 760;
inline constexpr int kMinimumClientWidth = 960;
inline constexpr int kMinimumClientHeight = 600;

// Purpose: Describe work-area fitting; inputs: physical screen coordinates; outputs: bounds and minimum-size status.
struct WindowLayout {
    RECT bounds{};
    bool minimum_fits = false;
};

// Purpose: Fit the fixed-style product window without shrinking native controls below the tested minimum.
// Inputs: work is the monitor work area; desired supplies position; frame is total nonclient size; dpi is positive.
// Outputs: Returns bounded normal dimensions, or minimum dimensions with an accessible origin if space is insufficient.
// Invalid OS geometry returns no layout; this helper performs no window or display mutations.
inline std::optional<WindowLayout> make_window_layout(const RECT& work, const RECT& desired, SIZE frame, UINT dpi) {
    if (dpi == 0 || dpi > 960 || work.right <= work.left || work.bottom <= work.top || frame.cx < 0 || frame.cy < 0) {
        return std::nullopt;
    }
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    const LONG width = std::clamp(work.right - work.left - frame.cx, static_cast<LONG>(scale(kMinimumClientWidth)),
                                  static_cast<LONG>(scale(kDesignClientWidth))) +
                       frame.cx;
    const LONG height = std::clamp(work.bottom - work.top - frame.cy, static_cast<LONG>(scale(kMinimumClientHeight)),
                                   static_cast<LONG>(scale(kDesignClientHeight))) +
                        frame.cy;
    const LONG left = std::clamp(desired.left, work.left, std::max(work.left, work.right - width));
    const LONG top = std::clamp(desired.top, work.top, std::max(work.top, work.bottom - height));
    return WindowLayout{RECT{left, top, left + width, top + height},
                        width <= work.right - work.left && height <= work.bottom - work.top};
}

}  // namespace superzip::app
