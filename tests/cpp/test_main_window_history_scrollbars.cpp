#include "native_window_history_fixture.hpp"

namespace {

using namespace flitzzip_test::native_window;

enum class HistoryScrollbar { Table, Details };

// Purpose: Locate the public scrollbar track for either independent History viewport.
// Inputs: Current DPI layout and viewport. Outputs: Client rectangle, without reading controller offsets.
RECT history_track(const HistorySession& s, HistoryScrollbar which) {
    const auto form = s.layout();
    if (which == HistoryScrollbar::Details) {
        return RECT{form.details.right - s.ui.px(12), form.details.top + s.ui.px(10), form.details.right - s.ui.px(7),
                    form.details.bottom - s.ui.px(8)};
    }
    return RECT{form.table.right - s.ui.px(18), form.table.top + s.ui.px(44), form.table.right - s.ui.px(6),
                form.table.bottom - s.ui.px(8)};
}

// Purpose: Observe the actual thumb independently of the product's scroll-position calculation.
// Inputs: Painted track. Outputs: First and last thumb pixels distinguished from the track background.
std::pair<int, int> history_thumb(HistorySession& s, HistoryScrollbar which) {
    const auto track = history_track(s, which);
    s.ui.paint();
    int first = -1;
    int last = -1;
    for (int y = track.top; y < track.bottom; ++y) {
        const auto color = GetPixel(s.ui.frame.dc(), (track.left + track.right) / 2, y);
        if (color == RGB(64, 83, 90) || color == RGB(63, 82, 89)) {
            if (first < 0) {
                first = y;
            }
            last = y;
        }
    }
    REQUIRE_TRUE(first >= track.top && last >= first);
    return {first, last};
}

// Purpose: Require the real scrollbar endpoint rather than reproducing its interpolation formula.
// Inputs: Viewport and expected end. Outputs: Thumb touches the corresponding track edge within native rounding.
void require_history_endpoint(HistorySession& s, HistoryScrollbar which, bool bottom) {
    const auto track = history_track(s, which);
    require_native_observation(
        [&] {
            const auto [first, last] = history_thumb(s, which);
            return bottom ? last >= track.bottom - s.ui.px(2) : first <= track.top + s.ui.px(2);
        },
        "History scrollbar did not reach its independently expected endpoint");
}

// Purpose: Inspect the capture owned by this exact UI thread rather than the calling test thread.
// Inputs: Hidden owned window. Outputs: Actual native capture owner, with no desktop-global window lookup.
HWND history_capture(const HistorySession& s) {
    GUITHREADINFO state{};
    state.cbSize = sizeof(state);
    REQUIRE_TRUE(GetGUIThreadInfo(GetWindowThreadProcessId(s.ui.window, nullptr), &state) != FALSE);
    return state.hwndCapture;
}

// Purpose: Begin dragging the actual visible thumb and prove native capture admission.
// Inputs: Owned viewport. Outputs: One real press; caller must release or cancel it.
void begin_history_drag(HistorySession& s, HistoryScrollbar which) {
    const auto track = history_track(s, which);
    const auto [first, last] = history_thumb(s, which);
    REQUIRE_TRUE(last - first + 1 < track.bottom - track.top - s.ui.px(2));
    HiddenController::send(s.ui.window, WM_LBUTTONDOWN, MK_LBUTTON,
                           MAKELPARAM((track.left + track.right) / 2, (first + last) / 2));
    REQUIRE_TRUE(history_capture(s) == s.ui.window);
}

// Purpose: Deliver a bounded drag movement followed by native release.
// Inputs: Owned viewport and requested client Y. Outputs: One move/release sequence and released native capture.
void finish_history_drag(HistorySession& s, HistoryScrollbar which, int y) {
    const auto track = history_track(s, which);
    const auto point = MAKELPARAM((track.left + track.right) / 2, y);
    HiddenController::send(s.ui.window, WM_MOUSEMOVE, MK_LBUTTON, point);
    HiddenController::send(s.ui.window, WM_LBUTTONUP, 0, point);
    REQUIRE_TRUE(history_capture(s) != s.ui.window);
}

}  // namespace

