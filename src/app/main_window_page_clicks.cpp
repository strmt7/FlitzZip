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

// Purpose: Handle Queue page hit-testing and commands.
// Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
// Outputs: Returns true when a Queue control consumed the click.
bool MainWindow::handle_queue_click(const RECT& content, int x, int y) {
    const auto layout = queue_layout(content);
    if (handle_active_dropdown_click(x, y)) {
        return true;
    }
    UiState state;
    {
        std::lock_guard lock(mutex_);
        state = state_;
    }
    if (has_selected_queue_items(state) && contains_point(layout.remove_selected, x, y)) {
        return remove_selected_queue_items();
    }
    if (contains_point(layout.add_files, x, y)) {
        add_files();
        return true;
    }
    if (contains_point(layout.add_folder, x, y)) {
        add_folder();
        return true;
    }
    if (contains_point(layout.clear, x, y)) {
        clear_queue();
        return true;
    }
    clamp_queue_scroll_offset(layout.table, state.queued_paths.size());
    const RECT columns_table = queue_columns_table(layout.table, state.queued_paths.size());
    const int max_scroll = queue_max_scroll_offset(layout.table, state.queued_paths.size());
    const int header_bottom = layout.table.top + scale(kQueueHeaderHeight);
    const int row_height = scale(kQueueRowHeight);
    if (y >= layout.table.top && y < header_bottom) {
        const RECT header_row{layout.table.left, layout.table.top, layout.table.right, header_bottom};
        const auto columns = queue_column_layout(columns_table, header_row);
        for (int separator = 0; separator < static_cast<int>(columns.resize_grips.size()); ++separator) {
            if (contains_point(columns.resize_grips[static_cast<std::size_t>(separator)], x, y)) {
                begin_queue_column_resize(separator, x);
                return true;
            }
        }
        if (contains_point(columns.header_checkbox, x, y)) {
            return toggle_all_queue_items();
        }
        return true;
    }
    if (max_scroll > 0) {
        const RECT track = queue_scrollbar_track_rect(layout.table);
        const RECT thumb = queue_scrollbar_thumb_rect(layout.table, state.queued_paths.size());
        if (contains_point(thumb, x, y)) {
            begin_queue_scroll_drag(y, layout.table, state.queued_paths.size());
            return true;
        }
        if (contains_point(track, x, y)) {
            const int visible_rows = std::max(1, queue_visible_row_count(layout.table));
            (void)scroll_queue_rows(y < thumb.top ? -visible_rows : visible_rows);
            return true;
        }
    }
    if (y >= header_bottom && y < layout.table.bottom && row_height > 0) {
        if (x >= columns_table.right) {
            return true;
        }
        const int visible_index = (y - header_bottom) / row_height;
        const int index = queue_scroll_first_row_ + visible_index;
        const RECT row{columns_table.left, header_bottom + (visible_index * row_height), columns_table.right,
                       header_bottom + ((visible_index + 1) * row_height)};
        const auto columns = queue_column_layout(columns_table, row);
        if (contains_point(columns.checkbox, x, y)) {
            return toggle_queue_item(static_cast<std::size_t>(std::max(0, index)));
        }
        {
            std::lock_guard lock(mutex_);
            normalize_queue_selection_locked();
            if (index >= 0 && index < static_cast<int>(state_.queued_paths.size())) {
                state_.selected_queue_index = index;
                request_repaint();
                return true;
            }
        }
    }
    return false;
}

