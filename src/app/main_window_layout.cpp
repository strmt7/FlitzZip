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

// Purpose: Compute Queue page rectangles shared by rendering and hit testing.
// Inputs: `rect` is the content area in physical pixels.
// Outputs: Returns DPI-scaled Queue page control rectangles.
MainWindow::QueueLayout MainWindow::queue_layout(const RECT& rect) const {
    QueueLayout layout{};
    layout.area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    const int header_top = layout.area.top;
    const int header_bottom = header_top + scale(kPageHeaderHeight);
    layout.clear = RECT{layout.area.right - scale(72), header_top, layout.area.right, header_bottom};
    layout.add_folder =
        RECT{layout.clear.left - scale(12) - scale(108), header_top, layout.clear.left - scale(12), header_bottom};
    layout.add_files = RECT{layout.add_folder.left - scale(12) - scale(96), header_top,
                            layout.add_folder.left - scale(12), header_bottom};
    const int remove_width = scale(136);
    const int remove_left =
        layout.area.left + ((layout.add_files.left - scale(28) - layout.area.left) - remove_width) / 2;
    layout.remove_selected = RECT{remove_left, header_top, remove_left + remove_width, header_bottom};
    layout.table = RECT{layout.area.left, layout.area.top + scale(54), layout.area.right, layout.area.bottom};
    return layout;
}

// Purpose: Return the shared bottom-right primary action button rectangle for operation pages.
// Inputs: `area` is the DPI-scaled page content area.
// Outputs: Returns a 110x36 design-pixel command rectangle aligned with Settings Apply.
RECT MainWindow::primary_action_rect(const RECT& area) const {
    return RECT{area.right - scale(110), area.bottom - scale(54), area.right, area.bottom - scale(18)};
}

// Purpose: Return the standard page-title text rectangle aligned to page action buttons.
// Inputs: `area` is the DPI-scaled page content area.
// Outputs: Returns the shared title rectangle for every top-level page header.
RECT MainWindow::page_title_rect(const RECT& area) const {
    return RECT{area.left, area.top, area.right, area.top + scale(kPageHeaderHeight)};
}

// Purpose: Return the secondary action button aligned immediately left of a primary action.
// Inputs: `primary` is a DPI-scaled primary action rectangle.
// Outputs: Returns a same-sized button with the standard command gap.
RECT MainWindow::secondary_action_rect_left_of(const RECT& primary) const {
    const int gap = scale(12);
    const int width = primary.right - primary.left;
    return RECT{primary.left - gap - width, primary.top, primary.left - gap, primary.bottom};
}

// Purpose: Return the History Clear History button rectangle with Restore Defaults-equivalent visual margins.
// Inputs: `area` is the DPI-scaled page content area.
// Outputs: Returns a right-aligned command rectangle sized from the active button font.
RECT MainWindow::history_clear_button_rect(const RECT& area) const {
    const int restore_width = scale(134);
    const int restore_text = text_width(L"Restore Defaults", tiny_font_);
    const int clear_text = text_width(L"Clear History", tiny_font_);
    const int margin = std::max(scale(12), (restore_width - restore_text) / 2);
    const int width = std::clamp(clear_text + (margin * 2), scale(92), restore_width);
    return RECT{area.right - width, area.top, area.right, area.top + scale(34)};
}

// Purpose: Compute History page rectangles shared by rendering, scrolling, and hit testing.
// Inputs: `rect` is the content area in physical pixels.
// Outputs: Returns DPI-scaled History page filters, table, and details panel rectangles.
MainWindow::HistoryLayout MainWindow::history_layout(const RECT& rect) const {
    HistoryLayout layout{};
    layout.area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    layout.operation_filter =
        RECT{layout.area.left, layout.area.top + scale(48), layout.area.left + scale(220), layout.area.top + scale(92)};
    layout.status_filter = RECT{layout.area.left + scale(238), layout.area.top + scale(48),
                                layout.area.left + scale(458), layout.area.top + scale(92)};
    layout.clear = history_clear_button_rect(layout.area);
    layout.table =
        RECT{layout.area.left, layout.area.top + scale(112), layout.area.right, layout.area.bottom - scale(118)};
    layout.details = RECT{layout.area.left, layout.table.bottom + scale(20), layout.area.right, layout.area.bottom};
    return layout;
}

