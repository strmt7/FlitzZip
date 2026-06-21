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

// Purpose: Track pointer hover state and arm native leave notifications.
// Inputs: `lparam` contains client-coordinate mouse position from `WM_MOUSEMOVE`.
// Outputs: Updates hover state, resize cursors, tooltip tracking, and returns the handled Win32 result.
LRESULT MainWindow::handle_mouse_move(LPARAM lparam) {
    mouse_position_ = POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    mouse_inside_client_ = true;
    if (primary_mouse_down_ && queue_column_resize_separator_ >= 0) {
        update_queue_column_resize(mouse_position_.x);
    } else if (primary_mouse_down_ && history_column_resize_separator_ >= 0) {
        update_history_column_resize(mouse_position_.x);
    } else if (primary_mouse_down_ && queue_scroll_dragging_) {
        update_queue_scroll_drag(mouse_position_.y);
    }
    if (!mouse_tracking_) {
        TRACKMOUSEEVENT event{};
        event.cbSize = sizeof(event);
        event.dwFlags = TME_LEAVE;
        event.hwndTrack = hwnd_;
        mouse_tracking_ = TrackMouseEvent(&event) != FALSE;
    }
    if (queue_column_resize_separator_ >= 0 || history_column_resize_separator_ >= 0) {
        SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
    } else {
        UiState state;
        {
            std::lock_guard lock(mutex_);
            state = state_;
        }
        if (state.extract_overwrite_prompt_visible || state.license_notices_dialog_visible) {
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            update_text_tooltip_tracking();
            request_repaint();
            return 0;
        }
        bool over_resize_grip = false;
        if (state.page == Page::Queue) {
            const auto layout = queue_layout(content_rect());
            const int header_bottom = layout.table.top + scale(kQueueHeaderHeight);
            if (mouse_position_.y >= layout.table.top && mouse_position_.y < header_bottom) {
                const RECT header_row{layout.table.left, layout.table.top, layout.table.right, header_bottom};
                const auto columns =
                    queue_column_layout(queue_columns_table(layout.table, state.queued_paths.size()), header_row);
                over_resize_grip = std::ranges::any_of(columns.resize_grips, [this](const RECT& grip) {
                    return contains_point(grip, mouse_position_.x, mouse_position_.y);
                });
            }
        }
        if (state.page == Page::History) {
            const RECT area = inset_rect(content_rect(), scale(kPageInsetX), scale(kPageInsetY));
            const RECT table{area.left, area.top + scale(112), area.right, area.bottom - scale(96)};
            const int header_bottom = table.top + scale(kQueueHeaderHeight);
            if (mouse_position_.y >= table.top && mouse_position_.y < header_bottom) {
                const RECT header_row{table.left, table.top, table.right, header_bottom};
                const auto columns = history_column_layout(table, header_row);
                over_resize_grip = std::ranges::any_of(columns.resize_grips, [this](const RECT& grip) {
                    return contains_point(grip, mouse_position_.x, mouse_position_.y);
                });
            }
        }
        SetCursor(LoadCursor(nullptr, over_resize_grip ? IDC_SIZEWE : IDC_ARROW));
    }
    update_text_tooltip_tracking();
    request_repaint();
    return 0;
}

// Purpose: Clear pointer hover state after the cursor leaves the client area.
// Inputs: None; uses current capture state.
// Outputs: Updates mouse state and returns the handled Win32 result.
LRESULT MainWindow::handle_mouse_leave() {
    mouse_inside_client_ = false;
    if (!mouse_capture_active_) {
        primary_mouse_down_ = false;
    }
    mouse_tracking_ = false;
    KillTimer(hwnd_, kTextTooltipTimer);
    text_tooltip_cell_active_ = false;
    text_tooltip_visible_ = false;
    text_tooltip_text_.clear();
    request_repaint();
    return 0;
}

// Purpose: Decide whether a text value overflows the visible cell.
// Inputs: `text` is rendered in `cell` with `font`.
// Outputs: Returns true when the text would be ellipsized.
bool MainWindow::text_overflows_cell(std::wstring_view text, const RECT& cell, HFONT font) const {
    if (text.empty() || cell.right <= cell.left) {
        return false;
    }
    return text_width(text, font) > (cell.right - cell.left);
}

