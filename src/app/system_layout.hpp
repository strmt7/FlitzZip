#pragma once

#include <algorithm>
#include <string_view>
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

// Purpose: Separate graph and wrapped details; inputs: physical pixels; outputs: non-overlapping body rectangles.
struct PerformanceCardBody {
    RECT plot{};
    RECT detail{};
};

// Purpose: Reserve native-font text height without changing graph samples or cadence.
// Inputs: dc has the detail font selected; card is a tested metric card; dpi is positive; text is bounded UI text.
// Outputs: Returns contained text/plot bounds, preserving the original detail height when text already fits.
inline PerformanceCardBody make_performance_card_body(HDC dc, const RECT& card, UINT dpi, std::wstring_view text) {
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    PerformanceCardBody result{};
    result.plot = {card.left + scale(12), card.top + scale(70), card.right - scale(12), card.bottom - scale(58)};
    RECT measured{result.plot.left, 0, result.plot.right, 0};
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    const int available = std::max(scale(42), static_cast<int>(card.bottom - result.plot.top) - scale(64));
    const int height = std::clamp(static_cast<int>(measured.bottom), scale(42), available);
    result.detail = {result.plot.left, card.bottom - scale(8) - height, result.plot.right, card.bottom - scale(8)};
    result.plot.bottom = result.detail.top - scale(8);
    return result;
}

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