// Purpose: Compute fixed checkbox and resizable data-column geometry for the Queue table.
// Inputs: `table` is the full table and `row` is the header or one body row.
// Outputs: Returns rectangles shared by rendering and hit testing.
MainWindow::QueueColumnLayout MainWindow::queue_column_layout(const RECT& table, const RECT& row) const {
    QueueColumnLayout columns{};
    const int checkbox_width = scale(kQueueCheckboxColumnWidth);
    const int available = std::max(scale(360), static_cast<int>(table.right - table.left) - checkbox_width - scale(22));
    const std::array<int, 4> minimums{scale(90), scale(70), scale(70), scale(120)};
    std::array<int, 4> widths{};
    int requested = 0;
    for (std::size_t i = 0; i < widths.size(); ++i) {
        widths[i] = std::max(minimums[i], scale(queue_column_widths_[i]));
        requested += widths[i];
    }
    if (requested != available && requested > 0) {
        const double ratio = static_cast<double>(available) / static_cast<double>(requested);
        int used = 0;
        for (std::size_t i = 0; i + 1U < widths.size(); ++i) {
            widths[i] = std::max(minimums[i], static_cast<int>(std::round(static_cast<double>(widths[i]) * ratio)));
            used += widths[i];
        }
        widths.back() = std::max(minimums.back(), available - used);
    }

    const int checkbox_target_size = scale(24);
    const int checkbox_left = table.left + std::max(0, (checkbox_width - checkbox_target_size) / 2);
    const int checkbox_top = row.top + std::max(0, (static_cast<int>(row.bottom - row.top) - checkbox_target_size) / 2);
    columns.header_checkbox =
        RECT{checkbox_left, checkbox_top, checkbox_left + checkbox_target_size, checkbox_top + checkbox_target_size};
    columns.checkbox = columns.header_checkbox;
    int left = table.left + checkbox_width;
    columns.name = RECT{left, row.top, left + widths[0], row.bottom};
    left = columns.name.right;
    columns.size = RECT{left, row.top, left + widths[1], row.bottom};
    left = columns.size.right;
    columns.type = RECT{left, row.top, left + widths[2], row.bottom};
    left = columns.type.right;
    columns.path = RECT{left, row.top, table.right - scale(12), row.bottom};
    const int grip = scale(kQueueResizeGripHalfWidth);
    columns.resize_grips = {
        RECT{columns.name.right - grip, row.top, columns.name.right + grip, row.bottom},
        RECT{columns.size.right - grip, row.top, columns.size.right + grip, row.bottom},
        RECT{columns.type.right - grip, row.top, columns.type.right + grip, row.bottom},
    };
    return columns;
}