// Purpose: Measure text with an existing GDI font.
// Inputs: `text` is UTF-16 content and `font` is one of the window-owned fonts.
// Outputs: Returns the text width in physical pixels, or zero when measurement is unavailable.
int MainWindow::text_width(std::wstring_view text, HFONT font) const {
    if (text.empty() || font == nullptr) {
        return 0;
    }
    HDC dc = GetDC(hwnd_);
    if (dc == nullptr) {
        return 0;
    }
    HGDIOBJ previous = SelectObject(dc, font);
    SIZE size{};
    const BOOL measured =
        GetTextExtentPoint32W(dc, text.data(), static_cast<int>(std::min<std::size_t>(text.size(), INT_MAX)), &size);
    SelectObject(dc, previous);
    ReleaseDC(hwnd_, dc);
    return measured == FALSE ? 0 : size.cx;
}

// Purpose: Return the ellipsized text under the current mouse, if any.
// Inputs: None; uses current page layout, queue/form state, and selected fonts.
// Outputs: Returns true with cell/text set only for eligible truncated text targets.
bool MainWindow::text_tooltip_candidate_at_mouse(RECT& cell, std::wstring& text) {
    if (!mouse_inside_client_ || primary_mouse_down_) {
        return false;
    }
    UiState state;
    {
        std::lock_guard lock(mutex_);
        state = state_;
    }
    if (state.extract_overwrite_prompt_visible || state.license_notices_dialog_visible) {
        return false;
    }
    return queue_text_tooltip_candidate_at_mouse(state, cell, text) ||
           field_text_tooltip_candidate_at_mouse(state, cell, text);
}

// Purpose: Return the ellipsized Queue text under the current mouse, if any.
// Inputs: `state` is a stable UI snapshot and `cell`/`text` receive tooltip data.
// Outputs: Returns true only for truncated Queue Name or Path cells.
bool MainWindow::queue_text_tooltip_candidate_at_mouse(const UiState& state, RECT& cell, std::wstring& text) {
    if (state.page != Page::Queue || state.queued_paths.empty()) {
        return false;
    }
    const auto layout = queue_layout(content_rect());
    const int header_bottom = layout.table.top + scale(kQueueHeaderHeight);
    const int row_height = scale(kQueueRowHeight);
    const RECT columns_table = queue_columns_table(layout.table, state.queued_paths.size());
    if (row_height <= 0 || mouse_position_.y < header_bottom || mouse_position_.y >= layout.table.bottom ||
        mouse_position_.x >= columns_table.right) {
        return false;
    }
    const int first_visible_row =
        std::clamp(queue_scroll_first_row_, 0, queue_max_scroll_offset(layout.table, state.queued_paths.size()));
    const int visible_index = (mouse_position_.y - header_bottom) / row_height;
    const int row_index = first_visible_row + visible_index;
    if (row_index < 0 || row_index >= static_cast<int>(state.queued_paths.size())) {
        return false;
    }
    const RECT row{columns_table.left, header_bottom + (visible_index * row_height), columns_table.right,
                   header_bottom + ((visible_index + 1) * row_height)};
    const auto columns = queue_column_layout(columns_table, row);
    const auto& path = state.queued_paths[static_cast<std::size_t>(row_index)];
    const RECT name_cell = inset_rect(columns.name, scale(8), 0);
    const RECT path_cell = inset_rect(columns.path, scale(8), 0);
    if (contains_point(name_cell, mouse_position_.x, mouse_position_.y)) {
        auto value = path.filename().wstring();
        if (text_overflows_cell(value, name_cell, tiny_font_)) {
            cell = name_cell;
            text = std::move(value);
            return true;
        }
    }
    if (contains_point(path_cell, mouse_position_.x, mouse_position_.y)) {
        auto value = path.wstring();
        if (text_overflows_cell(value, path_cell, tiny_font_)) {
            cell = path_cell;
            text = std::move(value);
            return true;
        }
    }
    return false;
}

