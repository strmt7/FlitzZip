#include "native_window_visual_fixture.hpp"

namespace {

using namespace flitzzip_test::native_window;

// Purpose: Drive the public About modal and observe generated notice text and real scrollbar bounds.
// Inputs: Hidden native controller. Outputs: Per-case ownership and externally visible modal geometry.
class LicenseSession final {
  public:
    VisualSession ui;
    RECT panel{};
    RECT viewport{};
    RECT heading_region{};
    std::vector<COLORREF> closed_heading;

    // Purpose: Open the license surface through keyboard focus, without private commands or child-window assumptions.
    // Inputs: Fresh hidden About page. Outputs: Visible-in-capture modal containing the manifest's FlitzZip heading.
    LicenseSession() {
        const auto content = native_content_rect(ui.window);
        const int width =
            std::min(ui.px(800), std::max(ui.px(620), static_cast<int>(content.right - content.left) - ui.px(110)));
        const int height =
            std::min(ui.px(540), std::max(ui.px(410), static_cast<int>(content.bottom - content.top) - ui.px(90)));
        panel.left = content.left + (content.right - content.left - width) / 2;
        panel.top = content.top + (content.bottom - content.top - height) / 2;
        panel.right = panel.left + width;
        panel.bottom = panel.top + height;
        viewport =
            RECT{panel.left + ui.px(34), panel.top + ui.px(112), panel.right - ui.px(38), panel.bottom - ui.px(78)};
        click_native_navigation(ui.window, 7);
        heading_region =
            RECT{panel.left + ui.px(24), panel.top + ui.px(18), panel.left + ui.px(160), panel.top + ui.px(54)};
        closed_heading = ui.pixels(heading_region);
        open();
    }

    // Purpose: Reopen using the last About focus target. Inputs: Closed modal. Outputs: First-tab top text asserted.
    void open() {
        ui.key(VK_END);
        ui.key(VK_RETURN);
        require_heading(L"== FlitzZip ==");
        REQUIRE_TRUE(ui.pixels(heading_region) != closed_heading);
    }