// Purpose: Compute fixed-header resizable columns for the History table.
// Inputs: `table` is the full History table and `row` is the header or one body row.
// Outputs: Returns rectangles shared by rendering and hit testing.
MainWindow::HistoryColumnLayout MainWindow::history_column_layout(const RECT& table, const RECT& row) const {
    HistoryColumnLayout columns{};
    const int left_padding = scale(14);
    const int right_padding = scale(16);
    const int available =
        std::max(scale(360), static_cast<int>(table.right - table.left) - left_padding - right_padding);
    const std::array<int, 5> minimums{scale(82), scale(78), scale(140), scale(160), scale(74)};
    std::array<int, 5> widths{};
    int requested = 0;
    for (std::size_t i = 0; i < widths.size(); ++i) {
        widths[i] = std::max(minimums[i], scale(history_column_widths_[i]));
        requested += widths[i];
    }
    if (requested != available && requested > 0) {
        const double ratio = static_cast<double>(available) / static_cast<double>(requested);
        int used = 0;
        for (std::size_t i = 0; i + 1U < widths.size(); ++i) {
            widths[i] = std::max(minimums[i], static_cast<int>(std::round(static_cast<double>(widths[i]) * ratio)));
            used += widths[i];
        }
        widths.back() = std::max(minimums.back(), available - used);
    }

    int left = table.left + left_padding;
    columns.time = RECT{left, row.top, left + widths[0], row.bottom};
    left = columns.time.right;
    columns.operation = RECT{left, row.top, left + widths[1], row.bottom};
    left = columns.operation.right;
    columns.archive_name = RECT{left, row.top, left + widths[2], row.bottom};
    left = columns.archive_name.right;
    columns.archive_path = RECT{left, row.top, left + widths[3], row.bottom};
    left = columns.archive_path.right;
    columns.status = RECT{left, row.top, table.right - right_padding, row.bottom};
    const int grip = scale(kQueueResizeGripHalfWidth);
    columns.resize_grips = {
        RECT{columns.time.right - grip, row.top, columns.time.right + grip, row.bottom},
        RECT{columns.operation.right - grip, row.top, columns.operation.right + grip, row.bottom},
        RECT{columns.archive_name.right - grip, row.top, columns.archive_name.right + grip, row.bottom},
        RECT{columns.archive_path.right - grip, row.top, columns.archive_path.right + grip, row.bottom},
    };
    return columns;
}

// Purpose: Reserve Queue table body space for an overflow scrollbar when needed.
// Inputs: `table` is the full table rectangle and `row_count` is the number of queued entries.
// Outputs: Returns the column layout table with the scrollbar gutter removed only when rows overflow.
RECT MainWindow::queue_columns_table(const RECT& table, std::size_t row_count) const {
    RECT columns = table;
    if (queue_max_scroll_offset(table, row_count) > 0) {
        columns.right -= scale(kQueueScrollbarWidth + 8);
    }
    return columns;
}

// Purpose: Reserve History table body space for an overflow scrollbar when needed.
// Inputs: `table` is the full table rectangle and `row_count` is the number of filtered history entries.
// Outputs: Returns the column layout table with the scrollbar gutter removed only when rows overflow.
RECT MainWindow::history_columns_table(const RECT& table, std::size_t row_count) const {
    RECT columns = table;
    if (history_max_scroll_offset(table, row_count) > 0) {
        columns.right -= scale(kQueueScrollbarWidth + 8);
    }
    return columns;
}

// Purpose: Count the number of complete Queue rows visible below the fixed header.
// Inputs: `table` is the full Queue table rectangle.
// Outputs: Returns a non-negative visible row count.
int MainWindow::queue_visible_row_count(const RECT& table) const {
    const int body_height = std::max(0, static_cast<int>(table.bottom - (table.top + scale(kQueueHeaderHeight))));
    const int row_height = std::max(1, scale(kQueueRowHeight));
    return std::max(0, body_height / row_height);
}

// Purpose: Count the number of complete History rows visible below the fixed header.
// Inputs: `table` is the full History table rectangle.
// Outputs: Returns a non-negative visible row count.
int MainWindow::history_visible_row_count(const RECT& table) const {
    return queue_visible_row_count(table);
}

// Purpose: Compute the largest valid first visible Queue row.
// Inputs: `table` is the full Queue table rectangle and `row_count` is the number of queued entries.
// Outputs: Returns zero when no scrolling is needed, otherwise the maximum scroll offset.
int MainWindow::queue_max_scroll_offset(const RECT& table, std::size_t row_count) const {
    const int visible_rows = queue_visible_row_count(table);
    const int rows = static_cast<int>(std::min<std::size_t>(row_count, static_cast<std::size_t>(INT_MAX)));
    return std::max(0, rows - visible_rows);
}