// Purpose: Clamp History table dragging at both ends and stop movement when the press is released.
// Inputs: Overflowing real results and out-of-track drags. Outputs: Exact boundary row sequence and released capture.
TEST_CASE(main_window_history_scrollbar_drag_bounds_and_release) {
    HistorySession s;
    s.seed(24);
    const auto track = history_track(s, HistoryScrollbar::Table);
    const auto table = s.layout().table;
    const int visible = (table.bottom - table.top - s.ui.px(36)) / s.ui.px(34);
    const auto first = 24 - visible;
    begin_history_drag(s, HistoryScrollbar::Table);
    finish_history_drag(s, HistoryScrollbar::Table, track.bottom + s.ui.px(100));
    require_history_endpoint(s, HistoryScrollbar::Table, true);
    for (int row = 0; row < 3; ++row) {
        s.require_row(row, static_cast<std::size_t>(first + row));
    }
    HiddenController::send(s.ui.window, WM_MOUSEMOVE, 0, MAKELPARAM(track.left, track.top));
    require_history_endpoint(s, HistoryScrollbar::Table, true);
    begin_history_drag(s, HistoryScrollbar::Table);
    finish_history_drag(s, HistoryScrollbar::Table, track.top - s.ui.px(100));
    require_history_endpoint(s, HistoryScrollbar::Table, false);
    s.require_row(0, 0);
    s.require_row(1, 1);
    s.finish();
}

// Purpose: Cancel actual table capture and retain ordinary page-click scrolling afterward.
// Inputs: Begun drag, WM_CANCELMODE, stale movement and a track click. Outputs: No stale drag and exact page movement.
TEST_CASE(main_window_history_scrollbar_cancel_then_page_click) {
    HistorySession s;
    s.seed(24);
    const auto track = history_track(s, HistoryScrollbar::Table);
    begin_history_drag(s, HistoryScrollbar::Table);
    HiddenController::send(s.ui.window, WM_CANCELMODE);
    REQUIRE_TRUE(history_capture(s) != s.ui.window);
    HiddenController::send(s.ui.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(track.left, track.bottom));
    require_history_endpoint(s, HistoryScrollbar::Table, false);
    click_native_point(s.ui.window, POINT{(track.left + track.right) / 2, track.bottom - s.ui.px(2)});
    const auto table = s.layout().table;
    const int visible = (table.bottom - table.top - s.ui.px(36)) / s.ui.px(34);
    s.require_row(0, static_cast<std::size_t>(visible));
    s.require_row(1, static_cast<std::size_t>(visible + 1));
    s.finish();
}

// Purpose: Drag overflowing details independently and recover exact top text on a reverse drag.
// Inputs: Long real path, both endpoints and a post-release move. Outputs: Distinct bottom, stable release and exact
// top.
TEST_CASE(main_window_history_details_drag_bounds_and_release) {
    HistorySession s;
    s.compress(true, true);
    s.open();
    const auto area = history_detail_region(s);
    const auto top = settled_history_pixels(s, area);
    const auto track = history_track(s, HistoryScrollbar::Details);
    begin_history_drag(s, HistoryScrollbar::Details);
    finish_history_drag(s, HistoryScrollbar::Details, track.bottom + s.ui.px(100));
    require_history_endpoint(s, HistoryScrollbar::Details, true);
    const auto bottom = settled_history_pixels(s, area);
    REQUIRE_TRUE(bottom != top);
    HiddenController::send(s.ui.window, WM_MOUSEMOVE, 0, MAKELPARAM(track.left, track.top));
    REQUIRE_TRUE(settled_history_pixels(s, area) == bottom);
    begin_history_drag(s, HistoryScrollbar::Details);
    finish_history_drag(s, HistoryScrollbar::Details, track.top - s.ui.px(100));
    require_history_endpoint(s, HistoryScrollbar::Details, false);
    REQUIRE_TRUE(settled_history_pixels(s, area) == top);
    s.finish();
}