    // Purpose: Independently render the literal first notice heading using the public native typography.
    // Inputs: Manifest title, not generated product text. Outputs: Exact first-line RGB equality or failure.
    void require_heading(std::wstring_view text) {
        const RECT line{viewport.left, viewport.top, viewport.left + ui.px(230), viewport.top + ui.px(17)};
        QueueCapture expected(line.right - line.left, line.bottom - line.top);
        RECT bounds{0, 0, line.right - line.left, line.bottom - line.top};
        SetDCBrushColor(expected.dc(), flitzzip::app::kPanel2);
        REQUIRE_TRUE(FillRect(expected.dc(), &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH))) != FALSE);
        using Font = std::unique_ptr<std::remove_pointer_t<HFONT>, decltype(&DeleteObject)>;
        Font font(CreateFontW(-MulDiv(9, static_cast<int>(GetDpiForWindow(ui.window)), 72), 0, 0, 0, FW_NORMAL, FALSE,
                              FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, L"Segoe UI"),
                  DeleteObject);
        REQUIRE_TRUE(font != nullptr);
        flitzzip::app::GdiDcState saved(expected.dc());
        REQUIRE_TRUE(saved.valid());
        REQUIRE_TRUE(SelectObject(expected.dc(), font.get()) != nullptr);
        SetBkMode(expected.dc(), TRANSPARENT);
        SetTextColor(expected.dc(), flitzzip::app::kText);
        REQUIRE_TRUE(DrawTextW(expected.dc(), text.data(), static_cast<int>(text.size()), &bounds,
                               DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX) != 0);
        std::vector<COLORREF> reference;
        for (int y = 0; y < bounds.bottom; ++y) {
            for (int x = 0; x < bounds.right; ++x) {
                reference.push_back(GetPixel(expected.dc(), x, y));
            }
        }
        require_native_observation([&] { return ui.pixels(line) == reference; },
                                   "License heading differs from its manifest title");
    }

    // Purpose: Observe the rendered scrollbar, independently of private offsets and font-height calculations.
    // Inputs: Active overflowing notice. Outputs: First and last colored thumb pixels on the track centerline.
    std::pair<int, int> thumb() {
        ui.paint();
        int first = -1;
        int last = -1;
        for (int y = viewport.top + ui.px(3); y < viewport.bottom - ui.px(3); ++y) {
            const auto color = GetPixel(ui.frame.dc(), viewport.right - ui.px(2), y);
            if (GetRValue(color) > 40 && GetGValue(color) > 50 && GetBValue(color) > 60) {
                if (first < 0) {
                    first = y;
                }
                last = y;
            }
        }
        REQUIRE_TRUE(first >= 0 && last > first);
        return {first, last};
    }

    // Purpose: Await an animated native endpoint. Inputs: Desired top or bottom. Outputs: Proven track bound.
    void require_endpoint(bool bottom) {
        require_native_observation(
            [&] {
                const auto range = thumb();
                return bottom ? range.second >= viewport.bottom - ui.px(4) : range.first <= viewport.top + ui.px(4);
            },
            "License scrollbar did not reach its native endpoint");
    }

    // Purpose: Snapshot only notice body pixels, excluding hover, timestamps and scrollbar decoration.
    // Inputs: Active modal. Outputs: A small exact text-region sample.
    std::vector<COLORREF> body() {
        return ui.pixels(RECT{viewport.left, viewport.top, viewport.left + ui.px(230), viewport.top + ui.px(48)});
    }

    // Purpose: Observe completion of smooth scrolling without inferring text movement from coarse scrollbar pixels.
    // Inputs: Active notice and a bounded observation window. Outputs: Text unchanged for 160 ms, or failure.
    std::vector<COLORREF> settled_body() {
        auto previous = body();
        auto unchanged_since = std::chrono::steady_clock::now();
        require_native_observation(
            [&] {
                const auto next = body();
                const auto now = std::chrono::steady_clock::now();
                if (next != previous) {
                    previous = next;
                    unchanged_since = now;
                }
                return now - unchanged_since >= 160ms;
            },
            "License text did not finish its scroll animation");
        return previous;
    }

    // Purpose: Click a tab through its public hit region. Inputs: First/second ordinal. Outputs: Native mouse action.
    void tab(int index) {
        const int left = panel.left + ui.px(24 + index * 136);
        click_native_control(ui.window, RECT{left, panel.top + ui.px(62), left + ui.px(128), panel.top + ui.px(96)});
    }

    // Purpose: Locate the visible modal Close control. Inputs: Current panel. Outputs: Physical button bounds.
    RECT close_button() const {
        return RECT{panel.right - ui.px(134), panel.bottom - ui.px(58), panel.right - ui.px(24),
                    panel.bottom - ui.px(22)};
    }

    // Purpose: Prove dismissal by successfully navigating the underlying page. Inputs: Closed modal. Outputs: Queue.
    void require_closed() {
        require_native_observation([&] { return ui.pixels(heading_region) == closed_heading; },
                                   "Deferred license dismissal did not restore the About surface");
        click_native_navigation(ui.window, 0);
        require_native_navigation_selection(ui.window, ui.frame.dc(), 0);
    }
};

}  // namespace

// Purpose: Keep all legal keyboard scroll directions inside actual notice bounds and recover exact initial text.
// Inputs: Native Home/End, line and page movements. Outputs: Both endpoints, intermediate motion and top identity.
TEST_CASE(main_window_licenses_keyboard_scroll_bounds_and_exact_home) {
    LicenseSession s;
    const auto initial = s.body();
    s.ui.key(VK_END);
    s.require_endpoint(true);
    const auto bottom = s.body();
    REQUIRE_TRUE(bottom != initial);
    for (const auto key : {VK_DOWN, VK_NEXT, VK_END}) {
        s.ui.key(key);
        s.require_endpoint(true);
        REQUIRE_TRUE(s.body() == bottom);
    }
    s.ui.key(VK_PRIOR);
    require_native_observation([&] { return s.body() != bottom; }, "Page Up did not move notice text");
    s.ui.key(VK_HOME);
    s.require_endpoint(false);
    s.require_heading(L"== FlitzZip ==");
    for (const auto key : {VK_UP, VK_PRIOR, VK_HOME}) {
        s.ui.key(key);
        REQUIRE_TRUE(s.body() == initial);
    }
    s.ui.key(VK_DOWN);
    require_native_observation([&] { return s.body() != initial; }, "Down did not move notice text");
    s.ui.finish();
}

