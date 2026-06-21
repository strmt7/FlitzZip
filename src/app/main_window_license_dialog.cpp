#include "app/main_window_impl.hpp"

#include "superzip_license_notices.hpp"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef MSGFLT_ALLOW
#define MSGFLT_ALLOW 1
#endif

namespace superzip::app {

namespace {

constexpr std::array<std::string_view, 2> kLicenseNoticeGroups{"SuperZip", "Other"};
constexpr std::array<std::wstring_view, 2> kLicenseNoticeTabLabels{L"SuperZip", L"Other"};

}  // namespace

// Purpose: Return the About page Licenses button rectangle.
// Inputs: `area` is the DPI-scaled page content area.
// Outputs: Returns a compact command rectangle inside the About card.
RECT MainWindow::about_licenses_button_rect(const RECT& area) const {
    const RECT card{area.left, area.top + scale(54), area.right, area.bottom - scale(60)};
    const int button_width = scale(112);
    const int button_height = scale(36);
    return RECT{card.right - scale(42) - button_width, card.bottom - scale(58), card.right - scale(42),
                card.bottom - scale(58) + button_height};
}

// Purpose: Return the centered license-notice modal panel.
// Inputs: `rect` is the full client area.
// Outputs: Returns a DPI-scaled panel rectangle using the main-window visual system.
RECT MainWindow::license_notices_dialog_rect(const RECT& rect) const {
    const int client_width = static_cast<int>(rect.right - rect.left);
    const int client_height = static_cast<int>(rect.bottom - rect.top);
    const int width = std::min(scale(800), std::max(scale(620), client_width - scale(110)));
    const int height = std::min(scale(540), std::max(scale(410), client_height - scale(90)));
    const int left = rect.left + (client_width - width) / 2;
    const int top = rect.top + (client_height - height) / 2;
    return RECT{left, top, left + width, top + height};
}

// Purpose: Return the scrollable text viewport inside the license-notice modal.
// Inputs: `modal` is the license-notice panel rectangle.
// Outputs: Returns the clipped text area used for drawing and scroll math.
RECT MainWindow::license_notices_viewport_rect(const RECT& modal) const {
    return RECT{modal.left + scale(34), modal.top + scale(112), modal.right - scale(38), modal.bottom - scale(78)};
}

// Purpose: Return the Close button rectangle for the license-notice modal.
// Inputs: `modal` is the license-notice panel rectangle.
// Outputs: Returns a right-aligned command rectangle.
RECT MainWindow::license_notices_close_button_rect(const RECT& modal) const {
    const int button_width = scale(110);
    const int button_height = scale(36);
    return RECT{modal.right - scale(24) - button_width, modal.bottom - scale(22) - button_height,
                modal.right - scale(24), modal.bottom - scale(22)};
}

// Purpose: Return SuperZip/Other tab rectangles for the license-notice modal.
// Inputs: `modal` is the license-notice panel rectangle.
// Outputs: Returns tab rectangles in SuperZip, Other order.
std::array<RECT, 2> MainWindow::license_notices_tab_rects(const RECT& modal) const {
    const int top = modal.top + scale(62);
    const int height = scale(34);
    const int width = scale(128);
    const int gap = scale(8);
    const int left = modal.left + scale(24);
    return {RECT{left, top, left + width, top + height},
            RECT{left + width + gap, top, left + (width * 2) + gap, top + height}};
}

// Purpose: Build the generated license-notice text for the active modal tab.
// Inputs: None; reads the generated notice table and current tab index.
// Outputs: Returns cached UTF-16 notice body generated from source-controlled licenses.
const std::wstring& MainWindow::license_notices_text() {
    const int tab_index = std::clamp(license_notices_tab_index_, 0, 1);
    auto& cached = cached_license_notices_text_[static_cast<std::size_t>(tab_index)];
    if (!cached.empty()) {
        return cached;
    }
    std::string combined;
    const std::string_view active_group = kLicenseNoticeGroups[static_cast<std::size_t>(tab_index)];
    for (const auto& notice : kLicenseNotices) {
        if (notice.group != active_group) {
            continue;
        }
        if (!combined.empty()) {
            combined += "\n\n";
        }
        combined += "== ";
        combined.append(notice.title.data(), notice.title.size());
        combined += " ==\n\n";
        combined.append(notice.text.data(), notice.text.size());
    }
    cached = widen(combined);
    return cached;
}

// Purpose: Change the active license-notice tab.
// Inputs: `tab_index` is zero for SuperZip and one for Other.
// Outputs: Updates tab and scroll state, then queues repaint when the tab changes.
void MainWindow::select_license_notices_tab(int tab_index) {
    const int next = std::clamp(tab_index, 0, 1);
    if (license_notices_tab_index_ == next) {
        return;
    }
    license_notices_tab_index_ = next;
    license_notices_scroll_pixels_ = 0;
    request_repaint();
}

// Purpose: Measure the rendered license-notice body height.
// Inputs: `dc` is the current target and `viewport` is the text viewport.
// Outputs: Returns the DPI-scaled text height needed for scroll bounds.
int MainWindow::license_notices_text_height(HDC dc, const RECT& viewport) const {
    const std::wstring& text = const_cast<MainWindow*>(this)->license_notices_text();
    const int viewport_width = static_cast<int>(viewport.right - viewport.left);
    RECT measure{0, 0, std::max(1, viewport_width - scale(8)), 0};
    HGDIOBJ previous_font = SelectObject(dc, tiny_font_);
    DrawTextW(dc, text.data(), static_cast<int>(std::min<std::size_t>(text.size(), INT_MAX)), &measure,
              DT_CALCRECT | DT_WORDBREAK | DT_EXPANDTABS | DT_NOPREFIX);
    SelectObject(dc, previous_font);
    return std::max(scale(1), static_cast<int>(measure.bottom - measure.top));
}

// Purpose: Apply a bounded scroll delta to the license-notice modal.
// Inputs: `delta_pixels` is positive for down and negative for up.
// Outputs: Updates scroll offset and queues a repaint when the dialog is visible.
bool MainWindow::scroll_license_notices_dialog(int delta_pixels) {
    bool visible = false;
    {
        std::lock_guard lock(mutex_);
        visible = state_.license_notices_dialog_visible;
    }
    if (!visible) {
        return false;
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    const RECT workspace{content_rect().left, content_rect().top, client.right, content_rect().bottom};
    const RECT viewport = license_notices_viewport_rect(license_notices_dialog_rect(workspace));
    HDC dc = GetDC(hwnd_);
    if (dc == nullptr) {
        return true;
    }
    const int content_height = license_notices_text_height(dc, viewport);
    ReleaseDC(hwnd_, dc);
    const int viewport_height = static_cast<int>(viewport.bottom - viewport.top);
    const int max_scroll = std::max(0, content_height - viewport_height);
    const int next = std::clamp(license_notices_scroll_pixels_ + delta_pixels, 0, max_scroll);
    if (next != license_notices_scroll_pixels_) {
        license_notices_scroll_pixels_ = next;
        request_repaint();
    }
    return true;
}

// Purpose: Return the product-styled license-notice scrollbar track.
// Inputs: `viewport` is the clipped license text viewport.
// Outputs: Returns the slim vertical track inside the modal text frame.
RECT MainWindow::license_notices_scrollbar_track_rect(const RECT& viewport) const {
    return RECT{viewport.right - scale(5), viewport.top, viewport.right, viewport.bottom};
}

// Purpose: Return the license-notice scrollbar thumb for the current scroll offset.
// Inputs: `viewport` is the clipped license text viewport and `content_height` is the measured notice height.
// Outputs: Returns an empty rectangle when no scrolling is needed.
RECT MainWindow::license_notices_scrollbar_thumb_rect(const RECT& viewport, int content_height) const {
    const int viewport_height = std::max(1, static_cast<int>(viewport.bottom - viewport.top));
    const int max_scroll = std::max(0, content_height - viewport_height);
    if (max_scroll <= 0) {
        return RECT{};
    }
    const RECT track = license_notices_scrollbar_track_rect(viewport);
    const int track_height = std::max(1, static_cast<int>(track.bottom - track.top));
    const int thumb_height = std::clamp((viewport_height * track_height) / std::max(viewport_height, content_height),
                                        scale(28), track_height);
    const int travel = std::max(0, track_height - thumb_height);
    const int top = track.top + (std::clamp(license_notices_scroll_pixels_, 0, max_scroll) * travel / max_scroll);
    return RECT{track.left, top, track.right, top + thumb_height};
}

// Purpose: Begin dragging the license-notice scrollbar thumb.
// Inputs: `y` is the mouse position; `viewport` and `content_height` define the active scroll geometry.
// Outputs: Captures the scroll baseline when the notice body overflows.
void MainWindow::begin_license_notices_scroll_drag(int y, const RECT& viewport, int content_height) {
    const RECT thumb = license_notices_scrollbar_thumb_rect(viewport, content_height);
    if (thumb.right <= thumb.left) {
        return;
    }
    license_notices_scroll_dragging_ = true;
    license_notices_scroll_drag_start_y_ = y;
    license_notices_scroll_drag_start_offset_ = license_notices_scroll_pixels_;
    license_notices_scroll_drag_content_height_ = content_height;
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

// Purpose: Update license-notice scrolling from an active thumb drag.
// Inputs: `y` is the current client mouse y-coordinate.
// Outputs: Moves the bounded scroll offset and queues repaint when changed.
void MainWindow::update_license_notices_scroll_drag(int y) {
    if (!license_notices_scroll_dragging_) {
        return;
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    const RECT workspace{content_rect().left, content_rect().top, client.right, content_rect().bottom};
    const RECT viewport = license_notices_viewport_rect(license_notices_dialog_rect(workspace));
    const int viewport_height = std::max(1, static_cast<int>(viewport.bottom - viewport.top));
    const int max_scroll = std::max(0, license_notices_scroll_drag_content_height_ - viewport_height);
    if (max_scroll <= 0) {
        license_notices_scroll_pixels_ = 0;
        return;
    }
    const RECT track = license_notices_scrollbar_track_rect(viewport);
    const RECT thumb = license_notices_scrollbar_thumb_rect(viewport, license_notices_scroll_drag_content_height_);
    const int travel = std::max(1, static_cast<int>((track.bottom - track.top) - (thumb.bottom - thumb.top)));
    const int delta = static_cast<int>(std::lround(static_cast<double>(y - license_notices_scroll_drag_start_y_) *
                                                   static_cast<double>(max_scroll) / static_cast<double>(travel)));
    const int previous = license_notices_scroll_pixels_;
    license_notices_scroll_pixels_ = std::clamp(license_notices_scroll_drag_start_offset_ + delta, 0, max_scroll);
    if (license_notices_scroll_pixels_ != previous) {
        request_repaint();
    }
}

// Purpose: End any active license-notice scrollbar drag.
// Inputs: None.
// Outputs: Clears the modal scrollbar drag state.
void MainWindow::end_license_notices_scroll_drag() {
    license_notices_scroll_dragging_ = false;
}

// Purpose: Close the license-notice modal.
// Inputs: None.
// Outputs: Clears modal state, resets scroll state, and queues repaint.
void MainWindow::close_license_notices_dialog() {
    {
        std::lock_guard lock(mutex_);
        state_.license_notices_dialog_visible = false;
    }
    license_notices_scroll_pixels_ = 0;
    request_repaint();
}

// Purpose: Open the generated license-notice modal from the About page.
// Inputs: None.
// Outputs: Shows the modal, resets scroll state, and queues repaint.
void MainWindow::show_license_notices_dialog() {
    close_active_dropdown();
    license_notices_tab_index_ = 0;
    (void)license_notices_text();
    {
        std::lock_guard lock(mutex_);
        state_.license_notices_dialog_visible = true;
        state_.status = "License notices";
    }
    license_notices_scroll_pixels_ = 0;
    request_repaint();
}

// Purpose: Draw the SuperZip-owned generated license-notice modal.
// Inputs: `dc` is the target, `rect` is the full client area, and `state` contains modal visibility.
// Outputs: Renders no pixels unless the license-notice dialog is active.
void MainWindow::draw_license_notices_dialog(HDC dc, const RECT& rect, const UiState& state) {
    if (!state.license_notices_dialog_visible) {
        return;
    }
    const RECT workspace{content_rect().left, content_rect().top, rect.right, content_rect().bottom};
    fill_rect(dc, workspace, RGB(7, 11, 13));
    const RECT modal = license_notices_dialog_rect(workspace);
    fill_round_rect(dc, modal, kPanel, scale(6));
    stroke_rect(dc, modal, RGB(72, 93, 101));
    fill_rect(dc, RECT{modal.left, modal.top, modal.right, modal.top + scale(3)}, kAccent);

    SelectObject(dc, small_font_);
    draw_text(dc, RECT{modal.left + scale(24), modal.top + scale(18), modal.right - scale(24), modal.top + scale(54)},
              L"Licenses", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, tiny_font_);
    const auto tabs = license_notices_tab_rects(modal);
    for (int index = 0; index < static_cast<int>(tabs.size()); ++index) {
        const RECT tab = tabs[static_cast<std::size_t>(index)];
        const bool active = index == license_notices_tab_index_;
        const COLORREF fill = interactive_fill(active ? kAccent : kPanel2, tab, true, active);
        fill_round_rect(dc, tab, fill, scale(4));
        stroke_rect(dc, tab, active ? kAccent2 : kBorder);
        draw_text(dc, inset_rect(tab, scale(12), 0), kLicenseNoticeTabLabels[static_cast<std::size_t>(index)], kText,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    const RECT viewport = license_notices_viewport_rect(modal);
    const int content_height = license_notices_text_height(dc, viewport);
    const int viewport_height = static_cast<int>(viewport.bottom - viewport.top);
    const int max_scroll = std::max(0, content_height - viewport_height);
    license_notices_scroll_pixels_ = std::clamp(license_notices_scroll_pixels_, 0, max_scroll);

    fill_round_rect(dc,
                    RECT{viewport.left - scale(10), viewport.top - scale(8), viewport.right + scale(10),
                         viewport.bottom + scale(8)},
                    kPanel2, scale(4));
    stroke_rect(dc,
                RECT{viewport.left - scale(10), viewport.top - scale(8), viewport.right + scale(10),
                     viewport.bottom + scale(8)},
                kBorder);
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, viewport.left, viewport.top, viewport.right, viewport.bottom);
    SelectObject(dc, tiny_font_);
    const RECT text_rect{viewport.left, viewport.top - license_notices_scroll_pixels_, viewport.right - scale(10),
                         viewport.top - license_notices_scroll_pixels_ + content_height + scale(8)};
    draw_text(dc, text_rect, license_notices_text(), kText,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EXPANDTABS | DT_NOPREFIX);
    RestoreDC(dc, saved);

    if (max_scroll > 0) {
        const RECT track = license_notices_scrollbar_track_rect(viewport);
        fill_round_rect(dc, track, RGB(18, 28, 32), scale(4));
        const RECT thumb = license_notices_scrollbar_thumb_rect(viewport, content_height);
        fill_round_rect(dc, thumb, interactive_fill(RGB(64, 83, 90), thumb), scale(4));
    }

    const RECT close = license_notices_close_button_rect(modal);
    draw_button(dc, close, L"Close", true);
    RECT focus = inset_rect(close, -scale(3), -scale(3));
    HPEN pen = CreatePen(PS_SOLID, std::max(1, scale(1)), RGB(99, 130, 140));
    HGDIOBJ previous_pen = SelectObject(dc, pen);
    HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, focus.left, focus.top, focus.right, focus.bottom, scale(5), scale(5));
    SelectObject(dc, previous_brush);
    SelectObject(dc, previous_pen);
    DeleteObject(pen);
}

// Purpose: Handle mouse activation while the license-notice modal is active.
// Inputs: `x` and `y` are client coordinates.
// Outputs: Consumes every click and closes only when the Close button is hit.
bool MainWindow::handle_license_notices_dialog_click(int x, int y) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const RECT workspace{content_rect().left, content_rect().top, client.right, content_rect().bottom};
    const RECT modal = license_notices_dialog_rect(workspace);
    if (contains_point(license_notices_close_button_rect(modal), x, y)) {
        close_license_notices_dialog();
        return true;
    }
    const auto tabs = license_notices_tab_rects(modal);
    for (int index = 0; index < static_cast<int>(tabs.size()); ++index) {
        if (contains_point(tabs[static_cast<std::size_t>(index)], x, y)) {
            select_license_notices_tab(index);
            return true;
        }
    }
    const RECT viewport = license_notices_viewport_rect(modal);
    HDC dc = GetDC(hwnd_);
    const int content_height = dc == nullptr ? 0 : license_notices_text_height(dc, viewport);
    if (dc != nullptr) {
        ReleaseDC(hwnd_, dc);
    }
    const RECT track = license_notices_scrollbar_track_rect(viewport);
    const RECT thumb = license_notices_scrollbar_thumb_rect(viewport, content_height);
    if (contains_point(thumb, x, y)) {
        begin_license_notices_scroll_drag(y, viewport, content_height);
        return true;
    }
    if (contains_point(track, x, y) && thumb.right > thumb.left) {
        (void)scroll_license_notices_dialog(y < thumb.top ? -static_cast<int>(viewport.bottom - viewport.top)
                                                          : static_cast<int>(viewport.bottom - viewport.top));
        return true;
    }
    if (contains_point(viewport, x, y)) {
        return true;
    }
    return true;
}

// Purpose: Handle keyboard activation and scrolling while the license-notice modal is active.
// Inputs: `key` is the pressed virtual key.
// Outputs: Consumes close and scroll keys without leaking focus to the underlying page.
bool MainWindow::handle_license_notices_dialog_key(WPARAM key) {
    if (key == VK_ESCAPE || key == VK_RETURN || key == VK_SPACE) {
        close_license_notices_dialog();
        return true;
    }
    if (key == VK_LEFT || key == VK_RIGHT) {
        select_license_notices_tab(license_notices_tab_index_ + (key == VK_RIGHT ? 1 : -1));
        return true;
    }
    if (key == VK_UP) {
        return scroll_license_notices_dialog(-scale(36));
    }
    if (key == VK_DOWN) {
        return scroll_license_notices_dialog(scale(36));
    }
    if (key == VK_PRIOR) {
        return scroll_license_notices_dialog(-scale(260));
    }
    if (key == VK_NEXT) {
        return scroll_license_notices_dialog(scale(260));
    }
    if (key == VK_HOME) {
        license_notices_scroll_pixels_ = 0;
        request_repaint();
        return true;
    }
    if (key == VK_END) {
        return scroll_license_notices_dialog(INT_MAX / 4);
    }
    return true;
}

// Purpose: Handle About page hit-testing and commands.
// Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
// Outputs: Returns true when the Licenses command consumed the click.
bool MainWindow::handle_about_click(const RECT& content, int x, int y) {
    const RECT area = inset_rect(content, scale(kPageInsetX), scale(kPageInsetY));
    if (contains_point(about_licenses_button_rect(area), x, y)) {
        show_license_notices_dialog();
        return true;
    }
    return false;
}

}  // namespace superzip::app