// Purpose: Compute the largest valid first visible History row.
// Inputs: `table` is the full History table rectangle and `row_count` is the filtered row count.
// Outputs: Returns zero when no scrolling is needed, otherwise the maximum scroll offset.
int MainWindow::history_max_scroll_offset(const RECT& table, std::size_t row_count) const {
    const int visible_rows = history_visible_row_count(table);
    const int rows = static_cast<int>(std::min<std::size_t>(row_count, static_cast<std::size_t>(INT_MAX)));
    return std::max(0, rows - visible_rows);
}

// Purpose: Clamp Queue scroll state after queue or viewport changes.
// Inputs: `table` is the visible table and `row_count` is the current queue size.
// Outputs: Keeps `queue_scroll_first_row_` inside the valid range.
void MainWindow::clamp_queue_scroll_offset(const RECT& table, std::size_t row_count) {
    queue_scroll_first_row_ = std::clamp(queue_scroll_first_row_, 0, queue_max_scroll_offset(table, row_count));
}

// Purpose: Clamp History scroll state after filters, history, or viewport changes.
// Inputs: `table` is the visible table and `row_count` is the filtered history size.
// Outputs: Keeps `history_scroll_first_row_` inside the valid range.
void MainWindow::clamp_history_scroll_offset(const RECT& table, std::size_t row_count) {
    history_scroll_first_row_ = std::clamp(history_scroll_first_row_, 0, history_max_scroll_offset(table, row_count));
}

// Purpose: Return the Queue scrollbar track inside the table body.
// Inputs: `table` is the full Queue table rectangle.
// Outputs: Returns a narrow body-only track rectangle.
RECT MainWindow::queue_scrollbar_track_rect(const RECT& table) const {
    const int width = scale(kQueueScrollbarWidth);
    return RECT{table.right - width - scale(6), table.top + scale(kQueueHeaderHeight) + scale(8),
                table.right - scale(6), table.bottom - scale(8)};
}

// Purpose: Return the History scrollbar track inside the table body.
// Inputs: `table` is the full History table rectangle.
// Outputs: Returns a narrow body-only track rectangle matching Queue.
RECT MainWindow::history_scrollbar_track_rect(const RECT& table) const {
    return queue_scrollbar_track_rect(table);
}

// Purpose: Return the Queue scrollbar thumb for the current scroll position.
// Inputs: `table` is the full table and `row_count` is the number of queued entries.
// Outputs: Returns an empty rectangle when no scrollbar is required.
RECT MainWindow::queue_scrollbar_thumb_rect(const RECT& table, std::size_t row_count) const {
    const int max_offset = queue_max_scroll_offset(table, row_count);
    if (max_offset <= 0 || row_count == 0U) {
        return RECT{};
    }
    const RECT track = queue_scrollbar_track_rect(table);
    const int track_height = std::max(1, static_cast<int>(track.bottom - track.top));
    const int visible_rows = std::max(1, queue_visible_row_count(table));
    const int rows = static_cast<int>(std::min<std::size_t>(row_count, static_cast<std::size_t>(INT_MAX)));
    const int min_thumb = scale(32);
    const int thumb_height =
        std::clamp((track_height * visible_rows) / std::max(visible_rows, rows), min_thumb, track_height);
    const int travel = std::max(0, track_height - thumb_height);
    const int top = track.top + (travel * std::clamp(queue_scroll_first_row_, 0, max_offset)) / max_offset;
    return RECT{track.left, top, track.right, top + thumb_height};
}