// Purpose: Isolate generated notice groups and reset scrolling only on a real tab change.
// Inputs: Native keyboard tab selection at both clamped ends. Outputs: Literal group headings and retained same-tab
// end.
TEST_CASE(main_window_licenses_keyboard_tabs_isolate_and_clamp) {
    LicenseSession s;
    s.ui.key(VK_LEFT);
    s.require_heading(L"== FlitzZip ==");
    s.ui.key(VK_END);
    s.require_endpoint(true);
    s.ui.key(VK_RIGHT);
    s.require_heading(L"== miniz 3.1.2 ==");
    s.ui.key(VK_END);
    s.require_endpoint(true);
    const auto last = s.body();
    s.ui.key(VK_RIGHT);
    REQUIRE_TRUE(s.body() == last);
    s.ui.key(VK_LEFT);
    s.require_heading(L"== FlitzZip ==");
    s.ui.finish();
}

// Purpose: Verify pointer tab selection has the same isolation and reset behavior as keyboard navigation.
// Inputs: Real tab hit regions and scrolled text. Outputs: Independent headings and same-tab position preservation.
TEST_CASE(main_window_licenses_pointer_tabs_reset_only_on_change) {
    LicenseSession s;
    s.tab(1);
    s.require_heading(L"== miniz 3.1.2 ==");
    s.ui.key(VK_END);
    s.require_endpoint(true);
    const auto last = s.body();
    s.tab(1);
    REQUIRE_TRUE(s.body() == last);
    s.tab(0);
    s.require_heading(L"== FlitzZip ==");
    s.ui.finish();
}

// Purpose: Prevent modal keyboard and pointer input from activating underlying navigation or unrelated work.
// Inputs: Outside-panel clicks, unassigned keys and Tab. Outputs: Modal/group retained and normal navigation after
// close.
TEST_CASE(main_window_licenses_modal_contains_unrelated_input) {
    LicenseSession s;
    const auto before = read_native_fixture(flitzzip::app::settings_file_path());
    click_native_navigation(s.ui.window, 6);
    click_native_control(s.ui.window, RECT{s.viewport.left, s.viewport.top, s.viewport.left + s.ui.px(30),
                                           s.viewport.top + s.ui.px(30)});
    for (const auto key : {VK_TAB, VK_F1, VK_DELETE, static_cast<int>('A')}) {
        s.ui.key(static_cast<WPARAM>(key));
    }
    s.require_heading(L"== FlitzZip ==");
    require_native_navigation_selection(s.ui.window, s.ui.frame.dc(), 7);
    REQUIRE_EQ(read_native_fixture(flitzzip::app::settings_file_path()), before);
    s.ui.key(VK_ESCAPE);
    s.require_closed();
    s.ui.finish();
}

// Purpose: Qualify every documented keyboard dismissal and reset both group and scrolling on a fresh opening.
// Inputs: Escape, Enter and Space after selecting/scolling Other. Outputs: Dismissal, ordinary reuse and first-tab
// reset.
TEST_CASE(main_window_licenses_close_keys_and_reopen_reset) {
    LicenseSession s;
    for (const auto key : {VK_ESCAPE, VK_RETURN, VK_SPACE}) {
        s.tab(1);
        s.ui.key(VK_END);
        s.require_endpoint(true);
        s.ui.key(key);
        s.require_closed();
        click_native_navigation(s.ui.window, 7);
        s.open();
    }
    s.ui.finish();
}