// Purpose: Handle Compress page hit-testing and commands.
// Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
// Outputs: Returns true when a compression control consumed the click.
bool MainWindow::handle_compress_click(const RECT& content, int x, int y) {
    const auto layout = compress_layout(content);
    if (handle_active_dropdown_click(x, y)) {
        return true;
    }
    if (contains_point(layout.start, x, y)) {
        start_compress();
        return true;
    }
    if (contains_point(layout.destination, x, y)) {
        choose_destination();
        return true;
    }
    if (contains_point(layout.format, x, y)) {
        open_dropdown(DropdownId::CompressFormat);
        return true;
    }
    if (contains_point(layout.compression_level, x, y)) {
        bool enabled = false;
        {
            std::lock_guard lock(mutex_);
            enabled = compression_format_uses_level(compression_format_value(state_.compression_format_index));
        }
        if (enabled) {
            open_dropdown(DropdownId::CompressLevel);
        }
        return true;
    }
    if (contains_point(layout.method, x, y)) {
        bool enabled = false;
        {
            std::lock_guard lock(mutex_);
            enabled = compression_format_uses_suzip_tuning(compression_format_value(state_.compression_format_index));
        }
        if (enabled) {
            open_dropdown(DropdownId::CompressMethod);
        }
        return true;
    }
    if (contains_point(layout.block_size, x, y)) {
        bool enabled = false;
        {
            std::lock_guard lock(mutex_);
            enabled = compression_format_uses_suzip_tuning(compression_format_value(state_.compression_format_index));
        }
        if (enabled) {
            open_dropdown(DropdownId::CompressBlockSize);
        }
        return true;
    }
    if (contains_point(layout.solid_archive, x, y)) {
        return toggle_bool_setting(&UiState::solid_archive, ToggleId::SolidArchive);
    }
    if (contains_point(layout.store_timestamps, x, y)) {
        return toggle_bool_setting(&UiState::store_timestamps, ToggleId::StoreTimestamps);
    }
    if (contains_point(layout.delete_after_compression, x, y)) {
        return toggle_bool_setting(&UiState::delete_after_compression, ToggleId::DeleteAfterCompression);
    }
    if (contains_point(layout.verify, x, y)) {
        return toggle_bool_setting(&UiState::verify_after_write_opt_in, ToggleId::VerifyAfterWrite);
    }
    if (contains_point(layout.sha, x, y)) {
        return toggle_bool_setting(&UiState::integrity_hash_opt_in, ToggleId::IntegrityHash);
    }
    if (contains_point(layout.defender, x, y)) {
        return toggle_bool_setting(&UiState::defender_scan_opt_in, ToggleId::DefenderScan);
    }
    return false;
}

// Purpose: Handle Extract page hit-testing and commands.
// Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
// Outputs: Returns true when an extraction control consumed the click.
bool MainWindow::handle_extract_click(const RECT& content, int x, int y) {
    const auto layout = extract_layout(content);
    if (handle_active_dropdown_click(x, y)) {
        return true;
    }
    if (contains_point(layout.start, x, y)) {
        start_extract();
        return true;
    }
    if (contains_point(layout.destination, x, y)) {
        choose_destination();
        return true;
    }
    if (contains_point(layout.overwrite_policy, x, y)) {
        open_dropdown(DropdownId::ExtractOverwrite);
        return true;
    }
    if (contains_point(layout.verify_metadata, x, y)) {
        return toggle_bool_setting(&UiState::verify_metadata_before_extract, ToggleId::VerifyMetadata);
    }
    if (contains_point(layout.open_destination_after_extract, x, y)) {
        return toggle_bool_setting(&UiState::open_destination_after_extract, ToggleId::OpenDestinationAfterExtract);
    }
    if (contains_point(layout.sha, x, y)) {
        return toggle_bool_setting(&UiState::integrity_hash_opt_in, ToggleId::IntegrityHash);
    }
    if (contains_point(layout.defender, x, y)) {
        return toggle_bool_setting(&UiState::defender_scan_opt_in, ToggleId::DefenderScan);
    }
    return false;
}

