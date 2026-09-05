#pragma once

#include <windows.h>

namespace superzip::app {

// Purpose: Share System page geometry between rendering, dropdown placement, and viewport tests.
// Inputs: Rectangles are physical client pixels at the caller's DPI.
// Outputs: Describes a compact runtime band followed by the existing monitor grid.
struct SystemLayout {
    RECT area{};
    RECT runtime{};
    RECT policy{};
    RECT architecture{};
    RECT monitor{};
};

// Purpose: Lay out the System page without filesystem, device, or window queries.
// Inputs: content is the page rectangle, excluding shell chrome; dpi is a positive Windows monitor DPI.
// Outputs: Returns shared rectangles; callers provide at least the tested 960-by-600-DIP client viewport.
inline SystemLayout make_system_layout(const RECT& content, UINT dpi) {
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    SystemLayout result{};
    result.area = {content.left + scale(30), content.top + scale(22), content.right - scale(30),
                   content.bottom - scale(22)};
    const auto& area = result.area;
    result.runtime = {area.left, area.top + scale(54), area.right, area.top + scale(86)};
    const auto mid = area.left + (area.right - area.left) / 2;
    result.policy = {area.left, area.top + scale(96), mid, area.top + scale(124)};
    result.architecture = {mid + scale(16), result.policy.top, area.right, result.policy.bottom};
    result.monitor = {area.left, area.top + scale(146), area.right, area.bottom};
    return result;
}

}  // namespace superzip::app
