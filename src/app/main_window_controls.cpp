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

// Purpose: Draw a standard SuperZip command button.
// Inputs: `dc` is the target, `rect` is the button bounds, `text` is the caption, and `active` selects accent styling.
// Outputs: Renders the button without mutating state.
void MainWindow::draw_button(HDC dc, const RECT& rect, const wchar_t* text, bool active) {
    const bool hovered = mouse_inside_client_ && contains_point(rect, mouse_position_.x, mouse_position_.y);
    const bool pressed = hovered && primary_mouse_down_;
    const double release_progress = button_release_progress(rect);
    const COLORREF base_fill = active ? kAccent : kPanel2;
    const COLORREF pressed_fill = active ? RGB(144, 22, 31) : RGB(38, 54, 60);
    const COLORREF fill = pressed ? pressed_fill : interactive_fill(base_fill, rect, true, active);
    const COLORREF border = active ? (pressed ? RGB(111, 18, 26) : kAccent2) : kBorder;
    fill_round_rect(dc, rect, fill, scale(4));
    stroke_rect(dc, rect, border);
    if (release_progress < 1.0) {
        const double eased = ease_out(release_progress);
        const int inset = static_cast<int>(std::round(static_cast<double>(scale(7)) * eased));
        const COLORREF pulse = blend_color(active ? RGB(255, 123, 131) : RGB(126, 151, 159), fill, eased);
        stroke_rect(dc, inset_rect(rect, std::max(1, inset), std::max(1, inset)), pulse);
    }
    SelectObject(dc, tiny_font_);
    RECT label = inset_rect(rect, scale(12), 0);
    if (pressed) {
        OffsetRect(&label, scale(1), scale(1));
    }
    draw_text(dc, label, text, kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// Purpose: Draw a slim progress indicator for the matching active operation.
// Inputs: `dc` is the target, `rect` is the fixed bar slot, `state` is copied UI state, and `operation` selects a tab.
// Outputs: Renders no pixels unless the active progress snapshot matches the requested operation.
void MainWindow::draw_operation_progress_bar(HDC dc, const RECT& rect, const UiState& state, OperationKind operation) {
    if (state.progress.operation != operation || !progress_bar_active(state)) {
        return;
    }
    const double ratio = std::clamp(progress_ratio(state.progress), 0.02, 1.0);
    fill_round_rect(dc, rect, RGB(34, 50, 56), scale(4));
    RECT fill = rect;
    fill.right = fill.left + static_cast<int>(std::round(static_cast<double>(fill.right - fill.left) * ratio));
    if (fill.right > fill.left) {
        fill_round_rect(dc, fill, operation == OperationKind::Verify ? kInfo : kAccent, scale(4));
    }
}

// Purpose: Draw a DPI-scaled opt-in settings toggle row.
// Inputs: `dc` is the target, `rect` is the row rectangle, `text` is display text, and `enabled` selects checked
// styling. Outputs: Renders the toggle and label into `dc`.
void MainWindow::draw_toggle(HDC dc, const RECT& rect, const wchar_t* text, bool enabled, ToggleId id) {
    draw_interactive_hover_surface(dc, rect, true);
    draw_text(dc, RECT{rect.left + scale(54), rect.top, rect.right, rect.bottom}, text, kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const int cy = (rect.top + rect.bottom) / 2;
    const int track_height = scale(18);
    RECT track{rect.left, cy - track_height / 2, rect.left + scale(42), cy + track_height / 2};
    const double position = toggle_visual_position(id, enabled);
    fill_round_rect(dc, track, blend_color(RGB(57, 69, 75), RGB(43, 111, 72), position), scale(14));
    HBRUSH knob = CreateSolidBrush(blend_color(kMuted, kOk, position));
    HGDIOBJ previous = SelectObject(dc, knob);
    const int knob_size = scale(14);
    const int knob_travel = (track.right - track.left) - knob_size - scale(6);
    const int knob_left =
        track.left + scale(3) + static_cast<int>(std::round(position * static_cast<double>(std::max(0, knob_travel))));
    Ellipse(dc, knob_left, cy - knob_size / 2, knob_left + knob_size, cy + knob_size / 2);
    SelectObject(dc, previous);
    DeleteObject(knob);
}

// Purpose: Draw a DPI-scaled checkbox row.
// Inputs: `dc`, `rect`, `text`, `checked`, and `interactive` describe the visual state.
// Outputs: Renders a checkbox and label into `dc`.
void MainWindow::draw_checkbox(HDC dc, const RECT& rect, const wchar_t* text, bool checked, bool interactive) {
    draw_interactive_hover_surface(dc, rect, interactive);
    const int box_size = scale(16);
    const int cy = rect.top + ((rect.bottom - rect.top) / 2);
    RECT box{rect.left, cy - (box_size / 2), rect.left + box_size, cy - (box_size / 2) + box_size};
    const COLORREF fill = checked ? kAccent : (interactive ? interactive_fill(kPanel2, rect, true) : kPanel);
    const COLORREF stroke = checked ? kAccent2 : (interactive ? kBorder : RGB(42, 52, 56));
    const COLORREF text_color = interactive ? kText : kMuted;
    fill_rect(dc, box, fill);
    stroke_rect(dc, box, stroke);
    if (checked) {
        draw_line(dc, box.left + scale(3), box.top + scale(8), box.left + scale(7), box.bottom - scale(4), kText);
        draw_line(dc, box.left + scale(7), box.bottom - scale(4), box.right - scale(3), box.top + scale(4), kText);
    }
    if (text != nullptr && text[0] != L'\0') {
        draw_text(dc, RECT{rect.left + scale(26), rect.top, rect.right, rect.bottom}, text, text_color,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

// Purpose: Draw a form field or select-style value box.
// Inputs: `dc` is the target, `rect` is the box, `label` names the field, `value` is display text, `select` adds an
// affordance, `enabled` controls disabled styling, and `clickable` enables hover without a menu arrow. Outputs: Renders
// label and bordered field with ellipsized value.
void MainWindow::draw_field(HDC dc, const RECT& rect, const wchar_t* label, const std::wstring& value, bool select,
                            bool enabled, bool clickable, COLORREF value_color_override) {
    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{rect.left, rect.top, rect.right, rect.top + scale(18)}, label, enabled ? kMuted : kSubtle,
              DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT box{rect.left, rect.top + scale(20), rect.right, rect.bottom};
    const COLORREF base = enabled ? kPanel2 : RGB(20, 28, 31);
    fill_round_rect(dc, box, interactive_fill(base, box, enabled && (select || clickable)), scale(3));
    stroke_rect(dc, box, enabled ? kBorder : RGB(39, 50, 55));
    const COLORREF value_color =
        value_color_override == CLR_INVALID ? (enabled ? kText : kSubtle) : value_color_override;
    draw_text(dc, field_value_cell(rect, select), value, value_color,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (select && enabled) {
        const int arrow_cx = box.right - scale(15);
        const int arrow_cy = (box.top + box.bottom) / 2;
        POINT arrow[3] = {
            {arrow_cx - scale(5), arrow_cy - scale(2)},
            {arrow_cx + scale(5), arrow_cy - scale(2)},
            {arrow_cx, arrow_cy + scale(4)},
        };
        HBRUSH brush = CreateSolidBrush(kMuted);
        HGDIOBJ previous = SelectObject(dc, brush);
        Polygon(dc, arrow, 3);
        SelectObject(dc, previous);
        DeleteObject(brush);
    }
}

// Purpose: Draw the currently expanded select/dropdown menu.
// Inputs: `dc` is the target, `content` is the active content area, and `state` is copied UI state.
// Outputs: Renders the active dropdown menu using the same row height as hit testing.
void MainWindow::draw_active_dropdown(HDC dc, const RECT& content, const UiState& state) {
    if (state.active_dropdown == DropdownId::None) {
        return;
    }
    const auto options = dropdown_options(state.active_dropdown);
    if (options.empty()) {
        return;
    }
    const RECT menu = dropdown_menu_rect(state.active_dropdown, content);
    if (menu.right <= menu.left || menu.bottom <= menu.top) {
        return;
    }
    const RECT shadow{menu.left + scale(3), menu.top + scale(3), menu.right + scale(3), menu.bottom + scale(3)};
    fill_round_rect(dc, shadow, RGB(6, 10, 12), scale(4));
    fill_round_rect(dc, menu, kPanel2, scale(4));
    stroke_rect(dc, menu, kAccent);

    const int row_height = scale(options.size() > 10U ? 28 : 32);
    const int selected = dropdown_selected_index(state, state.active_dropdown);
    const int keyboard_selected =
        dropdown_keyboard_index_ >= 0 && dropdown_keyboard_index_ < static_cast<int>(options.size())
            ? dropdown_keyboard_index_
            : selected;
    SelectObject(dc, tiny_font_);
    for (int index = 0; index < static_cast<int>(options.size()); ++index) {
        const RECT row{menu.left + scale(1), menu.top + scale(1) + (index * row_height), menu.right - scale(1),
                       menu.top + scale(1) + ((index + 1) * row_height)};
        const bool is_selected = index == selected;
        const bool is_keyboard_selected = index == keyboard_selected;
        const COLORREF base = is_selected ? RGB(126, 24, 31) : ((index % 2 == 0) ? kPanel2 : kPanel);
        const COLORREF keyed = is_keyboard_selected && !is_selected ? RGB(46, 63, 70) : base;
        fill_rect(dc, row, interactive_fill(keyed, row, true, is_selected));
        if (index > 0) {
            draw_line(dc, menu.left + scale(8), row.top, menu.right - scale(8), row.top, kBorder);
        }
        const RECT text_rect{row.left + scale(12), row.top, row.right - scale(38), row.bottom};
        if (is_selected) {
            const int check_mid = (row.top + row.bottom) / 2;
            draw_line(dc, row.right - scale(28), check_mid, row.right - scale(23), check_mid + scale(5), kText);
            draw_line(dc, row.right - scale(23), check_mid + scale(5), row.right - scale(13), check_mid - scale(6),
                      kText);
        }
        draw_text(dc, text_rect, options[static_cast<std::size_t>(index)], is_selected ? kText : kMuted,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

// Purpose: Resolve the field rectangle that owns a dropdown menu.
// Inputs: `id` identifies the dropdown and `content` is the current page content rectangle.
// Outputs: Returns a DPI-scaled anchor rectangle or an empty rectangle for inactive IDs.
RECT MainWindow::dropdown_anchor_rect(DropdownId id, const RECT& content) const {
    switch (id) {
    case DropdownId::CompressFormat:
        return compress_layout(content).format;
    case DropdownId::CompressLevel:
        return compress_layout(content).compression_level;
    case DropdownId::CompressMethod:
        return compress_layout(content).method;
    case DropdownId::CompressBlockSize:
        return compress_layout(content).block_size;
    case DropdownId::ExtractOverwrite:
        return extract_layout(content).overwrite_policy;
    case DropdownId::HistoryOperation: {
        const RECT area = inset_rect(content, scale(kPageInsetX), scale(kPageInsetY));
        return RECT{area.left, area.top + scale(48), area.left + scale(220), area.top + scale(92)};
    }
    case DropdownId::HistoryStatus: {
        const RECT area = inset_rect(content, scale(kPageInsetX), scale(kPageInsetY));
        return RECT{area.left + scale(238), area.top + scale(48), area.left + scale(458), area.top + scale(92)};
    }
    case DropdownId::GpuUpdateSpeed: {
        const RECT area = inset_rect(content, scale(kPageInsetX), scale(kPageInsetY));
        const RECT monitor{area.left, area.top + scale(342), area.right, area.bottom};
        return performance_update_speed_rect(monitor);
    }
    case DropdownId::SettingsMemoryPolicy:
        return settings_layout(content).memory_policy;
    case DropdownId::SettingsLogLevel:
        return settings_layout(content).log_level;
    case DropdownId::SettingsLogRetention:
        return settings_layout(content).log_retention;
    case DropdownId::None:
        return RECT{};
    }
    return RECT{};
}

// Purpose: Resolve the overlay menu rectangle for a dropdown.
// Inputs: `id` identifies the dropdown and `content` is the current content rectangle.
// Outputs: Returns a DPI-scaled menu rectangle positioned inside the content area.
RECT MainWindow::dropdown_menu_rect(DropdownId id, const RECT& content) const {
    const auto options = dropdown_options(id);
    if (options.empty()) {
        return RECT{};
    }
    RECT anchor = dropdown_anchor_rect(id, content);
    if (anchor.right <= anchor.left || anchor.bottom <= anchor.top) {
        return RECT{};
    }
    const int gap = scale(4);
    const int row_height = scale(options.size() > 10U ? 28 : 32);
    const int menu_height = row_height * static_cast<int>(options.size()) + scale(2);
    int top = anchor.bottom + gap;
    int bottom = top + menu_height;
    if (bottom > content.bottom - scale(8)) {
        bottom = anchor.top - gap;
        top = bottom - menu_height;
    }
    if (top < content.top + scale(8)) {
        top = content.top + scale(8);
        bottom = top + menu_height;
    }
    return RECT{anchor.left, top, anchor.right, bottom};
}

// Purpose: Draw the permanent top accent rule for the content surface.
// Inputs: `dc` is the target and `rect` is the content surface rectangle.
// Outputs: Renders a stable accent line without page-change animation.
void MainWindow::draw_tab_transition(HDC dc, const RECT& rect) {
    fill_rect(dc, RECT{rect.left, rect.top, rect.right, rect.top + scale(2)}, kAccent);
}

RECT MainWindow::content_rect() const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int top_bar = scale(kTopBar);
    const int rail_width = scale(kRailWidth);
    const int status_bar = scale(kStatusBar);
    return RECT{rail_width, top_bar, client.right, client.bottom - status_bar};
}

// Purpose: Return keyboard-focusable controls for the current page.
// Inputs: `content` is the current content rectangle and `state` is a copied UI snapshot.
// Outputs: Returns controls in Tab order using the same geometry as mouse hit testing.

}  // namespace superzip::app