// Purpose: Handle History page hit-testing and commands.
// Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
// Outputs: Returns true when a history filter or command consumed the click.
bool MainWindow::handle_history_click(const RECT& content, int x, int y) {
    RECT area = inset_rect(content, scale(kPageInsetX), scale(kPageInsetY));
    const RECT operation{area.left, area.top + scale(48), area.left + scale(220), area.top + scale(92)};
    const RECT status{area.left + scale(238), area.top + scale(48), area.left + scale(458), area.top + scale(92)};
    const RECT clear = history_clear_button_rect(area);
    const RECT table{area.left, area.top + scale(112), area.right, area.bottom - scale(96)};
    const int header_bottom = table.top + scale(kQueueHeaderHeight);
    if (handle_active_dropdown_click(x, y)) {
        return true;
    }
    if (contains_point(operation, x, y)) {
        open_dropdown(DropdownId::HistoryOperation);
        return true;
    }
    if (contains_point(status, x, y)) {
        open_dropdown(DropdownId::HistoryStatus);
        return true;
    }
    if (contains_point(clear, x, y)) {
        clear_history();
        return true;
    }
    if (y >= table.top && y < header_bottom) {
        const RECT header_row{table.left, table.top, table.right, header_bottom};
        const auto columns = history_column_layout(table, header_row);
        for (int separator = 0; separator < static_cast<int>(columns.resize_grips.size()); ++separator) {
            if (contains_point(columns.resize_grips[static_cast<std::size_t>(separator)], x, y)) {
                begin_history_column_resize(separator, x);
                return true;
            }
        }
        return true;
    }
    return false;
}

// Purpose: Handle Security page hit-testing and commands.
// Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
// Outputs: Returns true when the security review command consumed the click.
bool MainWindow::handle_security_click(const RECT& content, int x, int y) {
    RECT area = inset_rect(content, scale(kPageInsetX), scale(kPageInsetY));
    const RECT verify_button = primary_action_rect(area);
    if (!contains_point(verify_button, x, y)) {
        return false;
    }
    start_security_verify();
    return true;
}

// Purpose: Start a real background security verification pass for queued inputs.
// Inputs: None; reads enabled queue rows and security opt-ins.
// Outputs: Launches a Verify job or reports that no queued item is selected.
void MainWindow::start_security_verify() {
    std::vector<std::filesystem::path> sources;
    bool integrity = false;
    bool defender = false;
    {
        std::lock_guard lock(mutex_);
        normalize_queue_selection_locked();
        for (std::size_t i = 0; i < state_.queued_paths.size(); ++i) {
            if (state_.queued_enabled[i]) {
                sources.push_back(state_.queued_paths[i]);
            }
        }
        integrity = state_.integrity_hash_opt_in;
        defender = state_.defender_scan_opt_in;
    }
    if (sources.empty()) {
        {
            std::lock_guard lock(mutex_);
            state_.status = "Select queue items before security verification";
        }
        request_repaint();
        return;
    }
    run_job(
        [this, sources, integrity, defender] {
            ProgressState progress;
            progress.start(OperationKind::Verify, sources.size(), sources.size());
            auto publish = [this, &progress] { publish_progress_snapshot(progress.snapshot()); };
            for (const auto& path : sources) {
                progress.set_current(path.filename().string());
                publish();
                std::error_code ec;
                if (!std::filesystem::exists(path, ec)) {
                    throw SecurityError("queued path does not exist: " + path.string());
                }
                if (integrity && std::filesystem::is_regular_file(path, ec)) {
                    const auto hash = hash_file(path, IntegrityMode::Sha256);
                    append_history_entry("Security", path.filename().string(), "SHA-256 " + hash.hex_digest, true);
                }
                if (defender) {
                    const auto scan = scan_with_windows_defender(path, DefenderScanMode::FullPath);
                    append_history_entry("Security", path.filename().string(),
                                         defender_history_status("Defender", scan), !scan.attempted || scan.clean);
                    if (scan.attempted && !scan.clean) {
                        throw SecurityError("Microsoft Defender did not report the path as clean: " + path.string());
                    }
                }
                progress.add_bytes(1);
                progress.finish_entry();
                publish();
            }
            append_history_entry("Security", "Security review",
                                 "Verified " + std::to_string(sources.size()) + " selected queue item(s)", true);
        },
        "Verifying");
}

// Purpose: Handle System page hit-testing and commands.
// Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
// Outputs: Returns true when the update-speed dropdown consumed the click.
bool MainWindow::handle_gpu_click(const RECT& content, int x, int y) {
    const RECT update_speed = dropdown_anchor_rect(DropdownId::GpuUpdateSpeed, content);
    if (handle_active_dropdown_click(x, y)) {
        return true;
    }
    if (contains_point(update_speed, x, y)) {
        open_dropdown(DropdownId::GpuUpdateSpeed);
        return true;
    }
    return false;
}

