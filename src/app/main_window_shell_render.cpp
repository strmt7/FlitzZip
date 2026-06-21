#include "app/main_window_impl.hpp"

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

// Purpose: Paint the current frame through an off-screen buffer.
// Inputs: None; uses the current window paint region.
// Outputs: Draws the full client frame and releases GDI objects before returning.
void MainWindow::paint() {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT rect{};
    GetClientRect(hwnd_, &rect);
    const int width = std::max(1L, rect.right - rect.left);
    const int height = std::max(1L, rect.bottom - rect.top);
    HDC buffer_dc = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    HGDIOBJ previous_bitmap = SelectObject(buffer_dc, bitmap);
    layout_and_draw(buffer_dc, rect);
    BitBlt(dc, 0, 0, width, height, buffer_dc, 0, 0, SRCCOPY);
    SelectObject(buffer_dc, previous_bitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer_dc);
    EndPaint(hwnd_, &ps);
}

// Purpose: Draw the current frame using DPI-scaled layout regions.
// Inputs: `dc` is the off-screen device context and `rect` is the client rectangle in physical pixels.
// Outputs: Writes the shell, navigation, active page, overlays, and status strip into `dc`.
void MainWindow::layout_and_draw(HDC dc, const RECT& rect) {
    UiState state;
    {
        std::lock_guard lock(mutex_);
        state = state_;
    }

    fill_rect(dc, rect, kBg);
    const int top_bar = scale(kTopBar);
    const int rail_width = scale(kRailWidth);
    const int status_bar = scale(kStatusBar);
    RECT top{rect.left, rect.top, rect.right, rect.top + top_bar};
    RECT rail{rect.left, top.bottom, rect.left + rail_width, rect.bottom - status_bar};
    RECT content{rail.right, top.bottom, rect.right, rect.bottom - status_bar};
    RECT status{rect.left, rect.bottom - status_bar, rect.right, rect.bottom};

    draw_top_bar(dc, top);
    draw_navigation(dc, rail, state);
    draw_content(dc, content, state);
    draw_tab_transition(dc, RECT{rect.left, content.top, rect.right, content.bottom});
    draw_active_dropdown(dc, content, state);
    draw_keyboard_focus(dc, content, state);
    draw_text_tooltip(dc);
    draw_status_bar(dc, status, state);
    draw_license_notices_dialog(dc, rect, state);
    draw_extract_overwrite_prompt(dc, rect, state);
}

// Purpose: Return whether the mouse is currently over a live clickable target.
// Inputs: `rect` is the clickable target and `enabled` must be false for static or disabled UI.
// Outputs: Returns true only for enabled controls under the mouse pointer.
bool MainWindow::interactive_hovered(const RECT& rect, bool enabled) const {
    return enabled && mouse_inside_client_ && contains_point(rect, mouse_position_.x, mouse_position_.y);
}

// Purpose: Compute the subtle clickable hover fill used by all interactive boxes.
// Inputs: `base` is the non-hover fill, `rect` is the clickable target, `enabled` gates interaction, and `accent`
// selects command-button treatment.
// Outputs: Returns a slightly lifted background color without changing borders or behavior.
COLORREF MainWindow::interactive_fill(COLORREF base, const RECT& rect, bool enabled, bool accent) const {
    if (!interactive_hovered(rect, enabled)) {
        return base;
    }
    return blend_color(base, accent ? RGB(255, 74, 84) : RGB(63, 82, 89), accent ? 0.26 : 0.34);
}

// Purpose: Draw the shared hover background for row-like controls.
// Inputs: `dc` is the target, `rect` is the clickable row, and `enabled` gates interaction.
// Outputs: Adds only a subtle background lift when the row is hovered.
void MainWindow::draw_interactive_hover_surface(HDC dc, const RECT& rect, bool enabled) {
    if (!interactive_hovered(rect, enabled)) {
        return;
    }
    fill_round_rect(dc, rect, interactive_fill(kPanel, rect, enabled), scale(3));
}