// Purpose: Return the History scrollbar thumb for the current scroll position.
// Inputs: `table` is the full table and `row_count` is the filtered history entry count.
// Outputs: Returns an empty rectangle when no scrollbar is required.
RECT MainWindow::history_scrollbar_thumb_rect(const RECT& table, std::size_t row_count) const {
    const int max_offset = history_max_scroll_offset(table, row_count);
    if (max_offset <= 0 || row_count == 0U) {
        return RECT{};
    }
    const RECT track = history_scrollbar_track_rect(table);
    const int track_height = std::max(1, static_cast<int>(track.bottom - track.top));
    const int visible_rows = std::max(1, history_visible_row_count(table));
    const int rows = static_cast<int>(std::min<std::size_t>(row_count, static_cast<std::size_t>(INT_MAX)));
    const int min_thumb = scale(32);
    const int thumb_height =
        std::clamp((track_height * visible_rows) / std::max(visible_rows, rows), min_thumb, track_height);
    const int travel = std::max(0, track_height - thumb_height);
    const int top = track.top + (travel * std::clamp(history_scroll_first_row_, 0, max_offset)) / max_offset;
    return RECT{track.left, top, track.right, top + thumb_height};
}

// Purpose: Move the Queue table by a row delta.
// Inputs: `delta_rows` is positive for down and negative for up.
// Outputs: Mutates the first visible row and queues repaint when the visible range changes.
bool MainWindow::scroll_queue_rows(int delta_rows) {
    UiState state;
    {
        std::lock_guard lock(mutex_);
        state = state_;
    }
    if (state.page != Page::Queue || state.queued_paths.empty()) {
        return false;
    }
    const auto layout = queue_layout(content_rect());
    const int previous = queue_scroll_first_row_;
    queue_scroll_first_row_ = std::clamp(queue_scroll_first_row_ + delta_rows, 0,
                                         queue_max_scroll_offset(layout.table, state.queued_paths.size()));
    if (queue_scroll_first_row_ == previous) {
        return false;
    }
    request_repaint();
    return true;
}

// Purpose: Move the History table by a row delta.
// Inputs: `delta_rows` is positive for down and negative for up.
// Outputs: Mutates the first visible row and queues repaint when the visible range changes.
bool MainWindow::scroll_history_rows(int delta_rows) {
    UiState state;
    {
        std::lock_guard lock(mutex_);
        state = state_;
    }
    if (state.page != Page::History || state.history.empty()) {
        return false;
    }
    const auto visible = filtered_history_indices(state);
    const RECT area = inset_rect(content_rect(), scale(kPageInsetX), scale(kPageInsetY));
    const RECT table{area.left, area.top + scale(112), area.right, area.bottom - scale(96)};
    const int previous = history_scroll_first_row_;
    history_scroll_first_row_ =
        std::clamp(history_scroll_first_row_ + delta_rows, 0, history_max_scroll_offset(table, visible.size()));
    if (history_scroll_first_row_ == previous) {
        return false;
    }
    request_repaint();
    return true;
}