// Purpose: End details dragging on capture cancellation and retain both track-page directions.
// Inputs: Cancelled real capture, stale move and page clicks. Outputs: Original text, then bottom and top endpoints.
TEST_CASE(main_window_history_details_capture_cancel_and_track_pages) {
    HistorySession s;
    s.compress(true, true);
    s.open();
    const auto area = history_detail_region(s);
    const auto top = settled_history_pixels(s, area);
    const auto track = history_track(s, HistoryScrollbar::Details);
    begin_history_drag(s, HistoryScrollbar::Details);
    HiddenController::send(s.ui.window, WM_CANCELMODE);
    REQUIRE_TRUE(history_capture(s) != s.ui.window);
    HiddenController::send(s.ui.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(track.left, track.bottom));
    REQUIRE_TRUE(settled_history_pixels(s, area) == top);
    click_native_point(s.ui.window, POINT{(track.left + track.right) / 2, track.bottom - s.ui.px(1)});
    require_history_endpoint(s, HistoryScrollbar::Details, true);
    click_native_point(s.ui.window, POINT{(track.left + track.right) / 2, track.top + s.ui.px(1)});
    require_history_endpoint(s, HistoryScrollbar::Details, false);
    REQUIRE_TRUE(settled_history_pixels(s, area) == top);
    s.finish();
}

// Purpose: Clamp column resizing to its public minimum and end a later resize when capture is cancelled.
// Inputs: Header drag beyond the left bound and cancelled second drag. Outputs: Independent separator/text geometry.
TEST_CASE(main_window_history_column_resize_minimum_and_capture_cancel) {
    HistorySession s;
    s.seed(5);
    const auto header = s.row_rect(-1);
    const auto before = s.cells(-1);
    const auto grip = MAKELPARAM(before[0].right + s.ui.px(8), (header.top + header.bottom) / 2);
    HiddenController::send(s.ui.window, WM_LBUTTONDOWN, MK_LBUTTON, grip);
    REQUIRE_TRUE(history_capture(s) == s.ui.window);
    const auto outside_point = MAKELPARAM(header.left - s.ui.px(1000), (header.top + header.bottom) / 2);
    HiddenController::send(s.ui.window, WM_MOUSEMOVE, MK_LBUTTON, outside_point);
    HiddenController::send(s.ui.window, WM_LBUTTONUP, 0, outside_point);
    s.ui.paint();
    const int separator = header.left + s.ui.px(14 + 82);
    REQUIRE_EQ(GetPixel(s.ui.frame.dc(), separator, header.top + s.ui.px(10)), flitzzip::app::kBorder);
    const RECT operation{separator + s.ui.px(8), before[1].top, before[1].right, before[1].bottom};
    REQUIRE_TRUE(native_cell_text_matches(s.ui.frame.dc(), operation, GetDpiForWindow(s.ui.window), L"Operation",
                                          flitzzip::app::kMuted,
                                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS));
    const auto second = MAKELPARAM(before[1].right + s.ui.px(8), (header.top + header.bottom) / 2);
    HiddenController::send(s.ui.window, WM_LBUTTONDOWN, MK_LBUTTON, second);
    HiddenController::send(s.ui.window, WM_CANCELMODE);
    REQUIRE_TRUE(history_capture(s) != s.ui.window);
    const auto cancelled = s.ui.pixels(header);
    HiddenController::send(s.ui.window, WM_MOUSEMOVE, MK_LBUTTON,
                           MAKELPARAM(header.right + s.ui.px(100), (header.top + header.bottom) / 2));
    REQUIRE_TRUE(s.ui.pixels(header) == cancelled);
    s.finish();
}

// Purpose: Distinguish two matching filtered rows through their actual operation details, not identical status labels.
// Inputs: Multiple successful ZIP jobs at different owned paths. Outputs: Distinct details and exact reselection.
TEST_CASE(main_window_history_filtered_selection_preserves_original_identity) {
    HistorySession s;
    s.seed(24);
    s.filter(1, 1);
    REQUIRE_TRUE(s.visible_rows().size() > 1);
    click_native_control(s.ui.window, s.row_rect(0));
    s.require_selected(0);
    const auto first = s.ui.pixels(history_detail_region(s));
    click_native_control(s.ui.window, s.row_rect(1));
    s.require_selected(1);
    REQUIRE_TRUE(s.ui.pixels(history_detail_region(s)) != first);
    click_native_control(s.ui.window, s.row_rect(0));
    s.require_selected(0);
    REQUIRE_TRUE(s.ui.pixels(history_detail_region(s)) == first);
    s.finish();
}