// Purpose: Draw the delayed text tooltip when an eligible ellipsized value is hovered.
// Inputs: `dc` is the target for the current frame.
// Outputs: Renders a compact tooltip near the source cell without affecting layout.
void MainWindow::draw_text_tooltip(HDC dc) {
    if (!text_tooltip_visible_ || text_tooltip_text_.empty()) {
        return;
    }
    const RECT content = content_rect();
    const int max_width = scale(620);
    RECT measured{0, 0, max_width - scale(20), 0};
    SelectObject(dc, tiny_font_);
    DrawTextW(dc, text_tooltip_text_.data(), static_cast<int>(text_tooltip_text_.size()), &measured,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    const int tooltip_width =
        std::clamp<int>(static_cast<int>(measured.right - measured.left) + scale(20), scale(160), max_width);
    const int tooltip_height =
        std::clamp<int>(static_cast<int>(measured.bottom - measured.top) + scale(16), scale(34), scale(132));
    int left = std::clamp<int>(static_cast<int>(text_tooltip_cell_.left), static_cast<int>(content.left) + scale(8),
                               static_cast<int>(content.right) - tooltip_width - scale(8));
    int top = text_tooltip_cell_.bottom + scale(6);
    if (top + tooltip_height > content.bottom - scale(8)) {
        top = text_tooltip_cell_.top - tooltip_height - scale(6);
    }
    top = std::clamp<int>(top, static_cast<int>(content.top) + scale(8),
                          static_cast<int>(content.bottom) - tooltip_height - scale(8));
    const RECT tooltip{left, top, left + tooltip_width, top + tooltip_height};
    fill_round_rect(dc, tooltip, RGB(33, 45, 50), scale(4));
    stroke_rect(dc, tooltip, RGB(80, 99, 106));
    draw_text(dc, inset_rect(tooltip, scale(10), scale(7)), text_tooltip_text_, kText,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
}

// Purpose: Return the centered extraction overwrite modal panel.
// Inputs: `rect` is the full client area.
// Outputs: Returns a DPI-scaled panel rectangle using the main-window visual system.
RECT MainWindow::extract_overwrite_prompt_rect(const RECT& rect) const {
    const int width = scale(620);
    const int height = scale(258);
    const int left = rect.left + ((rect.right - rect.left) - width) / 2;
    const int top = rect.top + ((rect.bottom - rect.top) - height) / 2;
    return RECT{left, top, left + width, top + height};
}

// Purpose: Return modal action button rectangles in Continue/Cancel order.
// Inputs: `modal` is the overwrite prompt panel rectangle.
// Outputs: Returns two right-aligned button rectangles.
std::array<RECT, 2> MainWindow::extract_overwrite_prompt_buttons(const RECT& modal) const {
    const int button_height = scale(36);
    const int gap = scale(12);
    const int cancel_width = scale(110);
    const int continue_width = scale(142);
    const int bottom = modal.bottom - scale(22);
    const RECT cancel{modal.right - scale(22) - cancel_width, bottom - button_height, modal.right - scale(22), bottom};
    const RECT allow{cancel.left - gap - continue_width, cancel.top, cancel.left - gap, cancel.bottom};
    return {allow, cancel};
}

// Purpose: Draw the SuperZip-owned extraction overwrite confirmation modal.
// Inputs: `dc` is the target, `rect` is the full client area, and `state` contains modal text.
// Outputs: Renders no pixels unless the overwrite confirmation is active.
void MainWindow::draw_extract_overwrite_prompt(HDC dc, const RECT& rect, const UiState& state) {
    if (!state.extract_overwrite_prompt_visible) {
        return;
    }
    const RECT workspace{content_rect().left, content_rect().top, rect.right, content_rect().bottom};
    fill_rect(dc, workspace, RGB(7, 11, 13));
    fill_rect(dc, RECT{rect.left, workspace.top, rect.right, workspace.top + scale(2)}, kAccent);
    const RECT modal = extract_overwrite_prompt_rect(workspace);
    fill_round_rect(dc, modal, kPanel, scale(6));
    stroke_rect(dc, modal, RGB(72, 93, 101));

    SelectObject(dc, small_font_);
    draw_text(dc, RECT{modal.left + scale(24), modal.top + scale(18), modal.right - scale(24), modal.top + scale(54)},
              L"Confirm overwrite", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(dc, tiny_font_);
    const RECT message{modal.left + scale(24), modal.top + scale(68), modal.right - scale(24), modal.top + scale(122)};
    draw_text(dc, message,
              L"The extraction destination already contains items, or SuperZip cannot prove it is empty. Continue only "
              L"if archive entries may replace existing files when their paths conflict.",
              kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);

    const RECT destination{modal.left + scale(24), modal.top + scale(132), modal.right - scale(24),
                           modal.top + scale(186)};
    draw_field(dc, destination, L"Destination", state.extract_overwrite_prompt_destination, false, true);

    const auto buttons = extract_overwrite_prompt_buttons(modal);
    draw_button(dc, buttons[0], L"Allow overwrite", true);
    draw_button(dc, buttons[1], L"Cancel", false);

    const int focus_index = std::clamp(modal_focus_index_, 0, 1);
    RECT focus = inset_rect(buttons[static_cast<std::size_t>(focus_index)], -scale(3), -scale(3));
    HPEN pen = CreatePen(PS_SOLID, std::max(1, scale(1)), RGB(99, 130, 140));
    HGDIOBJ previous_pen = SelectObject(dc, pen);
    HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, focus.left, focus.top, focus.right, focus.bottom, scale(5), scale(5));
    SelectObject(dc, previous_brush);
    SelectObject(dc, previous_pen);
    DeleteObject(pen);
}

// Purpose: Draw the keyboard focus affordance for the current target.
// Inputs: `dc` is the target, `content` is the content area, and `state` is the copied UI state.
// Outputs: Renders a minimal non-hover focus indicator without mutating state.
void MainWindow::draw_keyboard_focus(HDC dc, const RECT& content, const UiState& state) {
    if (state.extract_overwrite_prompt_visible || state.license_notices_dialog_visible) {
        return;
    }
    const auto targets = focus_targets_for(content, state);
    if (targets.empty()) {
        return;
    }
    const int index = (keyboard_focus_index_ % static_cast<int>(targets.size()) + static_cast<int>(targets.size())) %
                      static_cast<int>(targets.size());
    if (targets[static_cast<std::size_t>(index)].kind == FocusTargetKind::Navigation) {
        return;
    }
    RECT focus = targets[static_cast<std::size_t>(index)].rect;
    focus = inset_rect(focus, scale(2), scale(2));
    HPEN pen = CreatePen(PS_SOLID, std::max(1, scale(1)), RGB(99, 130, 140));
    HGDIOBJ previous_pen = SelectObject(dc, pen);
    HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, focus.left, focus.top, focus.right, focus.bottom, scale(4), scale(4));
    SelectObject(dc, previous_brush);
    SelectObject(dc, previous_pen);
    DeleteObject(pen);
}

// Purpose: Draw the persistent product shell strip.
// Inputs: `dc` is the target and `rect` is the full client rectangle.
// Outputs: Renders the brand chrome; page-specific actions stay inside their pages.
void MainWindow::draw_top_bar(HDC dc, const RECT& rect) {
    fill_rect(dc, rect, kShell);
    draw_line(dc, rect.left, rect.bottom - 1, rect.right, rect.bottom - 1, kBorder);

    const int rail_width = scale(kRailWidth);
    RECT logo{scale(18), scale(14), scale(38), scale(36)};
    draw_logo(dc, logo, kAccent);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{scale(48), rect.top, rail_width + scale(76), rect.bottom}, L"SuperZip", kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// Purpose: Draw the compact navigation rail.
// Inputs: `dc` is the target, `rect` is the rail rectangle, and `state` is the copied UI state.
// Outputs: Renders all primary pages with active-page highlighting.
void MainWindow::draw_navigation(HDC dc, const RECT& rect, const UiState& state) {
    fill_rect(dc, rect, kRail);
    draw_line(dc, rect.right - 1, rect.top, rect.right - 1, rect.bottom, kBorder);
    const std::array<Page, 8> pages{
        Page::Queue,   Page::Compress, Page::Extract,  Page::Security,
        Page::History, Page::Gpu,      Page::Settings, Page::About,
    };
    SelectObject(dc, tiny_font_);
    const int item_height = scale(63);
    int y = rect.top + scale(10);
    for (const auto page : pages) {
        RECT item{rect.left, y, rect.right, y + item_height};
        const bool active = state.page == page;
        const bool locked = operation_running(state) && !active;
        const bool hovered =
            !locked && mouse_inside_client_ && contains_point(item, mouse_position_.x, mouse_position_.y);
        const bool pressed = hovered && primary_mouse_down_;
        const RECT surface{item.left + scale(8), item.top + scale(5), item.right - scale(8), item.bottom - scale(5)};
        if (active) {
            fill_rect(dc, RECT{item.left, item.top, item.left + scale(5), item.bottom}, kAccent);
            fill_round_rect(dc, surface,
                            pressed   ? RGB(103, 20, 28)
                            : hovered ? RGB(148, 30, 39)
                                      : RGB(126, 24, 31),
                            scale(4));
        } else if (pressed) {
            fill_round_rect(dc, surface, RGB(31, 47, 53), scale(4));
            stroke_rect(dc, surface, RGB(72, 95, 103));
        } else if (hovered) {
            fill_round_rect(dc, surface, kPanel2, scale(4));
            stroke_rect(dc, surface, RGB(70, 91, 99));
        }
        const int icon_size = scale(30);
        const int icon_left = item.left + ((item.right - item.left) - icon_size) / 2;
        const int icon_top = item.top + ((item.bottom - item.top) - icon_size) / 2;
        RECT icon{icon_left, icon_top, icon_left + icon_size, icon_top + icon_size};
        if (pressed) {
            OffsetRect(&icon, scale(1), scale(1));
        }
        const COLORREF nav_color = active ? kText : locked ? RGB(73, 88, 93) : hovered ? RGB(198, 211, 215) : kMuted;
        draw_nav_icon(dc, page, icon, nav_color);
        y += item_height;
    }
}

// Purpose: Draw the persistent AMD GPU, operation progress, and local-clock status strip.
// Inputs: `dc` is the target, `rect` is the status-strip rectangle, and `state` is the copied UI state.
// Outputs: Renders backend status, stable progress columns when active, and the user's locale-formatted time.
void MainWindow::draw_status_bar(HDC dc, const RECT& rect, const UiState& state) {
    fill_rect(dc, rect, kShell);
    draw_line(dc, rect.left, rect.top, rect.right, rect.top, kBorder);
    const bool ready = gpu_ready(state);
    const int cy = (rect.top + rect.bottom) / 2;
    HBRUSH dot = CreateSolidBrush(ready ? kOk : kDanger);
    HGDIOBJ previous = SelectObject(dc, dot);
    Ellipse(dc, rect.left + scale(18), cy - scale(5), rect.left + scale(28), cy + scale(5));
    SelectObject(dc, previous);
    DeleteObject(dot);

    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{rect.left + scale(36), rect.top, rect.left + scale(202), rect.bottom},
              ready ? L"AMD GPU ready" : L"AMD GPU unavailable", ready ? kOk : kDanger,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_line(dc, rect.left + scale(220), rect.top + scale(8), rect.left + scale(220), rect.bottom - scale(8), kBorder);
    draw_line(dc, rect.left + scale(450), rect.top + scale(8), rect.left + scale(450), rect.bottom - scale(8), kBorder);

    if (progress_visible(state)) {
        const RECT progress_rect{rect.left + scale(468), rect.top, rect.left + scale(610), rect.bottom};
        const RECT throughput_rect{rect.left + scale(632), rect.top, rect.left + scale(824), rect.bottom};
        const RECT remaining_rect{rect.left + scale(846), rect.top, rect.right - scale(kClockSegmentWidth + 20),
                                  rect.bottom};
        draw_text(dc, progress_rect, L"Progress: " + progress_percent_text(state.progress), kMuted,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        draw_text(dc, throughput_rect, L"Throughput: " + rate_text(state.progress.throughput_bytes_per_second), kMuted,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        draw_text(dc, remaining_rect, L"Time remaining: " + progress_time_remaining_text(state), kMuted,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    draw_line(dc, rect.right - scale(kClockSegmentWidth), rect.top + scale(8), rect.right - scale(kClockSegmentWidth),
              rect.bottom - scale(8), kBorder);
    draw_text(dc, RECT{rect.right - scale(kClockSegmentWidth), rect.top, rect.right, rect.bottom},
              current_user_time_text(), kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// Purpose: Dispatch the active page renderer into the content region.
// Inputs: `dc` is the paint target, `rect` is the content bounds, and `state` is the copied UI state.
// Outputs: Draws exactly one active page without mutating UI state.
void MainWindow::draw_content(HDC dc, const RECT& rect, const UiState& state) {
    fill_rect(dc, rect, kBg);
    switch (state.page) {
    case Page::Queue:
        draw_queue_page(dc, rect, state);
        break;
    case Page::Compress:
        draw_compress_page(dc, rect, state);
        break;
    case Page::Extract:
        draw_extract_page(dc, rect, state);
        break;
    case Page::Security:
        draw_security_page(dc, rect, state);
        break;
    case Page::History:
        draw_history_page(dc, rect, state);
        break;
    case Page::Gpu:
        draw_gpu_page(dc, rect, state);
        break;
    case Page::Settings:
        draw_settings_page(dc, rect, state);
        break;
    case Page::About:
        draw_about_page(dc, rect);
        break;
    }
}

// Purpose: Compute Queue page rectangles shared by rendering and hit testing.
// Inputs: `rect` is the content area in physical pixels.
// Outputs: Returns DPI-scaled Queue page control rectangles.

}  // namespace superzip::app