// Purpose: Handle Settings page hit-testing and commands.
// Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
// Outputs: Returns true when a preference control consumed the click.
bool MainWindow::handle_settings_click(const RECT& content, int x, int y) {
    const auto layout = settings_layout(content);
    if (handle_active_dropdown_click(x, y)) {
        return true;
    }
    if (contains_point(layout.restore_defaults, x, y)) {
        try {
            reset_settings_to_defaults();
        } catch (const std::exception& error) {
            {
                std::lock_guard lock(mutex_);
                state_.status = std::string("Settings reset failed: ") + error.what();
            }
            append_log_entry(LogSeverity::Warning, "Settings reset failed");
        }
        return true;
    }
    if (contains_point(layout.apply, x, y)) {
        try {
            apply_settings();
        } catch (const std::exception& error) {
            {
                std::lock_guard lock(mutex_);
                state_.status = std::string("Settings apply failed: ") + error.what();
            }
            append_log_entry(LogSeverity::Warning, "Settings apply failed");
        }
        return true;
    }
    if (contains_point(layout.sha, x, y)) {
        return toggle_bool_setting(&UiState::integrity_hash_opt_in, ToggleId::IntegrityHash);
    }
    if (contains_point(layout.defender, x, y)) {
        return toggle_bool_setting(&UiState::defender_scan_opt_in, ToggleId::DefenderScan);
    }
    if (contains_point(layout.gpu, x, y)) {
        return toggle_bool_setting(&UiState::gpu_required, ToggleId::GpuRequired);
    }
    if (contains_point(layout.verify, x, y)) {
        return toggle_bool_setting(&UiState::verify_after_write_opt_in, ToggleId::VerifyAfterWrite);
    }
    if (contains_point(layout.open_destination_after_operation, x, y)) {
        return toggle_bool_setting(&UiState::open_destination_after_operation, ToggleId::OpenDestinationAfterOperation);
    }
    if (contains_point(layout.confirm_before_deleting, x, y)) {
        return toggle_bool_setting(&UiState::confirm_before_deleting, ToggleId::ConfirmBeforeDeleting);
    }
    if (contains_point(layout.show_operation_summary, x, y)) {
        return toggle_bool_setting(&UiState::show_operation_summary, ToggleId::ShowOperationSummary);
    }
    if (contains_point(layout.memory_policy, x, y)) {
        open_dropdown(DropdownId::SettingsMemoryPolicy);
        return true;
    }
    if (contains_point(layout.log_level, x, y)) {
        open_dropdown(DropdownId::SettingsLogLevel);
        return true;
    }
    if (contains_point(layout.log_retention, x, y)) {
        open_dropdown(DropdownId::SettingsLogRetention);
        return true;
    }
    return false;
}

// Purpose: Handle a click while a dropdown menu is expanded.
// Inputs: `x` and `y` are physical-pixel mouse coordinates relative to the client area.
// Outputs: Returns true when the click was consumed by dropdown close or selection.
bool MainWindow::handle_active_dropdown_click(int x, int y) {
    DropdownId active = DropdownId::None;
    {
        std::lock_guard lock(mutex_);
        active = state_.active_dropdown;
    }
    if (active == DropdownId::None) {
        return false;
    }

    const RECT content = content_rect();
    const RECT menu = dropdown_menu_rect(active, content);
    const RECT anchor = dropdown_anchor_rect(active, content);
    if (contains_point(menu, x, y)) {
        const auto options = dropdown_options(active);
        const int row_height = scale(options.size() > 10U ? 28 : 32);
        const int option_index = row_height > 0 ? (y - menu.top - scale(1)) / row_height : -1;
        if (option_index >= 0 && option_index < static_cast<int>(options.size())) {
            select_dropdown_option(active, option_index);
            return true;
        }
    }
    if (contains_point(anchor, x, y)) {
        close_active_dropdown();
        return true;
    }
    close_active_dropdown();
    return true;
}

// Purpose: Open one dropdown menu and seed keyboard selection to the current value.
// Inputs: `id` identifies the dropdown to open.
// Outputs: Updates UI state and queues a repaint.

}  // namespace superzip::app