// Purpose: Commit the Close command only when its owned press is released inside the same button.
// Inputs: Cancelled pointer release followed by a complete click. Outputs: Modal retained on cancellation and reusable
// close.
TEST_CASE(main_window_licenses_close_button_requires_matching_release) {
    LicenseSession s;
    const auto button = s.close_button();
    const auto point = MAKELPARAM((button.left + button.right) / 2, (button.top + button.bottom) / 2);
    HiddenController::send(s.ui.window, WM_LBUTTONDOWN, MK_LBUTTON, point);
    HiddenController::send(s.ui.window, WM_LBUTTONUP, 0,
                           MAKELPARAM(s.panel.left + s.ui.px(12), s.panel.top + s.ui.px(12)));
    s.require_heading(L"== FlitzZip ==");
    click_native_control(s.ui.window, button);
    s.require_closed();
    s.ui.finish();
}

// Purpose: Keep full and high-resolution wheel input equivalent without leaking position across modal reopen.
// Inputs: One detent and two half-detents at the same owned client point. Outputs: Exact final text identity and reset.
TEST_CASE(main_window_licenses_wheel_fractional_input_and_reopen) {
    LicenseSession s;
    const POINT point{s.viewport.left + s.ui.px(40), s.viewport.top + s.ui.px(30)};
    const auto initial = s.body();
    s.ui.wheel(-WHEEL_DELTA, point);
    require_native_observation([&] { return s.body() != initial; }, "Wheel did not scroll notice text");
    const auto whole = s.settled_body();
    s.ui.key(VK_HOME);
    s.require_heading(L"== FlitzZip ==");
    s.ui.wheel(-WHEEL_DELTA / 2, point);
    s.ui.wheel(-WHEEL_DELTA / 2, point);
    require_native_observation([&] { return s.body() == whole; }, "Fractional wheel motion differs from one detent");
    s.ui.key(VK_ESCAPE);
    s.open();
    s.ui.finish();
}

// Purpose: Bound real scrollbar dragging and stop accepting drag motion after button release.
// Inputs: Native press/move/release beyond both endpoints. Outputs: End/Home text bounds and inert post-release
// movement.
TEST_CASE(main_window_licenses_scrollbar_drag_bounds_and_release) {
    LicenseSession s;
    const auto drag = [&](int from, int to) {
        const int x = s.viewport.right - s.ui.px(2);
        HiddenController::send(s.ui.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, from));
        HiddenController::send(s.ui.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, to));
        HiddenController::send(s.ui.window, WM_LBUTTONUP, 0, MAKELPARAM(x, to));
    };
    auto range = s.thumb();
    drag((range.first + range.second) / 2, s.viewport.bottom + s.ui.px(70));
    s.require_endpoint(true);
    const auto bottom = s.body();
    HiddenController::send(s.ui.window, WM_MOUSEMOVE, 0, MAKELPARAM(s.viewport.right - s.ui.px(2), s.viewport.top));
    REQUIRE_TRUE(s.body() == bottom);
    range = s.thumb();
    drag((range.first + range.second) / 2, s.viewport.top - s.ui.px(70));
    s.require_endpoint(false);
    s.require_heading(L"== FlitzZip ==");
    s.ui.finish();
}

// Purpose: Release modal drag ownership when Windows cancels capture, without moving the system pointer.
// Inputs: Begun real thumb drag, WM_CANCELMODE and a subsequent pointer move. Outputs: Unchanged top text and usable
// keys.
TEST_CASE(main_window_licenses_capture_cancellation_ends_drag) {
    LicenseSession s;
    const auto range = s.thumb();
    const int x = s.viewport.right - s.ui.px(2);
    HiddenController::send(s.ui.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, (range.first + range.second) / 2));
    HiddenController::send(s.ui.window, WM_CANCELMODE);
    HiddenController::send(s.ui.window, WM_MOUSEMOVE, 0, MAKELPARAM(x, s.viewport.bottom));
    s.require_heading(L"== FlitzZip ==");
    s.ui.key(VK_END);
    s.require_endpoint(true);
    s.ui.finish();
}
