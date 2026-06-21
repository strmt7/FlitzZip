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

// Purpose: Draw the Queue page toolbar controls.
// Inputs: `dc` is the target, `layout` contains control rectangles, and `state` is copied UI state.
// Outputs: Renders item count and visible Queue commands without mutating state.
void MainWindow::draw_queue_toolbar(HDC dc, const QueueLayout& layout, const UiState& state) {
    const RECT area = layout.area;
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, layout.add_files.left - scale(18), area.top + scale(kPageTitleTextHeight)},
              L"Queue", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    if (has_selected_queue_items(state)) {
        draw_button(dc, layout.remove_selected, L"Remove selected", false);
    }
    draw_button(dc, layout.add_files, L"+ Add files", false);
    draw_button(dc, layout.add_folder, L"+ Add folder", false);
    draw_button(dc, layout.clear, L"Clear", false);
    SelectObject(dc, tiny_font_);
    const auto count_text =
        std::to_wstring(state.queued_paths.size()) + L" item" + (state.queued_paths.size() == 1 ? L"" : L"s");
    draw_text(dc,
              RECT{layout.add_files.left - scale(120), area.top, layout.add_files.left - scale(18),
                   area.top + scale(kPageHeaderHeight)},
              count_text, kMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

// Purpose: Draw the fixed Queue table header row.
// Inputs: `dc` is the paint target, `table`/`columns_table` describe table geometry, and `state` is copied UI state.
// Outputs: Renders select-all checkbox, column titles, resize separators, and header/body divider.
void MainWindow::draw_queue_table_header(HDC dc, const RECT& table, const RECT& columns_table, const UiState& state) {
    SelectObject(dc, tiny_font_);
    const int header_bottom = table.top + scale(kQueueHeaderHeight);
    const RECT header_row{table.left, table.top, table.right, header_bottom};
    const RECT header_band{table.left + scale(1), table.top + scale(1), table.right - scale(1), header_bottom};
    fill_rect(dc, header_band, blend_color(kPanel, kPanel2, 0.52));
    const auto header_columns = queue_column_layout(columns_table, header_row);
    const bool has_entries = !state.queued_paths.empty();
    const bool all_enabled =
        has_entries && state.queued_enabled.size() >= state.queued_paths.size() &&
        std::all_of(state.queued_enabled.begin(),
                    state.queued_enabled.begin() + static_cast<std::ptrdiff_t>(state.queued_paths.size()),
                    [](bool enabled) { return enabled; });
    draw_checkbox(dc, header_columns.header_checkbox, L"", has_entries && all_enabled, has_entries);
    draw_text(dc, inset_rect(header_columns.name, scale(8), 0), L"Name", kMuted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw_text(dc, inset_rect(header_columns.size, scale(8), 0), L"Size", kMuted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw_text(dc, inset_rect(header_columns.type, scale(8), 0), L"Type", kMuted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw_text(dc, inset_rect(header_columns.path, scale(8), 0), L"Path", kMuted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    for (const auto& grip : header_columns.resize_grips) {
        draw_line(dc, (grip.left + grip.right) / 2, table.top + scale(8), (grip.left + grip.right) / 2,
                  header_bottom - scale(8), kBorder);
    }
    draw_line(dc, table.left + scale(1), header_bottom - scale(1), table.right - scale(1), header_bottom - scale(1),
              RGB(73, 95, 102));
    draw_line(dc, table.left + scale(1), header_bottom, table.right - scale(1), header_bottom, RGB(9, 15, 18));
}

// Purpose: Draw Queue empty-state drag/drop guidance.
// Inputs: `dc` is the paint target and `table` is the Queue table rectangle.
// Outputs: Renders centered muted or highlighted drop text without changing queue state.
void MainWindow::draw_queue_empty_state(HDC dc, const RECT& table) {
    SelectObject(dc, body_font_);
    const RECT empty_drop_zone{table.left + scale(40), table.top + scale(36), table.right - scale(40),
                               table.bottom - scale(36)};
    draw_text(dc, empty_drop_zone, L"Drag & drop files or folders here, or use the Add files / Add folder buttons.",
              queue_drop_highlight_ ? kText : kMuted,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

// Purpose: Draw the visible Queue body rows below the fixed header.
// Inputs: `dc` is the paint target, `table`/`columns_table` describe geometry, `state` is copied UI state, and
// `first_visible_row` is the first queued item to render.
// Outputs: Renders clipped row backgrounds, row checkboxes, and text columns.
void MainWindow::draw_queue_table_rows(HDC dc, const RECT& table, const RECT& columns_table, const UiState& state,
                                       int first_visible_row) {
    const int saved = SaveDC(dc);
    const int header_bottom = table.top + scale(kQueueHeaderHeight);
    const int row_height = scale(kQueueRowHeight);
    IntersectClipRect(dc, columns_table.left + scale(1), header_bottom, columns_table.right - scale(1),
                      table.bottom - scale(1));
    const int visible_rows = std::max(0, queue_visible_row_count(table));
    for (int visible_index = 0; visible_index < visible_rows; ++visible_index) {
        const int row_index = first_visible_row + visible_index;
        if (row_index >= static_cast<int>(state.queued_paths.size())) {
            break;
        }
        const auto& path = state.queued_paths[static_cast<std::size_t>(row_index)];
        const int y = header_bottom + (visible_index * row_height);
        const int row_bottom = y + row_height;
        const bool selected = row_index == state.selected_queue_index;
        const RECT row_rect{columns_table.left, y, columns_table.right, row_bottom};
        const auto columns = queue_column_layout(columns_table, row_rect);
        const bool enabled = static_cast<std::size_t>(row_index) >= state.queued_enabled.size()
                                 ? true
                                 : state.queued_enabled[static_cast<std::size_t>(row_index)];
        const COLORREF base_row_fill = selected ? kPanel3 : ((row_index % 2 == 0) ? kPanel2 : kPanel);
        const COLORREF row_fill = interactive_fill(base_row_fill, row_rect);
        fill_rect(dc, RECT{columns_table.left + scale(1), y + scale(1), columns_table.right - scale(1), row_bottom},
                  row_fill);
        draw_checkbox(dc, columns.checkbox, L"", enabled);
        draw_text(dc, inset_rect(columns.name, scale(8), 0), path.filename().wstring(), kText,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        draw_text(dc, inset_rect(columns.size, scale(8), 0), entry_size_text(path), kMuted,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        draw_text(dc, inset_rect(columns.type, scale(8), 0), entry_type_text(path), kMuted,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        draw_text(dc, inset_rect(columns.path, scale(8), 0), path.wstring(), kMuted,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    RestoreDC(dc, saved);
}

// Purpose: Draw the Queue overflow scrollbar when rows exceed visible capacity.
// Inputs: `dc` is the paint target, `table` is the full table rectangle, `row_count` is the queue size, and
// `max_scroll` is the largest valid first visible row.
// Outputs: Renders the body-only scrollbar track and thumb when overflow exists.
void MainWindow::draw_queue_scrollbar(HDC dc, const RECT& table, std::size_t row_count, int max_scroll) {
    if (max_scroll <= 0) {
        return;
    }
    const RECT track = queue_scrollbar_track_rect(table);
    const RECT thumb = queue_scrollbar_thumb_rect(table, row_count);
    fill_round_rect(dc, track, RGB(18, 28, 32), scale(5));
    const COLORREF thumb_fill = interactive_fill(RGB(64, 83, 90), thumb);
    fill_round_rect(dc, thumb, thumb_fill, scale(5));
}

// Purpose: Draw the History overflow scrollbar with the Queue scrollbar visual system.
// Inputs: `dc` is the paint target, `table` is the full table rectangle, `row_count` is the filtered row count, and
// `max_scroll` is the largest valid first visible row.
// Outputs: Renders the body-only scrollbar track and thumb when overflow exists.
void MainWindow::draw_history_scrollbar(HDC dc, const RECT& table, std::size_t row_count, int max_scroll) {
    if (max_scroll <= 0) {
        return;
    }
    const RECT track = history_scrollbar_track_rect(table);
    const RECT thumb = history_scrollbar_thumb_rect(table, row_count);
    fill_round_rect(dc, track, RGB(18, 28, 32), scale(5));
    const COLORREF thumb_fill = interactive_fill(RGB(64, 83, 90), thumb);
    fill_round_rect(dc, thumb, thumb_fill, scale(5));
}

// Purpose: Draw the queue page with the file/folder selection table only.
// Inputs: `dc` is the target, `rect` is the content area, and `state` is copied UI state.
// Outputs: Renders queue selection controls; operation configuration remains on later pages.
void MainWindow::draw_queue_page(HDC dc, const RECT& rect, const UiState& state) {
    const auto layout = queue_layout(rect);
    draw_queue_toolbar(dc, layout, state);

    const RECT table = layout.table;
    const COLORREF table_fill = queue_drop_highlight_ ? blend_color(kPanel, kInfo, 0.16) : kPanel;
    fill_round_rect(dc, table, table_fill, scale(4));
    stroke_rect(dc, table, queue_drop_highlight_ ? RGB(70, 116, 130) : kBorder);

    const int max_scroll = queue_max_scroll_offset(table, state.queued_paths.size());
    const int first_visible_row = std::clamp(queue_scroll_first_row_, 0, max_scroll);
    const RECT columns_table = queue_columns_table(table, state.queued_paths.size());
    draw_queue_table_header(dc, table, columns_table, state);

    if (state.queued_paths.empty()) {
        draw_queue_empty_state(dc, table);
        return;
    }
    draw_queue_table_rows(dc, table, columns_table, state, first_visible_row);
    draw_queue_scrollbar(dc, table, state.queued_paths.size(), max_scroll);
}

// Purpose: Draw the compression settings page.
// Inputs: `dc` is the target, `rect` is the content area, and `state` contains opt-in settings.
// Outputs: Renders archive destination, format, compression level, advanced options, integrity toggles, and start
// control.
void MainWindow::draw_compress_page(HDC dc, const RECT& rect, const UiState& state) {
    const auto layout = compress_layout(rect);
    const RECT area = layout.area;
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(kPageTitleTextHeight)}, L"Compress", kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_button(dc, layout.start, L"Start", true);
    draw_operation_progress_bar(dc,
                                RECT{layout.start.left, layout.start.bottom + scale(4), layout.start.right,
                                     layout.start.bottom + scale(4) + scale(kOperationProgressHeight)},
                                state, OperationKind::Compress);

    const auto format = compression_format_value(state.compression_format_index);
    const bool suzip_tuning = compression_format_uses_suzip_tuning(format);
    const bool level_tuning = compression_format_uses_level(format);
    draw_field(dc, layout.archive_name, L"Archive name", compression_output_filename_for(state), false);
    draw_field(dc, layout.destination, L"Destination", destination_directory_or_default(state).wstring(), false, true,
               true);
    draw_field(dc, layout.format, L"Format", compression_format_text(state.compression_format_index), true);
    draw_field(dc, layout.compression_level, L"Compression level",
               level_tuning ? compression_level_text(state.compression_level_index) : L"-", level_tuning, level_tuning);
    draw_field(dc, layout.method, L"Compression method",
               suzip_tuning ? (state.gpu_required ? L"AMD HIP required" : L"AMD HIP preferred") : L"-", suzip_tuning,
               suzip_tuning);
    draw_field(dc, layout.block_size, L"Block size",
               suzip_tuning ? compression_block_size_text(state.compression_block_size_index) : L"-", suzip_tuning,
               suzip_tuning);

    RECT advanced = layout.advanced;
    fill_round_rect(dc, advanced, kPanel, scale(4));
    stroke_rect(dc, advanced, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{advanced.left + scale(16), advanced.top + scale(12), advanced.right, advanced.top + scale(36)},
              L"Advanced", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_toggle(dc, layout.solid_archive, L"Solid archive", state.solid_archive, ToggleId::SolidArchive);
    draw_toggle(dc, layout.store_timestamps, L"Store timestamps", state.store_timestamps, ToggleId::StoreTimestamps);
    draw_toggle(dc, layout.delete_after_compression, L"Delete files after compression", state.delete_after_compression,
                ToggleId::DeleteAfterCompression);
    draw_toggle(dc, layout.verify, L"Verify archive after write", state.verify_after_write_opt_in,
                ToggleId::VerifyAfterWrite);

    RECT security = layout.security;
    fill_round_rect(dc, security, kPanel, scale(4));
    stroke_rect(dc, security, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{security.left + scale(16), security.top + scale(12), security.right, security.top + scale(36)},
              L"Integrity and Security", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_toggle(dc, layout.sha, L"SHA-256 integrity check", state.integrity_hash_opt_in, ToggleId::IntegrityHash);
    draw_toggle(dc, layout.defender, L"Microsoft Defender scan", state.defender_scan_opt_in, ToggleId::DefenderScan);
}

// Purpose: Draw the extraction settings page.
// Inputs: `dc` is the target, `rect` is the content area, and `state` contains opt-in settings.
// Outputs: Renders archive/destination fields, overwrite policy, integrity toggles, and start control.
void MainWindow::draw_extract_page(HDC dc, const RECT& rect, const UiState& state) {
    const auto layout = extract_layout(rect);
    const RECT area = layout.area;
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(kPageTitleTextHeight)}, L"Extract", kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_button(dc, layout.start, L"Start", true);
    draw_operation_progress_bar(dc,
                                RECT{layout.start.left, layout.start.bottom + scale(4), layout.start.right,
                                     layout.start.bottom + scale(4) + scale(kOperationProgressHeight)},
                                state, OperationKind::Extract);

    const auto selected_archives = selected_extract_archive_paths(state);
    const auto archive = selected_extract_archive_text(selected_archives);
    draw_field(dc, layout.archive, L"Archive path", archive, false, true, false,
               selected_archives.empty() ? kMuted : CLR_INVALID);
    draw_field(dc, layout.destination, L"Destination", extraction_output_path_for(state).wstring(), false, true, true);
    const std::wstring detected_format = selected_extract_archive_format_text(selected_archives);
    draw_field(dc, layout.path_mode, L"Format", detected_format, false, detected_format != L"-");
    draw_field(dc, layout.overwrite_policy, L"Overwrite policy",
               state.overwrite ? L"Overwrite without asking" : L"Ask before overwriting", true);

    RECT checks = layout.checks;
    fill_round_rect(dc, checks, kPanel, scale(4));
    stroke_rect(dc, checks, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{checks.left + scale(16), checks.top + scale(12), checks.right, checks.top + scale(36)},
              L"Integrity and Security", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_toggle(dc, layout.verify_metadata, L"Verify archive metadata before extraction",
                state.verify_metadata_before_extract, ToggleId::VerifyMetadata);
    draw_toggle(dc, layout.open_destination_after_extract, L"Open destination folder after extraction",
                state.open_destination_after_extract, ToggleId::OpenDestinationAfterExtract);
    draw_toggle(dc, layout.sha, L"SHA-256 integrity check", state.integrity_hash_opt_in, ToggleId::IntegrityHash);
    draw_toggle(dc, layout.defender, L"Microsoft Defender scan", state.defender_scan_opt_in, ToggleId::DefenderScan);
}

// Purpose: Draw the security review page.
// Inputs: `dc` is the target, `rect` is the content area, and `state` contains security choices.
// Outputs: Renders path, CRC, integrity, Defender, and overwrite checks with explicit status.
void MainWindow::draw_security_page(HDC dc, const RECT& rect, const UiState& state) {
    RECT area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(kPageTitleTextHeight)}, L"Security", kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const RECT verify_button = primary_action_rect(area);
    draw_button(dc, verify_button, L"Verify", true);
    draw_operation_progress_bar(dc,
                                RECT{verify_button.left, verify_button.bottom + scale(4), verify_button.right,
                                     verify_button.bottom + scale(4) + scale(kOperationProgressHeight)},
                                state, OperationKind::Verify);

    RECT summary{area.left, area.top + scale(54), area.left + scale(430), area.bottom - scale(80)};
    fill_round_rect(dc, summary, kPanel, scale(4));
    stroke_rect(dc, summary, kBorder);
    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{summary.left + scale(16), summary.top, summary.left + scale(210), summary.top + scale(36)},
              L"Check", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{summary.left + scale(230), summary.top, summary.right - scale(16), summary.top + scale(36)},
              L"Status", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_line(dc, summary.left, summary.top + scale(36), summary.right, summary.top + scale(36), kBorder);
    struct Row {
        const wchar_t* name;
        const wchar_t* status;
        COLORREF color;
    };
    const Row rows[] = {
        {L"Path safety", L"Safe", kOk},
        {L"CRC metadata", L"Verified", kOk},
        {L"Post-write verify", state.verify_after_write_opt_in ? L"Selected" : L"Not selected",
         state.verify_after_write_opt_in ? kOk : kWarn},
        {L"SHA-256 optional", state.integrity_hash_opt_in ? L"Selected" : L"Not selected",
         state.integrity_hash_opt_in ? kOk : kWarn},
        {L"Defender optional", state.defender_scan_opt_in ? L"Selected" : L"Not selected",
         state.defender_scan_opt_in ? kOk : kWarn},
        {L"Overwrite policy", state.overwrite ? L"Overwrite without asking" : L"Ask before overwriting",
         state.overwrite ? kWarn : kOk},
        {L"GPU requirement", state.gpu_required ? L"AMD HIP required" : L"Fallback allowed",
         state.gpu_required ? kInfo : kWarn},
    };
    int y = summary.top + scale(36);
    for (const auto& row : rows) {
        const int bottom = y + scale(38);
        fill_rect(dc, RECT{summary.left + scale(1), y + scale(1), summary.right - scale(1), bottom},
                  ((y / scale(38)) % 2 == 0) ? kPanel2 : kPanel);
        draw_text(dc, RECT{summary.left + scale(16), y, summary.left + scale(210), bottom}, row.name, kText,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        draw_text(dc, RECT{summary.left + scale(230), y, summary.right - scale(16), bottom}, row.status, row.color,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y = bottom;
    }

    RECT detail{summary.right + scale(22), summary.top, area.right, summary.bottom};
    fill_round_rect(dc, detail, kPanel, scale(4));
    stroke_rect(dc, detail, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{detail.left + scale(18), detail.top + scale(14), detail.right, detail.top + scale(40)},
              L"Archive boundary contract", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_text(
        dc, RECT{detail.left + scale(18), detail.top + scale(56), detail.right - scale(18), detail.top + scale(154)},
        L"Extraction publishes final files only after each archive entry is normalized, decoded, and verified inside "
        L"the selected destination. Absolute paths, drive-rooted paths, UNC paths, traversal, unsafe names, malformed "
        L"block metadata, CRC mismatches, and overwrite attempts are rejected before final publication.",
        kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    draw_field(
        dc, RECT{detail.left + scale(18), detail.top + scale(184), detail.right - scale(18), detail.top + scale(234)},
        L"Archive",
        state.queued_paths.empty() ? L"Select one or more archives from the queue"
                                   : state.queued_paths.front().wstring(),
        false, true, false, state.queued_paths.empty() ? kMuted : CLR_INVALID);
    draw_field(
        dc, RECT{detail.left + scale(18), detail.top + scale(252), detail.left + scale(248), detail.top + scale(302)},
        L"Files", std::to_wstring(state.queued_paths.size()), false);
    draw_field(
        dc, RECT{detail.left + scale(270), detail.top + scale(252), detail.right - scale(18), detail.top + scale(302)},
        L"Total size", L"Calculated during job", false);
}

// Purpose: Draw the operation history page.
// Inputs: `dc` is the target, `rect` is the content area, and `state` contains session history.
// Outputs: Renders filters, history rows, and selected-operation details.
void MainWindow::draw_history_page(HDC dc, const RECT& rect, const UiState& state) {
    RECT area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(kPageTitleTextHeight)}, L"History", kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_button(dc, history_clear_button_rect(area), L"Clear History", false);
    draw_field(dc, RECT{area.left, area.top + scale(48), area.left + scale(220), area.top + scale(92)}, L"Operation",
               history_operation_filter_text(state.history_operation_filter_index), true);
    draw_field(dc, RECT{area.left + scale(238), area.top + scale(48), area.left + scale(458), area.top + scale(92)},
               L"Status", history_status_filter_text(state.history_status_filter_index), true);

    RECT table{area.left, area.top + scale(112), area.right, area.bottom - scale(96)};
    fill_round_rect(dc, table, kPanel, scale(4));
    stroke_rect(dc, table, kBorder);
    SelectObject(dc, tiny_font_);
    const int header_bottom = table.top + scale(kQueueHeaderHeight);
    const RECT header_row{table.left, table.top, table.right, header_bottom};
    const RECT header_band{table.left + scale(1), table.top + scale(1), table.right - scale(1), header_bottom};
    fill_rect(dc, header_band, blend_color(kPanel, kPanel2, 0.52));
    const auto visible_indices = filtered_history_indices(state);
    clamp_history_scroll_offset(table, visible_indices.size());
    const int max_scroll = history_max_scroll_offset(table, visible_indices.size());
    const int first_visible_row = std::clamp(history_scroll_first_row_, 0, max_scroll);
    const RECT columns_table = history_columns_table(table, visible_indices.size());
    const auto header_columns = history_column_layout(columns_table, header_row);
    draw_text(dc, inset_rect(header_columns.time, scale(8), 0), L"Time", kMuted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw_text(dc, inset_rect(header_columns.operation, scale(8), 0), L"Operation", kMuted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw_text(dc, inset_rect(header_columns.archive_name, scale(8), 0), L"Archive name", kMuted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw_text(dc, inset_rect(header_columns.archive_path, scale(8), 0), L"Archive path", kMuted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw_text(dc, inset_rect(header_columns.status, scale(8), 0), L"Status", kMuted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    for (const auto& grip : header_columns.resize_grips) {
        draw_line(dc, (grip.left + grip.right) / 2, table.top + scale(8), (grip.left + grip.right) / 2,
                  header_bottom - scale(8), kBorder);
    }
    draw_line(dc, table.left + scale(1), header_bottom - scale(1), table.right - scale(1), header_bottom - scale(1),
              RGB(73, 95, 102));
    draw_line(dc, table.left + scale(1), header_bottom, table.right - scale(1), header_bottom, RGB(9, 15, 18));
    int y = header_bottom;
    if (state.history.empty()) {
        draw_text(dc, RECT{table.left + scale(18), y + scale(10), table.right - scale(18), y + scale(52)},
                  L"No completed operations in this session yet.", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else {
        if (visible_indices.empty()) {
            draw_text(dc, RECT{table.left + scale(18), y + scale(10), table.right - scale(18), y + scale(52)},
                      L"No operations match the current filters.", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, columns_table.left + scale(1), header_bottom, columns_table.right - scale(1),
                          table.bottom - scale(1));
        const int visible_rows = history_visible_row_count(table);
        for (int visible_row = 0; visible_row < visible_rows; ++visible_row) {
            const int filtered_row = first_visible_row + visible_row;
            if (filtered_row < 0 || filtered_row >= static_cast<int>(visible_indices.size())) {
                break;
            }
            const auto history_index = visible_indices[static_cast<std::size_t>(filtered_row)];
            const auto& entry = state.history[history_index];
            const int bottom = y + scale(34);
            const RECT row{columns_table.left, y, columns_table.right, bottom};
            const auto columns = history_column_layout(columns_table, row);
            const bool selected = static_cast<int>(history_index) == state.selected_history_index;
            const COLORREF base_row_fill = selected ? kPanel3 : ((filtered_row % 2 == 0) ? kPanel2 : kPanel);
            fill_rect(dc, RECT{columns_table.left + scale(1), y + scale(1), columns_table.right - scale(1), bottom},
                      interactive_fill(base_row_fill, row, true));
            draw_text(dc, inset_rect(columns.time, scale(8), 0), local_time_text(entry.timestamp), kMuted,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            draw_text(dc, inset_rect(columns.operation, scale(8), 0), widen(entry.operation), kText,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            draw_text(dc, inset_rect(columns.archive_name, scale(8), 0), widen(entry.archive_name), kMuted,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            draw_text(dc, inset_rect(columns.archive_path, scale(8), 0), widen(entry.archive_path), kMuted,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            draw_text(dc, inset_rect(columns.status, scale(8), 0), entry.success ? L"Success" : L"Failure",
                      entry.success ? kOk : kDanger, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y = bottom;
        }
        RestoreDC(dc, saved);
        draw_history_scrollbar(dc, table, visible_indices.size(), max_scroll);
    }
    RECT details{area.left, area.bottom - scale(76), area.right, area.bottom};
    fill_round_rect(dc, details, kPanel, scale(4));
    stroke_rect(dc, details, kBorder);
    const bool selected_valid =
        state.selected_history_index >= 0 && state.selected_history_index < static_cast<int>(state.history.size()) &&
        history_entry_matches_filters(state, state.history[static_cast<std::size_t>(state.selected_history_index)]);
    if (!selected_valid) {
        draw_text(dc,
                  RECT{details.left + scale(18), details.top + scale(10), details.right - scale(18), details.bottom},
                  L"Selected operation details will appear here", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else {
        const auto& entry = state.history[static_cast<std::size_t>(state.selected_history_index)];
        std::wstring detail_text = std::wstring(L"Time: ") + local_time_text(entry.timestamp) + L"    Operation: " +
                                   widen(entry.operation) + L"    Status: " +
                                   (entry.success ? L"Success" : L"Failure") + L"\nArchive name: " +
                                   widen(entry.archive_name) + L"\nArchive path: " + widen(entry.archive_path) +
                                   L"\nDetails: " + widen(entry.detail);
        draw_text(dc,
                  RECT{details.left + scale(18), details.top + scale(10), details.right - scale(18),
                       details.bottom - scale(8)},
                  detail_text, kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    }
}

// Purpose: Draw the System page with current runtime and resource-status details.
// Inputs: `dc` is the double-buffered paint target, `rect` is the page bounds, and `state` supplies live GPU/status
// text. Outputs: Renders system diagnostics controls and informational panels without mutating state.
void MainWindow::draw_gpu_page(HDC dc, const RECT& rect, const UiState& state) {
    RECT area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(kPageTitleTextHeight)}, L"System", kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const bool ready = gpu_ready(state);
    RECT top{area.left, area.top + scale(54), area.right, area.top + scale(142)};
    fill_round_rect(dc, top, kPanel, scale(4));
    stroke_rect(dc, top, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{top.left + scale(18), top.top + scale(12), top.left + scale(180), top.top + scale(40)},
              L"HIP status", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{top.left + scale(190), top.top + scale(12), top.right - scale(18), top.top + scale(40)},
              widen(state.gpu_status), ready ? kOk : kWarn, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{top.left + scale(18), top.top + scale(50), top.right - scale(18), top.bottom - scale(12)},
              ready ? L"SuperZip will use AMD HIP for native .suzip jobs that require GPU acceleration."
                    : L"This build or host is not reporting an active AMD HIP device. GPU-required jobs fail instead "
                      L"of silently using a different vendor path.",
              kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);

    const int card_w = (area.right - area.left - scale(28)) / 3;
    RECT gpu{area.left, area.top + scale(166), area.left + card_w, area.top + scale(316)};
    RECT memory{gpu.right + scale(14), gpu.top, gpu.right + scale(14) + card_w, gpu.bottom};
    RECT accel{memory.right + scale(14), gpu.top, area.right, gpu.bottom};
    fill_round_rect(dc, gpu, kPanel, scale(4));
    fill_round_rect(dc, memory, kPanel, scale(4));
    fill_round_rect(dc, accel, kPanel, scale(4));
    stroke_rect(dc, gpu, kBorder);
    stroke_rect(dc, memory, kBorder);
    stroke_rect(dc, accel, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{gpu.left + scale(16), gpu.top + scale(12), gpu.right, gpu.top + scale(36)}, L"GPU", kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{memory.left + scale(16), memory.top + scale(12), memory.right, memory.top + scale(36)}, L"RAM",
              kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{accel.left + scale(16), accel.top + scale(12), accel.right, accel.top + scale(36)},
              L"Acceleration", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    const std::wstring gpu_detail =
        ready ? (std::wstring(L"Backend: AMD HIP\nDevice: ") +
                 (state.gpu_device_name.empty() ? L"Detected by HIP runtime" : widen(state.gpu_device_name)) +
                 L"\nArchitecture: " + (state.gpu_arch.empty() ? L"Runtime default" : widen(state.gpu_arch)))
              : L"Backend unavailable\nNo CUDA/WebGPU fallback\nHost stays AMD-only";
    draw_text(dc, RECT{gpu.left + scale(16), gpu.top + scale(48), gpu.right - scale(16), gpu.bottom - scale(14)},
              gpu_detail, kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    draw_text(
        dc, RECT{memory.left + scale(16), memory.top + scale(48), memory.right - scale(16), memory.bottom - scale(14)},
        L"Bounded chunks keep archive work from loading whole archives into RAM. Archive and compression adapters "
        L"cover ZIP, TAR filters, Gzip, Bzip2, Zstandard, Unix Compress, CAB, RPM, XZ, and LZMA; legacy transfer "
        L"decoders remain extract-only and path-validated.",
        kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    draw_text(dc,
              RECT{accel.left + scale(16), accel.top + scale(48), accel.right - scale(16), accel.bottom - scale(14)},
              state.gpu_required ? L"Mode: GPU required\nFallback: blocked for .suzip jobs\nDevice scope: AMD HIP only"
                                 : L"Mode: GPU preferred\nFallback: CPU codec allowed\nDevice scope: AMD HIP only",
              kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);

    draw_performance_monitor(dc, RECT{area.left, area.top + scale(342), area.right, area.bottom}, state);
}

// Purpose: Draw one metric card inside the live performance monitor.
// Inputs: `dc` is the target; text/value fields are preformatted; `history` contains normalized samples.
// Outputs: Renders a bordered Task Manager-style history graph without overflowing text.
void MainWindow::draw_performance_monitor_card(HDC dc, const RECT& graph, const wchar_t* label,
                                               const std::wstring& value, const std::wstring& detail,
                                               std::span<const double> history, COLORREF color,
                                               const std::wstring& graph_top_label,
                                               const std::wstring& graph_bottom_label) {
    fill_round_rect(dc, graph, kPanel2, scale(4));
    stroke_rect(dc, graph, kBorder);
    RECT label_rect{graph.left + scale(12), graph.top + scale(8), graph.right - scale(12), graph.top + scale(30)};
    RECT value_rect{graph.left + scale(12), graph.top + scale(30), graph.right - scale(12), graph.top + scale(60)};
    RECT plot{graph.left + scale(12), graph.top + scale(70), graph.right - scale(12), graph.bottom - scale(58)};
    RECT detail_rect{graph.left + scale(12), graph.bottom - scale(50), graph.right - scale(12),
                     graph.bottom - scale(8)};
    SelectObject(dc, small_font_);
    draw_text(dc, label_rect, label, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, body_font_);
    draw_text(dc, value_rect, value, color, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, tiny_font_);
    draw_graph_grid(dc, plot);
    const RECT graph_area = inset_rect(plot, scale(1), scale(1));
    draw_graph_series(dc, graph_area, history, color);
    draw_graph_axis_labels(dc, graph_area, graph_top_label, graph_bottom_label);
    draw_text(dc, detail_rect, detail, kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
}

// Purpose: Draw one paired metric card inside the live performance monitor.
// Inputs: `dc` is the target; primary and secondary histories are normalized samples.
// Outputs: Renders a shared-grid dual-line history graph without overflowing text.
void MainWindow::draw_dual_performance_monitor_card(HDC dc, const RECT& graph, const wchar_t* label,
                                                    const std::wstring& value, const std::wstring& detail,
                                                    std::span<const double> primary_history,
                                                    std::span<const double> secondary_history, COLORREF primary,
                                                    COLORREF secondary, const std::wstring& graph_top_label,
                                                    const std::wstring& graph_bottom_label) {
    fill_round_rect(dc, graph, kPanel2, scale(4));
    stroke_rect(dc, graph, kBorder);
    RECT label_rect{graph.left + scale(12), graph.top + scale(8), graph.right - scale(12), graph.top + scale(30)};
    RECT value_rect{graph.left + scale(12), graph.top + scale(30), graph.right - scale(12), graph.top + scale(60)};
    RECT plot{graph.left + scale(12), graph.top + scale(70), graph.right - scale(12), graph.bottom - scale(58)};
    RECT detail_rect{graph.left + scale(12), graph.bottom - scale(50), graph.right - scale(12),
                     graph.bottom - scale(8)};
    SelectObject(dc, small_font_);
    draw_text(dc, label_rect, label, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, body_font_);
    draw_text(dc, value_rect, value, kWarn, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, tiny_font_);
    draw_graph_grid(dc, plot);
    const RECT inset_plot = inset_rect(plot, scale(1), scale(1));
    draw_graph_series(dc, inset_plot, primary_history, primary);
    draw_graph_series(dc, inset_plot, secondary_history, secondary);
    draw_graph_axis_labels(dc, inset_plot, graph_top_label, graph_bottom_label);
    draw_text(dc, detail_rect, detail, kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
}

// Purpose: Return the four live-monitor card rectangles shared by rendering and hit testing.
// Inputs: `monitor` is the complete Performance Monitor panel.
// Outputs: Returns CPU, RAM, I/O, and GPU card rectangles in display order.
std::array<RECT, 4> MainWindow::performance_monitor_card_rects(const RECT& monitor) const {
    const int graph_top = monitor.top + scale(58);
    const int graph_bottom = monitor.bottom - scale(18);
    const int graph_w = (monitor.right - monitor.left - scale(86)) / 4;
    std::array<RECT, 4> cards{};
    for (int i = 0; i < 4; ++i) {
        cards[static_cast<std::size_t>(i)] =
            RECT{monitor.left + scale(18) + i * (graph_w + scale(16)), graph_top,
                 monitor.left + scale(18) + i * (graph_w + scale(16)) + graph_w, graph_bottom};
    }
    return cards;
}

// Purpose: Return the System refresh-interval field rectangle aligned to the GPU card.
// Inputs: `monitor` is the complete Performance Monitor panel.
// Outputs: Returns a narrow same-row field rectangle whose right edge matches the GPU card.
RECT MainWindow::performance_update_speed_rect(const RECT& monitor) const {
    const auto cards = performance_monitor_card_rects(monitor);
    const RECT gpu_card = cards.back();
    const int box_w = scale(kPerformanceUpdateFieldWidth);
    return RECT{gpu_card.right - box_w, monitor.top + scale(10), gpu_card.right, monitor.top + scale(40)};
}

// Purpose: Return the fixed-drive selector rectangle inside the I/O monitor card.
// Inputs: `monitor` is the Performance Monitor panel rectangle.
// Outputs: Returns a compact same-row dropdown aligned to the I/O card header.
RECT MainWindow::performance_io_drive_rect(const RECT& monitor) const {
    const auto cards = performance_monitor_card_rects(monitor);
    const RECT io_card = cards[2];
    const int box_w = scale(64);
    return RECT{io_card.right - scale(12) - box_w, io_card.top - scale(12), io_card.right - scale(12),
                io_card.top + scale(30)};
}

// Purpose: Draw the live performance monitor section on the System page.
// Inputs: `dc` is the target, `monitor` is the panel rectangle, and `state` contains the latest counters.
// Outputs: Renders CPU, RAM, selected-drive total I/O, and total GPU utilization history cards.
void MainWindow::draw_performance_monitor(HDC dc, const RECT& monitor, const UiState& state) {
    const auto& sample = state.performance;
    fill_round_rect(dc, monitor, kPanel, scale(4));
    stroke_rect(dc, monitor, kBorder);
    SelectObject(dc, small_font_);
    const RECT update_speed = performance_update_speed_rect(monitor);
    draw_text(
        dc,
        RECT{monitor.left + scale(16), monitor.top + scale(12), update_speed.left - scale(18), monitor.top + scale(36)},
        L"Performance Monitor", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_text(
        dc, RECT{update_speed.left - scale(116), update_speed.top, update_speed.left - scale(10), update_speed.bottom},
        L"Refresh interval", kMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    draw_field(dc, RECT{update_speed.left, update_speed.top - scale(20), update_speed.right, update_speed.bottom}, L"",
               performance_update_speed_text(state.performance_update_seconds), true);

    std::array<double, 96> cpu{};
    std::array<double, 96> memory{};
    std::array<double, 96> io_busy{};
    std::array<double, 96> gpu{};
    const auto sample_at = [this](std::size_t index) -> const PerformanceMonitorSample& {
        const auto start = performance_history_next_ + performance_history_.size() - performance_history_count_;
        return performance_history_[(start + index) % performance_history_.size()];
    };
    for (std::size_t i = 0; i < performance_history_count_; ++i) {
        const auto& item = sample_at(i);
        cpu[i] = item.cpu_percent / 100.0;
        memory[i] = item.system_memory_percent / 100.0;
        io_busy[i] = item.io_busy_percent / 100.0;
        gpu[i] = item.gpu_utilization_available ? item.gpu_utilization_percent / 100.0 : 0.0;
    }

    const auto cards = performance_monitor_card_rects(monitor);
    const std::span<const double> cpu_span(cpu.data(), performance_history_count_);
    const std::span<const double> memory_span(memory.data(), performance_history_count_);
    const std::span<const double> io_span(io_busy.data(), performance_history_count_);
    const std::span<const double> gpu_span(gpu.data(), performance_history_count_);
    const std::wstring percent_top = L"100%";
    const std::wstring zero_percent = L"0%";
    const auto drives_available = !fixed_io_drive_options().empty();
    for (int i = 0; i < 4; ++i) {
        RECT card = cards[static_cast<std::size_t>(i)];
        if (!sample.live) {
            draw_performance_monitor_card(dc, card,
                                          i == 0   ? L"CPU"
                                          : i == 1 ? L"RAM"
                                          : i == 2 ? L"I/O"
                                                   : L"GPU",
                                          L"Collecting", L"Waiting for first sample", std::span<const double>{},
                                          kSubtle, percent_top, zero_percent);
            if (i == 2) {
                draw_field(dc, performance_io_drive_rect(monitor), L"", io_drive_option_text(state.io_drive_index),
                           drives_available, drives_available);
            }
        } else if (i == 0) {
            const auto detail = std::wstring(L"CPU used (total): ") + percentage_text(sample.cpu_percent) +
                                L"\nCPU used (dedicated): " + percentage_text(sample.process_cpu_percent);
            draw_performance_monitor_card(dc, card, L"CPU", percentage_text(sample.cpu_percent), detail, cpu_span,
                                          kInfo, percent_top, zero_percent);
        } else if (i == 1) {
            const auto value = percentage_text(sample.system_memory_percent);
            const auto detail = std::wstring(L"RAM used (total): ") +
                                widen(human_bytes(static_cast<double>(sample.system_memory_used_bytes))) + L" / " +
                                widen(human_bytes(static_cast<double>(sample.system_memory_total_bytes))) +
                                L"\nRAM used (dedicated): " +
                                widen(human_bytes(static_cast<double>(sample.private_bytes)));
            draw_performance_monitor_card(dc, card, L"RAM", value, detail, memory_span, kOk, percent_top, zero_percent);
        } else if (i == 2) {
            const auto detail = std::wstring(L"Read: ") + rate_text(sample.io_read_bytes_per_second) + L"\nWrite: " +
                                rate_text(sample.io_write_bytes_per_second);
            const auto value = sample.live ? percentage_text(sample.io_busy_percent) : L"Unavailable";
            draw_performance_monitor_card(dc, card, L"I/O", value, detail, io_span, kWarn, percent_top, zero_percent);
            draw_field(dc, performance_io_drive_rect(monitor), L"", io_drive_option_text(state.io_drive_index),
                       drives_available, drives_available);
        } else {
            const bool has_vram = sample.vram_total_bytes > 0U;
            const auto used = sample.vram_total_bytes - sample.vram_free_bytes;
            const auto value =
                sample.gpu_utilization_available ? percentage_text(sample.gpu_utilization_percent) : L"Unavailable";
            const auto detail =
                has_vram ? std::wstring(L"VRAM used (total): ") + widen(human_bytes(static_cast<double>(used))) +
                               L" / " + widen(human_bytes(static_cast<double>(sample.vram_total_bytes))) +
                               L"\nVRAM used (dedicated): " +
                               widen(human_bytes(static_cast<double>(sample.process_dedicated_vram_bytes)))
                         : L"HIP VRAM unavailable\nGPU memory counter unavailable";
            draw_performance_monitor_card(dc, card, L"GPU", value, detail, gpu_span, kAccent, percent_top,
                                          zero_percent);
        }
    }
}

// Purpose: Draw the preferences page.
// Inputs: `dc` is the target, `rect` is the content area, and `state` contains toggled defaults.
// Outputs: Renders general, security, performance, and logging settings.
void MainWindow::draw_settings_page(HDC dc, const RECT& rect, const UiState& state) {
    const auto layout = settings_layout(rect);
    RECT area = layout.area;
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(kPageTitleTextHeight)}, L"Settings", kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_button(dc, layout.restore_defaults, L"Restore Defaults", false);
    draw_button(dc, layout.apply, L"Apply", true);

    RECT general = layout.general;
    RECT security = layout.security;
    RECT performance = layout.performance;
    RECT logging = layout.logging;
    for (RECT panel : {general, security, performance, logging}) {
        fill_round_rect(dc, panel, kPanel, scale(4));
        stroke_rect(dc, panel, kBorder);
    }
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{general.left + scale(16), general.top + scale(12), general.right, general.top + scale(36)},
              L"General", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{security.left + scale(16), security.top + scale(12), security.right, security.top + scale(36)},
              L"Security", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(
        dc,
        RECT{performance.left + scale(16), performance.top + scale(12), performance.right, performance.top + scale(36)},
        L"Performance", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{logging.left + scale(16), logging.top + scale(12), logging.right, logging.top + scale(36)},
              L"Logging", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_toggle(dc, layout.open_destination_after_operation, L"Open destination folder after operation",
                state.open_destination_after_operation, ToggleId::OpenDestinationAfterOperation);
    draw_toggle(dc, layout.confirm_before_deleting, L"Confirm before deleting files", state.confirm_before_deleting,
                ToggleId::ConfirmBeforeDeleting);
    draw_toggle(dc, layout.show_operation_summary, L"Show operation summary", state.show_operation_summary,
                ToggleId::ShowOperationSummary);
    draw_toggle(dc, layout.sha, L"SHA-256 integrity check", state.integrity_hash_opt_in, ToggleId::IntegrityHash);
    draw_toggle(dc, layout.defender, L"Microsoft Defender scan", state.defender_scan_opt_in, ToggleId::DefenderScan);
    draw_toggle(dc, layout.gpu, L"Require AMD GPU acceleration", state.gpu_required, ToggleId::GpuRequired);
    draw_toggle(dc, layout.verify, L"Verify archive after write", state.verify_after_write_opt_in,
                ToggleId::VerifyAfterWrite);
    draw_field(dc, layout.memory_policy, L"Memory policy", memory_policy_text(state.memory_policy_index), true);
    draw_field(dc, layout.log_level, L"Log level", log_level_text(state.log_level_index), true);
    draw_field(dc, layout.log_retention, L"Log retention", log_retention_text(state.log_retention_index), true);
    draw_button(dc, layout.open_log_file, L"Open log file", false);
}

// Purpose: Render the About page brand, version, and compatibility boundary summary.
// Inputs: `dc` is the target device context and `rect` is the DPI-scaled page bounds.
// Outputs: Draws into `dc`; does not mutate archive state or perform I/O.
void MainWindow::draw_about_page(HDC dc, const RECT& rect) {
    RECT area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(kPageTitleTextHeight)}, L"About", kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const RECT licenses_button = about_licenses_button_rect(area);
    RECT card{area.left, area.top + scale(54), area.right, area.bottom - scale(60)};
    fill_round_rect(dc, card, kPanel, scale(4));
    stroke_rect(dc, card, kBorder);
    RECT logo{card.left + scale(42), card.top + scale(56), card.left + scale(112), card.top + scale(126)};
    draw_logo(dc, logo, kAccent);
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{card.left + scale(142), card.top + scale(50), card.right - scale(40), card.top + scale(90)},
              L"SuperZip", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{card.left + scale(142), card.top + scale(94), card.right - scale(40), card.top + scale(122)},
              std::wstring(kProductTagline), kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{card.left + scale(142), card.top + scale(130), card.right - scale(40), card.top + scale(150)},
              L"Author: Efstratios Mitridis", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{card.left + scale(142), card.top + scale(146), card.right - scale(40), card.top + scale(166)},
              widen(std::string("Version: ") + SUPERZIP_VERSION), kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(
        dc, RECT{card.left + scale(42), card.top + scale(184), card.right - scale(42), card.top + scale(294)},
        L"SuperZip separates native .suzip GPU archive jobs from ZIP, TAR, compressed TAR/CPIO, Gzip, Bzip2, XZ, "
        L"LZMA, lzip, Zstandard, Unix Compress, CAB, 7z, LHA/LZH, CPIO, AR, DEB, ISO, and RPM standard "
        L"archive/compression handling. Legacy transfer decoders remain extract-only. AMD HIP is the only GPU "
        L"acceleration boundary; security-sensitive extraction validates paths and metadata before writing output.",
        kText, DT_LEFT | DT_TOP | DT_WORDBREAK);
    draw_button(dc, licenses_button, L"Licenses", false);
    draw_text(
        dc,
        RECT{card.left + scale(42), card.bottom - scale(80), licenses_button.left - scale(18), card.bottom - scale(38)},
        L"Built for 64-bit Windows, high-DPI displays, and responsive background archive work.", kMuted,
        DT_LEFT | DT_TOP | DT_WORDBREAK);
}

// Purpose: Draw a simple DPI-scaled command or navigation button.
// Inputs: `dc` is the target, `rect` is the button rectangle, `text` is display text, and `active` selects accent
// styling. Outputs: Renders hover, press, release, border, and ellipsized text states.

}  // namespace superzip::app