// Purpose: Start dragging the Queue scrollbar thumb.
// Inputs: `y` is the current client mouse y-coordinate and `table`/`row_count` describe the visible table.
// Outputs: Captures the drag baseline when scrolling is possible.
void MainWindow::begin_queue_scroll_drag(int y, const RECT& table, std::size_t row_count) {
    if (queue_max_scroll_offset(table, row_count) <= 0) {
        return;
    }
    queue_scroll_dragging_ = true;
    queue_scroll_drag_start_y_ = y;
    queue_scroll_drag_start_offset_ = queue_scroll_first_row_;
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

// Purpose: Start dragging the History scrollbar thumb.
// Inputs: `y` is the current client mouse y-coordinate and `table`/`row_count` describe the visible table.
// Outputs: Captures the drag baseline when scrolling is possible.
void MainWindow::begin_history_scroll_drag(int y, const RECT& table, std::size_t row_count) {
    if (history_max_scroll_offset(table, row_count) <= 0) {
        return;
    }
    history_scroll_dragging_ = true;
    history_scroll_drag_start_y_ = y;
    history_scroll_drag_start_offset_ = history_scroll_first_row_;
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

// Purpose: Update Queue scroll position from a scrollbar-thumb drag.
// Inputs: `y` is the current client mouse y-coordinate.
// Outputs: Mutates the first visible row and queues repaint when the thumb moves to another row.
void MainWindow::update_queue_scroll_drag(int y) {
    if (!queue_scroll_dragging_) {
        return;
    }
    UiState state;
    {
        std::lock_guard lock(mutex_);
        state = state_;
    }
    const auto layout = queue_layout(content_rect());
    const int max_offset = queue_max_scroll_offset(layout.table, state.queued_paths.size());
    if (max_offset <= 0) {
        queue_scroll_first_row_ = 0;
        return;
    }
    const RECT track = queue_scrollbar_track_rect(layout.table);
    const RECT thumb = queue_scrollbar_thumb_rect(layout.table, state.queued_paths.size());
    const int travel = std::max(1, static_cast<int>((track.bottom - track.top) - (thumb.bottom - thumb.top)));
    const int delta_rows = std::lround(static_cast<double>(y - queue_scroll_drag_start_y_) *
                                       static_cast<double>(max_offset) / static_cast<double>(travel));
    const int previous = queue_scroll_first_row_;
    queue_scroll_first_row_ = std::clamp(queue_scroll_drag_start_offset_ + delta_rows, 0, max_offset);
    if (queue_scroll_first_row_ != previous) {
        request_repaint();
    }
}

// Purpose: Update History scroll position from a scrollbar-thumb drag.
// Inputs: `y` is the current client mouse y-coordinate.
// Outputs: Mutates the first visible row and queues repaint when the thumb moves to another row.
void MainWindow::update_history_scroll_drag(int y) {
    if (!history_scroll_dragging_) {
        return;
    }
    UiState state;
    {
        std::lock_guard lock(mutex_);
        state = state_;
    }
    const auto visible = filtered_history_indices(state);
    const RECT area = inset_rect(content_rect(), scale(kPageInsetX), scale(kPageInsetY));
    const RECT table{area.left, area.top + scale(112), area.right, area.bottom - scale(96)};
    const int max_offset = history_max_scroll_offset(table, visible.size());
    if (max_offset <= 0) {
        history_scroll_first_row_ = 0;
        return;
    }
    const RECT track = history_scrollbar_track_rect(table);
    const RECT thumb = history_scrollbar_thumb_rect(table, visible.size());
    const int travel = std::max(1, static_cast<int>((track.bottom - track.top) - (thumb.bottom - thumb.top)));
    const int delta_rows = std::lround(static_cast<double>(y - history_scroll_drag_start_y_) *
                                       static_cast<double>(max_offset) / static_cast<double>(travel));
    const int previous = history_scroll_first_row_;
    history_scroll_first_row_ = std::clamp(history_scroll_drag_start_offset_ + delta_rows, 0, max_offset);
    if (history_scroll_first_row_ != previous) {
        request_repaint();
    }
}

// Purpose: End any active Queue scrollbar drag.
// Inputs: None.
// Outputs: Clears scrollbar drag state.
void MainWindow::end_queue_scroll_drag() {
    queue_scroll_dragging_ = false;
}

// Purpose: End any active History scrollbar drag.
// Inputs: None.
// Outputs: Clears scrollbar drag state.
void MainWindow::end_history_scroll_drag() {
    history_scroll_dragging_ = false;
}

// Purpose: Compute Compress page rectangles shared by rendering and hit testing.
// Inputs: `rect` is the content area in physical pixels.
// Outputs: Returns DPI-scaled Compress page control rectangles.
MainWindow::CompressLayout MainWindow::compress_layout(const RECT& rect) const {
    CompressLayout layout{};
    layout.area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    layout.start = primary_action_rect(layout.area);
    layout.stop = secondary_action_rect_left_of(layout.start);
    const int left = layout.area.left;
    const int mid = layout.area.left + (layout.area.right - layout.area.left) / 2 + scale(14);
    const int field_w = (layout.area.right - layout.area.left) / 2 - scale(26);
    layout.archive_name = RECT{left, layout.area.top + scale(54), left + field_w, layout.area.top + scale(104)};
    layout.destination = RECT{mid, layout.area.top + scale(54), mid + field_w, layout.area.top + scale(104)};
    layout.format = RECT{left, layout.area.top + scale(124), left + field_w, layout.area.top + scale(174)};
    layout.compression_level = RECT{mid, layout.area.top + scale(124), mid + field_w, layout.area.top + scale(174)};
    layout.method = RECT{left, layout.area.top + scale(194), left + field_w, layout.area.top + scale(244)};
    layout.block_size = RECT{mid, layout.area.top + scale(194), mid + field_w, layout.area.top + scale(244)};
    layout.advanced = RECT{left, layout.area.top + scale(270), layout.area.right, layout.area.top + scale(390)};
    layout.solid_archive = RECT{layout.advanced.left + scale(18), layout.advanced.top + scale(48),
                                layout.advanced.left + scale(310), layout.advanced.top + scale(76)};
    layout.store_timestamps = RECT{layout.advanced.left + scale(18), layout.advanced.top + scale(80),
                                   layout.advanced.left + scale(310), layout.advanced.top + scale(108)};
    layout.delete_after_compression = RECT{layout.advanced.left + scale(342), layout.advanced.top + scale(48),
                                           layout.advanced.left + scale(680), layout.advanced.top + scale(76)};
    layout.verify = RECT{layout.advanced.left + scale(342), layout.advanced.top + scale(80),
                         layout.advanced.left + scale(710), layout.advanced.top + scale(108)};
    layout.security = RECT{left, layout.area.top + scale(410), layout.area.right, layout.area.top + scale(528)};
    layout.sha = RECT{layout.security.left + scale(18), layout.security.top + scale(46),
                      layout.security.left + scale(420), layout.security.top + scale(78)};
    layout.defender = RECT{layout.security.left + scale(18), layout.security.top + scale(82),
                           layout.security.left + scale(420), layout.security.top + scale(114)};
    return layout;
}

// Purpose: Compute Extract page rectangles shared by rendering and hit testing.
// Inputs: `rect` is the content area in physical pixels.
// Outputs: Returns DPI-scaled Extract page control rectangles.
MainWindow::ExtractLayout MainWindow::extract_layout(const RECT& rect) const {
    ExtractLayout layout{};
    layout.area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    layout.start = primary_action_rect(layout.area);
    layout.stop = secondary_action_rect_left_of(layout.start);
    const int left = layout.area.left;
    const int mid = layout.area.left + (layout.area.right - layout.area.left) / 2 + scale(14);
    const int field_w = (layout.area.right - layout.area.left) / 2 - scale(26);
    layout.archive = RECT{left, layout.area.top + scale(54), left + field_w, layout.area.top + scale(104)};
    layout.destination = RECT{mid, layout.area.top + scale(54), mid + field_w, layout.area.top + scale(104)};
    layout.path_mode = RECT{left, layout.area.top + scale(124), left + field_w, layout.area.top + scale(174)};
    layout.overwrite_policy = RECT{mid, layout.area.top + scale(124), mid + field_w, layout.area.top + scale(174)};
    layout.checks =
        RECT{layout.area.left, layout.area.top + scale(200), layout.area.right, layout.area.top + scale(332)};
    layout.verify_metadata = RECT{layout.checks.left + scale(18), layout.checks.top + scale(48),
                                  layout.checks.left + scale(420), layout.checks.top + scale(78)};
    layout.open_destination_after_extract = RECT{layout.checks.left + scale(18), layout.checks.top + scale(80),
                                                 layout.checks.left + scale(420), layout.checks.top + scale(110)};
    layout.sha = RECT{layout.checks.left + scale(470), layout.checks.top + scale(48), layout.checks.right - scale(20),
                      layout.checks.top + scale(80)};
    layout.defender = RECT{layout.checks.left + scale(470), layout.checks.top + scale(84),
                           layout.checks.right - scale(20), layout.checks.top + scale(116)};
    return layout;
}

// Purpose: Compute Settings page rectangles shared by rendering and hit testing.
// Inputs: `rect` is the content area in physical pixels.
// Outputs: Returns DPI-scaled Settings page control rectangles.
MainWindow::SettingsLayout MainWindow::settings_layout(const RECT& rect) const {
    SettingsLayout layout{};
    layout.area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    layout.restore_defaults = RECT{layout.area.right - scale(260), layout.area.bottom - scale(54),
                                   layout.area.right - scale(126), layout.area.bottom - scale(18)};
    layout.apply = primary_action_rect(layout.area);
    const int panel_top = layout.area.top + scale(54);
    const int panel_bottom = panel_top + scale(168);
    layout.general = RECT{layout.area.left, panel_top, layout.area.left + scale(470), panel_bottom};
    layout.security = RECT{layout.general.left, layout.general.bottom + scale(16), layout.general.right,
                           layout.general.bottom + scale(176)};
    layout.performance = RECT{layout.general.right + scale(18), layout.general.top, layout.area.right, panel_bottom};
    layout.logging = RECT{layout.performance.left, layout.performance.bottom + scale(16), layout.area.right,
                          layout.performance.bottom + scale(176)};
    layout.sha = RECT{layout.security.left + scale(18), layout.security.top + scale(48),
                      layout.security.right - scale(16), layout.security.top + scale(80)};
    layout.defender = RECT{layout.security.left + scale(18), layout.security.top + scale(84),
                           layout.security.right - scale(16), layout.security.top + scale(116)};
    layout.gpu = RECT{layout.security.left + scale(18), layout.security.top + scale(120),
                      layout.security.right - scale(16), layout.security.top + scale(152)};
    layout.verify = RECT{layout.performance.left + scale(18), layout.performance.top + scale(48),
                         layout.performance.right - scale(18), layout.performance.top + scale(80)};
    const int performance_half_right =
        layout.performance.left + (layout.performance.right - layout.performance.left) / 2;
    const int logging_half_right = layout.logging.left + (layout.logging.right - layout.logging.left) / 2;
    layout.memory_policy = RECT{layout.performance.left + scale(18), layout.performance.top + scale(94),
                                performance_half_right, layout.performance.top + scale(140)};
    layout.log_level = RECT{layout.logging.left + scale(18), layout.logging.top + scale(48), logging_half_right,
                            layout.logging.top + scale(94)};
    layout.log_retention = RECT{layout.logging.left + scale(18), layout.logging.top + scale(106), logging_half_right,
                                layout.logging.top + scale(152)};
    const int log_button_width = layout.restore_defaults.right - layout.restore_defaults.left;
    layout.open_log_file = RECT{layout.logging.right - scale(18) - log_button_width, layout.logging.top + scale(82),
                                layout.logging.right - scale(18), layout.logging.top + scale(118)};
    layout.open_destination_after_operation = RECT{layout.general.left + scale(18), layout.general.top + scale(48),
                                                   layout.general.right - scale(16), layout.general.top + scale(78)};
    layout.confirm_before_deleting = RECT{layout.general.left + scale(18), layout.general.top + scale(82),
                                          layout.general.right - scale(16), layout.general.top + scale(112)};
    layout.show_operation_summary = RECT{layout.general.left + scale(18), layout.general.top + scale(116),
                                         layout.general.right - scale(16), layout.general.top + scale(146)};
    return layout;
}

// Purpose: Draw Queue page title and queue-management commands.
// Inputs: `dc` is the paint target, `layout` holds DPI-scaled rectangles, and `state` is the copied UI state.
// Outputs: Renders the title, optional Remove selected, Add files, Add folder, Clear, and item-count controls.

}  // namespace superzip::app