// Purpose: Return the ellipsized Compress/Extract field text under the current mouse, if any.
// Inputs: `state` is a stable UI snapshot and `cell`/`text` receive tooltip data.
// Outputs: Returns true only for truncated Archive/Destination fields explicitly allowed by the UI contract.
bool MainWindow::field_text_tooltip_candidate_at_mouse(const UiState& state, RECT& cell, std::wstring& text) {
    const RECT content = content_rect();
    if (state.page == Page::Compress) {
        const auto layout = compress_layout(content);
        return tooltip_candidate_for_field(layout.archive_name, compression_output_filename_for(state), cell, text) ||
               tooltip_candidate_for_field(layout.destination, destination_directory_or_default(state).wstring(), cell,
                                           text);
    }
    if (state.page == Page::Extract) {
        const auto layout = extract_layout(content);
        const auto archive = selected_extract_archive_text(selected_extract_archive_paths(state));
        return tooltip_candidate_for_field(layout.archive, archive, cell, text) ||
               tooltip_candidate_for_field(layout.destination, extraction_output_path_for(state).wstring(), cell, text);
    }
    return false;
}

// Purpose: Check one form field value for delayed tooltip eligibility.
// Inputs: `field` is the full labeled field rectangle, `value` is the visible text, and `cell`/`text` receive data.
// Outputs: Returns true only when the mouse is over a truncated value cell.
bool MainWindow::tooltip_candidate_for_field(const RECT& field, const std::wstring& value, RECT& cell,
                                             std::wstring& text) const {
    const RECT value_cell = field_value_cell(field, false);
    if (!contains_point(value_cell, mouse_position_.x, mouse_position_.y) ||
        !text_overflows_cell(value, value_cell, tiny_font_)) {
        return false;
    }
    cell = value_cell;
    text = value;
    return true;
}

// Purpose: Resolve the drawn value area inside a labeled field.
// Inputs: `field` is the full labeled field rectangle and `select` reserves space for a dropdown arrow when true.
// Outputs: Returns the value text rectangle used by both drawing and tooltip hit testing.
RECT MainWindow::field_value_cell(const RECT& field, bool select) const {
    const RECT box{field.left, field.top + scale(20), field.right, field.bottom};
    return RECT{box.left + scale(10), box.top, box.right - scale(select ? 30 : 10), box.bottom};
}

// Purpose: Update delayed text-tooltip tracking from the current mouse position.
// Inputs: None; reads current page, eligible text cells, and mouse coordinates.
// Outputs: Arms, hides, or preserves the tooltip timer based on ellipsized hover state.
void MainWindow::update_text_tooltip_tracking() {
    RECT candidate_cell{};
    std::wstring candidate_text;
    if (!text_tooltip_candidate_at_mouse(candidate_cell, candidate_text)) {
        KillTimer(hwnd_, kTextTooltipTimer);
        if (text_tooltip_cell_active_ || text_tooltip_visible_) {
            text_tooltip_cell_active_ = false;
            text_tooltip_visible_ = false;
            text_tooltip_text_.clear();
            request_repaint();
        }
        return;
    }
    const bool same_cell = text_tooltip_cell_active_ && EqualRect(&text_tooltip_cell_, &candidate_cell) != FALSE &&
                           text_tooltip_text_ == candidate_text;
    const bool same_point =
        text_tooltip_anchor_point_.x == mouse_position_.x && text_tooltip_anchor_point_.y == mouse_position_.y;
    if (same_cell && same_point) {
        return;
    }
    text_tooltip_cell_ = candidate_cell;
    text_tooltip_anchor_point_ = mouse_position_;
    text_tooltip_cell_active_ = true;
    text_tooltip_visible_ = false;
    text_tooltip_text_ = std::move(candidate_text);
    KillTimer(hwnd_, kTextTooltipTimer);
    SetTimer(hwnd_, kTextTooltipTimer, kTextTooltipDelayMs, nullptr);
    request_repaint();
}

// Purpose: Handle primary-button press using the same geometry as rendering.
// Inputs: `lparam` contains client-coordinate click position from `WM_LBUTTONDOWN`.
// Outputs: Updates capture/pressed state, dispatches page or content clicks, and returns the handled Win32 result.
LRESULT MainWindow::handle_primary_mouse_down(LPARAM lparam) {
    // Use the same scaled geometry for hit testing that the renderer uses, so
    // high-DPI displays do not create visual/click drift.
    const int x = GET_X_LPARAM(lparam);
    const int y = GET_Y_LPARAM(lparam);
    mouse_position_ = POINT{x, y};
    mouse_inside_client_ = true;
    primary_mouse_down_ = true;
    SetCapture(hwnd_);
    mouse_capture_active_ = true;
    request_repaint();

    bool overwrite_modal_visible = false;
    bool license_modal_visible = false;
    {
        std::lock_guard lock(mutex_);
        overwrite_modal_visible = state_.extract_overwrite_prompt_visible;
        license_modal_visible = state_.license_notices_dialog_visible;
    }
    if (license_modal_visible) {
        (void)handle_license_notices_dialog_click(x, y);
        return 0;
    }
    if (overwrite_modal_visible) {
        (void)handle_extract_overwrite_prompt_click(x, y);
        return 0;
    }

    const int rail_width = scale(kRailWidth);
    const int top_bar = scale(kTopBar);
    if (y < top_bar) {
        close_active_dropdown();
    } else if (x < rail_width) {
        const int nav_top = top_bar + scale(10);
        const int item_height = scale(63);
        const int item = item_height > 0 ? (y - nav_top) / item_height : -1;
        if (item >= 0 && item < 8) {
            close_active_dropdown();
            set_page(static_cast<Page>(item));
        }
    } else {
        (void)handle_content_click(x, y);
    }
    return 0;
}

// Purpose: Handle primary-button release and trigger the shared command release pulse.
// Inputs: `lparam` contains client-coordinate release position from `WM_LBUTTONUP`.
// Outputs: Releases capture, updates mouse state, queues animation, and returns the handled Win32 result.
LRESULT MainWindow::handle_primary_mouse_up(LPARAM lparam) {
    mouse_position_ = POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    primary_mouse_down_ = false;
    if (mouse_capture_active_) {
        ReleaseCapture();
        mouse_capture_active_ = false;
    }
    end_queue_column_resize();
    end_history_column_resize();
    end_queue_scroll_drag();
    if (mouse_inside_client_) {
        start_button_release_animation(mouse_position_);
    }
    request_repaint();
    return 0;
}

// Purpose: Normalize mouse state after Windows changes capture ownership.
// Inputs: None.
// Outputs: Clears pressed/capture flags and returns the handled Win32 result.
LRESULT MainWindow::handle_capture_changed() {
    primary_mouse_down_ = false;
    mouse_capture_active_ = false;
    end_queue_column_resize();
    end_history_column_resize();
    end_queue_scroll_drag();
    request_repaint();
    return 0;
}

// Purpose: Scroll the Queue table with native mouse-wheel semantics.
// Inputs: `wparam` contains wheel delta and `lparam` contains screen coordinates.
// Outputs: Updates the first visible Queue row when the pointer is over an overflowing Queue table.
LRESULT MainWindow::handle_mouse_wheel(WPARAM wparam, LPARAM lparam) {
    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    ScreenToClient(hwnd_, &point);
    UiState state;
    {
        std::lock_guard lock(mutex_);
        state = state_;
    }
    if (state.license_notices_dialog_visible) {
        const int wheel_steps = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        if (wheel_steps != 0) {
            (void)scroll_license_notices_dialog(-wheel_steps * scale(96));
        }
        return 0;
    }
    if (state.page != Page::Queue || state.queued_paths.empty()) {
        return DefWindowProcW(hwnd_, WM_MOUSEWHEEL, wparam, lparam);
    }
    const auto layout = queue_layout(content_rect());
    if (!contains_point(layout.table, point.x, point.y) ||
        queue_max_scroll_offset(layout.table, state.queued_paths.size()) == 0) {
        return DefWindowProcW(hwnd_, WM_MOUSEWHEEL, wparam, lparam);
    }

    queue_wheel_delta_remainder_ += GET_WHEEL_DELTA_WPARAM(wparam);
    const int wheel_steps = queue_wheel_delta_remainder_ / WHEEL_DELTA;
    queue_wheel_delta_remainder_ %= WHEEL_DELTA;
    if (wheel_steps != 0) {
        (void)scroll_queue_rows(-wheel_steps * 3);
    }
    return 0;
}

}  // namespace superzip::app
