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

// Purpose: Identify clipboard-copy accelerators that SuperZip-owned UI must consume.
// Inputs: `key` is the current virtual key and the current keyboard state supplies modifiers.
// Outputs: Returns true for Ctrl+C and Ctrl+Insert copy shortcuts.
bool is_copy_accelerator(WPARAM key) {
    const bool control_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    return control_down && (key == L'C' || key == VK_INSERT);
}

// Purpose: Build keyboard focus targets for the active page.
// Inputs: `content` is the page content rectangle and `state` is the copied UI state.
// Outputs: Returns ordered focus targets for Tab, arrows, Enter, and Space handling.
std::vector<FocusTarget> MainWindow::focus_targets_for(const RECT& content, const UiState& state) const {
    std::vector<FocusTarget> targets;
    targets.reserve(32);
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int item_height = scale(63);
    const int nav_top = scale(kTopBar) + scale(10);
    for (int index = 0; index < 8; ++index) {
        append_focus_target(targets, FocusTargetKind::Navigation,
                            RECT{client.left, nav_top + (index * item_height), client.left + scale(kRailWidth),
                                 nav_top + ((index + 1) * item_height)},
                            index);
    }
    switch (state.page) {
    case Page::Queue:
        add_queue_focus_targets(targets, content, state);
        break;
    case Page::Compress:
        add_compress_focus_targets(targets, content, state);
        break;
    case Page::Extract:
        add_extract_focus_targets(targets, content);
        break;
    case Page::Security: {
        RECT area = inset_rect(content, scale(kPageInsetX), scale(kPageInsetY));
        append_focus_target(targets, FocusTargetKind::SecurityVerify, primary_action_rect(area));
    } break;
    case Page::History: {
        RECT area = inset_rect(content, scale(kPageInsetX), scale(kPageInsetY));
        append_focus_target(targets, FocusTargetKind::HistoryOperation,
                            RECT{area.left, area.top + scale(48), area.left + scale(220), area.top + scale(92)});
        append_focus_target(
            targets, FocusTargetKind::HistoryStatus,
            RECT{area.left + scale(238), area.top + scale(48), area.left + scale(458), area.top + scale(92)});
        append_focus_target(targets, FocusTargetKind::HistoryClear, history_clear_button_rect(area));
        const RECT table{area.left, area.top + scale(112), area.right, area.bottom - scale(96)};
        const int header_bottom = table.top + scale(kQueueHeaderHeight);
        const int row_height = scale(kQueueRowHeight);
        const auto visible = filtered_history_indices(state);
        const RECT columns_table = history_columns_table(table, visible.size());
        const int first_visible_row =
            std::clamp(history_scroll_first_row_, 0, history_max_scroll_offset(table, visible.size()));
        const int remaining_rows = static_cast<int>(visible.size()) - first_visible_row;
        const int row_count = std::min(std::max(0, remaining_rows), std::max(0, history_visible_row_count(table)));
        for (int index = 0; index < row_count; ++index) {
            const int top = header_bottom + (index * row_height);
            const auto history_index = visible[static_cast<std::size_t>(first_visible_row + index)];
            append_focus_target(targets, FocusTargetKind::HistoryRow,
                                RECT{columns_table.left, top, columns_table.right, top + row_height},
                                static_cast<int>(history_index));
        }
    } break;
    case Page::Gpu:
        append_focus_target(targets, FocusTargetKind::SystemUpdateSpeed,
                            dropdown_anchor_rect(DropdownId::GpuUpdateSpeed, content));
        append_focus_target(targets, FocusTargetKind::SystemIoDrive,
                            dropdown_anchor_rect(DropdownId::SystemIoDrive, content));
        break;
    case Page::Settings:
        add_settings_focus_targets(targets, content);
        break;
    case Page::About:
        append_focus_target(targets, FocusTargetKind::AboutLicenses,
                            about_licenses_button_rect(inset_rect(content, scale(kPageInsetX), scale(kPageInsetY))));
        break;
    }
    return targets;
}

// Purpose: Append all Queue page keyboard-focus targets.
// Inputs: `targets` receives controls, `content` is the content rectangle, and `state` is a copied UI snapshot.
// Outputs: Adds Queue action buttons, header tick, and visible row targets.
void MainWindow::add_queue_focus_targets(std::vector<FocusTarget>& targets, const RECT& content,
                                         const UiState& state) const {
    const auto layout = queue_layout(content);
    if (has_selected_queue_items(state)) {
        append_focus_target(targets, FocusTargetKind::QueueRemoveSelected, layout.remove_selected);
    }
    append_focus_target(targets, FocusTargetKind::QueueAddFiles, layout.add_files);
    append_focus_target(targets, FocusTargetKind::QueueAddFolder, layout.add_folder);
    append_focus_target(targets, FocusTargetKind::QueueClear, layout.clear);
    if (state.queued_paths.empty()) {
        return;
    }
    const int header_bottom = layout.table.top + scale(kQueueHeaderHeight);
    const RECT columns_table = queue_columns_table(layout.table, state.queued_paths.size());
    const RECT header_row{layout.table.left, layout.table.top, layout.table.right, header_bottom};
    append_focus_target(targets, FocusTargetKind::QueueHeaderCheckbox,
                        queue_column_layout(columns_table, header_row).header_checkbox);
    const int row_height = scale(kQueueRowHeight);
    const int visible_rows = queue_visible_row_count(layout.table);
    const int row_count = std::min(static_cast<int>(state.queued_paths.size()), std::max(0, visible_rows));
    const int first_visible_row =
        std::clamp(queue_scroll_first_row_, 0, queue_max_scroll_offset(layout.table, state.queued_paths.size()));
    for (int index = 0; index < row_count; ++index) {
        const int top = header_bottom + (index * row_height);
        append_focus_target(targets, FocusTargetKind::QueueRow,
                            RECT{columns_table.left, top, columns_table.right, top + row_height},
                            first_visible_row + index);
    }
}

// Purpose: Append all Compress page keyboard-focus targets.
// Inputs: `targets` receives controls, `content` is the content rectangle, and `state` is a copied UI snapshot.
// Outputs: Adds command, destination, format, tuning, and toggle targets that are currently enabled.
void MainWindow::add_compress_focus_targets(std::vector<FocusTarget>& targets, const RECT& content,
                                            const UiState& state) const {
    const auto layout = compress_layout(content);
    const auto format = compression_format_value(state.compression_format_index);
    append_focus_target(targets, FocusTargetKind::CompressStart, layout.start);
    append_focus_target(targets, FocusTargetKind::CompressDestination, layout.destination);
    append_focus_target(targets, FocusTargetKind::CompressFormat, layout.format);
    if (compression_format_uses_level(format)) {
        append_focus_target(targets, FocusTargetKind::CompressLevel, layout.compression_level);
    }
    if (compression_format_uses_suzip_tuning(format)) {
        append_focus_target(targets, FocusTargetKind::CompressMethod, layout.method);
        append_focus_target(targets, FocusTargetKind::CompressBlockSize, layout.block_size);
    }
    append_focus_target(targets, FocusTargetKind::CompressSolidArchive, layout.solid_archive);
    append_focus_target(targets, FocusTargetKind::CompressStoreTimestamps, layout.store_timestamps);
    append_focus_target(targets, FocusTargetKind::CompressDeleteAfterCompression, layout.delete_after_compression);
    append_focus_target(targets, FocusTargetKind::CompressVerifyAfterWrite, layout.verify);
    append_focus_target(targets, FocusTargetKind::CompressIntegrityHash, layout.sha);
    append_focus_target(targets, FocusTargetKind::CompressDefenderScan, layout.defender);
}

// Purpose: Append all Extract page keyboard-focus targets.
// Inputs: `targets` receives controls and `content` is the content rectangle.
// Outputs: Adds command, destination, overwrite, and integrity/security toggles.
void MainWindow::add_extract_focus_targets(std::vector<FocusTarget>& targets, const RECT& content) const {
    const auto layout = extract_layout(content);
    append_focus_target(targets, FocusTargetKind::ExtractStart, layout.start);
    append_focus_target(targets, FocusTargetKind::ExtractDestination, layout.destination);
    append_focus_target(targets, FocusTargetKind::ExtractOverwrite, layout.overwrite_policy);
    append_focus_target(targets, FocusTargetKind::ExtractVerifyMetadata, layout.verify_metadata);
    append_focus_target(targets, FocusTargetKind::ExtractOpenDestination, layout.open_destination_after_extract);
    append_focus_target(targets, FocusTargetKind::ExtractIntegrityHash, layout.sha);
    append_focus_target(targets, FocusTargetKind::ExtractDefenderScan, layout.defender);
}

// Purpose: Append all Settings page keyboard-focus targets.
// Inputs: `targets` receives controls and `content` is the content rectangle.
// Outputs: Adds Settings buttons, toggles, and dropdowns in tab order.
void MainWindow::add_settings_focus_targets(std::vector<FocusTarget>& targets, const RECT& content) const {
    const auto layout = settings_layout(content);
    append_focus_target(targets, FocusTargetKind::SettingsRestoreDefaults, layout.restore_defaults);
    append_focus_target(targets, FocusTargetKind::SettingsApply, layout.apply);
    append_focus_target(targets, FocusTargetKind::SettingsOpenDestination, layout.open_destination_after_operation);
    append_focus_target(targets, FocusTargetKind::SettingsConfirmDelete, layout.confirm_before_deleting);
    append_focus_target(targets, FocusTargetKind::SettingsShowSummary, layout.show_operation_summary);
    append_focus_target(targets, FocusTargetKind::SettingsIntegrityHash, layout.sha);
    append_focus_target(targets, FocusTargetKind::SettingsDefenderScan, layout.defender);
    append_focus_target(targets, FocusTargetKind::SettingsGpuRequired, layout.gpu);
    append_focus_target(targets, FocusTargetKind::SettingsVerifyAfterWrite, layout.verify);
    append_focus_target(targets, FocusTargetKind::SettingsMemoryPolicy, layout.memory_policy);
    append_focus_target(targets, FocusTargetKind::SettingsLogLevel, layout.log_level);
    append_focus_target(targets, FocusTargetKind::SettingsLogRetention, layout.log_retention);
    append_focus_target(targets, FocusTargetKind::SettingsOpenLogFile, layout.open_log_file);
}

// Purpose: Normalize the current keyboard focus to a valid target index.
// Inputs: `targets` is the current page's focus target list.
// Outputs: Updates focus index and returns false if no target exists.
bool MainWindow::normalize_focus_index(const std::vector<FocusTarget>& targets) {
    if (targets.empty()) {
        keyboard_focus_index_ = 0;
        return false;
    }
    keyboard_focus_index_ =
        (keyboard_focus_index_ % static_cast<int>(targets.size()) + static_cast<int>(targets.size())) %
        static_cast<int>(targets.size());
    return true;
}

// Purpose: Move keyboard focus by a signed delta through the current page's focus list.
// Inputs: `delta` is normally +1 or -1.
// Outputs: Mutates focus index and queues a repaint.
bool MainWindow::move_keyboard_focus(int delta) {
    UiState state;
    {
        std::lock_guard lock(mutex_);
        state = state_;
    }
    const auto targets = focus_targets_for(content_rect(), state);
    if (!normalize_focus_index(targets)) {
        return false;
    }
    keyboard_focus_index_ += delta;
    normalize_focus_index(targets);
    request_repaint();
    return true;
}

// Purpose: Activate a focused control with Enter or Space.
// Inputs: `target` is the current focus target and `key` is the activating virtual key.
// Outputs: Executes the same command/toggle/dropdown path as mouse activation.
bool MainWindow::activate_focus_target(const FocusTarget& target, WPARAM key) {
    if (target.kind == FocusTargetKind::Navigation) {
        set_page(static_cast<Page>(std::clamp(target.index, 0, 7)));
        return true;
    }
    if (target.kind == FocusTargetKind::QueueRow && key == VK_SPACE) {
        return toggle_queue_item(static_cast<std::size_t>(std::max(0, target.index)));
    }
    if (target.kind == FocusTargetKind::HistoryRow && (key == VK_SPACE || key == VK_RETURN)) {
        std::lock_guard lock(mutex_);
        if (target.index >= 0 && target.index < static_cast<int>(state_.history.size())) {
            state_.selected_history_index = target.index;
            state_.status = "History row selected";
            request_repaint();
            return true;
        }
        return false;
    }
    const int x = (target.rect.left + target.rect.right) / 2;
    const int y = (target.rect.top + target.rect.bottom) / 2;
    return handle_content_click(x, y);
}

// Purpose: Move keyboard selection inside an expanded dropdown.
// Inputs: `key` is an arrow/Home/End key, `active` identifies the dropdown, and `state` is a stable UI snapshot.
// Outputs: Updates dropdown keyboard index and repaints when the dropdown consumes the key.
bool MainWindow::handle_dropdown_navigation_key(WPARAM key, DropdownId active, const UiState& state) {
    const auto options = dropdown_options(active);
    if (options.empty()) {
        return true;
    }
    int index = dropdown_keyboard_index_ >= 0 ? dropdown_keyboard_index_ : dropdown_selected_index(state, active);
    if (key == VK_HOME) {
        index = 0;
    } else if (key == VK_END) {
        index = static_cast<int>(options.size()) - 1;
    } else if (key == VK_UP || key == VK_LEFT) {
        --index;
    } else if (key == VK_DOWN || key == VK_RIGHT) {
        ++index;
    }
    dropdown_keyboard_index_ = (index % static_cast<int>(options.size()) + static_cast<int>(options.size())) %
                               static_cast<int>(options.size());
    request_repaint();
    return true;
}

// Purpose: Move keyboard focus through visible Queue rows.
// Inputs: `key` is Up or Down and `state` is a stable UI snapshot.
// Outputs: Updates selected Queue row, scroll offset, keyboard focus, and repaint state when consumed.
bool MainWindow::handle_queue_row_navigation_key(WPARAM key, const UiState& state) {
    const auto targets = focus_targets_for(content_rect(), state);
    if (!normalize_focus_index(targets)) {
        return false;
    }
    const auto& target = targets[static_cast<std::size_t>(keyboard_focus_index_)];
    if (target.kind != FocusTargetKind::QueueRow) {
        return false;
    }
    const auto layout = queue_layout(content_rect());
    const int row_count =
        static_cast<int>(std::min<std::size_t>(state.queued_paths.size(), static_cast<std::size_t>(INT_MAX)));
    const int visible_rows = std::max(1, queue_visible_row_count(layout.table));
    const int next_row = std::clamp(target.index + (key == VK_DOWN ? 1 : -1), 0, std::max(0, row_count - 1));
    {
        std::lock_guard lock(mutex_);
        state_.selected_queue_index = next_row;
    }
    const int max_scroll = queue_max_scroll_offset(layout.table, state.queued_paths.size());
    if (next_row < queue_scroll_first_row_) {
        queue_scroll_first_row_ = next_row;
    } else if (next_row >= queue_scroll_first_row_ + visible_rows) {
        queue_scroll_first_row_ = next_row - visible_rows + 1;
    }
    queue_scroll_first_row_ = std::clamp(queue_scroll_first_row_, 0, max_scroll);
    const auto refreshed_targets = focus_targets_for(content_rect(), state);
    for (int target_index = 0; target_index < static_cast<int>(refreshed_targets.size()); ++target_index) {
        const auto& refreshed = refreshed_targets[static_cast<std::size_t>(target_index)];
        if (refreshed.kind == FocusTargetKind::QueueRow && refreshed.index == next_row) {
            keyboard_focus_index_ = target_index;
            break;
        }
    }
    request_repaint();
    return true;
}

// Purpose: Move keyboard focus through visible History rows.
// Inputs: `key` is Up or Down and `state` is a stable UI snapshot.
// Outputs: Updates selected History row, scroll offset, keyboard focus, and repaint state when consumed.
bool MainWindow::handle_history_row_navigation_key(WPARAM key, const UiState& state) {
    const auto targets = focus_targets_for(content_rect(), state);
    if (!normalize_focus_index(targets)) {
        return false;
    }
    const auto& target = targets[static_cast<std::size_t>(keyboard_focus_index_)];
    if (target.kind != FocusTargetKind::HistoryRow) {
        return false;
    }
    const RECT area = inset_rect(content_rect(), scale(kPageInsetX), scale(kPageInsetY));
    const RECT table{area.left, area.top + scale(112), area.right, area.bottom - scale(96)};
    const auto visible = filtered_history_indices(state);
    if (visible.empty()) {
        return false;
    }
    const auto current_iter =
        std::find(visible.begin(), visible.end(), static_cast<std::size_t>(std::max(0, target.index)));
    const int current_filtered_index =
        current_iter == visible.end() ? 0 : static_cast<int>(std::distance(visible.begin(), current_iter));
    const int next_filtered_index = std::clamp(current_filtered_index + (key == VK_DOWN ? 1 : -1), 0,
                                               std::max(0, static_cast<int>(visible.size()) - 1));
    const int next_history_index = static_cast<int>(visible[static_cast<std::size_t>(next_filtered_index)]);
    {
        std::lock_guard lock(mutex_);
        state_.selected_history_index = next_history_index;
    }
    const int visible_rows = std::max(1, history_visible_row_count(table));
    const int max_scroll = history_max_scroll_offset(table, visible.size());
    if (next_filtered_index < history_scroll_first_row_) {
        history_scroll_first_row_ = next_filtered_index;
    } else if (next_filtered_index >= history_scroll_first_row_ + visible_rows) {
        history_scroll_first_row_ = next_filtered_index - visible_rows + 1;
    }
    history_scroll_first_row_ = std::clamp(history_scroll_first_row_, 0, max_scroll);
    const auto refreshed_targets = focus_targets_for(content_rect(), state);
    for (int target_index = 0; target_index < static_cast<int>(refreshed_targets.size()); ++target_index) {
        const auto& refreshed = refreshed_targets[static_cast<std::size_t>(target_index)];
        if (refreshed.kind == FocusTargetKind::HistoryRow && refreshed.index == next_history_index) {
            keyboard_focus_index_ = target_index;
            break;
        }
    }
    request_repaint();
    return true;
}

// Purpose: Move keyboard focus to the first or last focusable control.
// Inputs: `key` is Home or End and `state` is a stable UI snapshot.
// Outputs: Updates keyboard focus and repaints when at least one target exists.
bool MainWindow::handle_home_end_navigation_key(WPARAM key, const UiState& state) {
    const auto targets = focus_targets_for(content_rect(), state);
    if (!normalize_focus_index(targets)) {
        return false;
    }
    keyboard_focus_index_ = key == VK_HOME ? 0 : static_cast<int>(targets.size()) - 1;
    request_repaint();
    return true;
}

// Purpose: Move within open dropdown options or Queue rows using arrow keys.
// Inputs: `key` is one of the supported arrow/home/end virtual keys.
// Outputs: Updates selection or focus and returns true when consumed.
bool MainWindow::handle_navigation_key(WPARAM key) {
    DropdownId active = DropdownId::None;
    UiState state;
    {
        std::lock_guard lock(mutex_);
        active = state_.active_dropdown;
        state = state_;
    }
    if (active != DropdownId::None) {
        return handle_dropdown_navigation_key(key, active, state);
    }
    if (state.page == Page::Queue && !state.queued_paths.empty() && (key == VK_UP || key == VK_DOWN)) {
        return handle_queue_row_navigation_key(key, state);
    }
    if (state.page == Page::History && !state.history.empty() && (key == VK_UP || key == VK_DOWN)) {
        return handle_history_row_navigation_key(key, state);
    }
    if (key == VK_HOME || key == VK_END) {
        return handle_home_end_navigation_key(key, state);
    }
    if (key == VK_LEFT || key == VK_UP) {
        return move_keyboard_focus(-1);
    }
    if (key == VK_RIGHT || key == VK_DOWN) {
        return move_keyboard_focus(1);
    }
    return false;
}

// Purpose: Handle keyboard traversal and activation with native Windows conventions.
// Inputs: `wparam` is a virtual key and `lparam` is the raw key message payload.
// Outputs: Moves focus, activates controls, updates dropdowns, or returns default processing.
LRESULT MainWindow::handle_key_down(WPARAM wparam, LPARAM) {
    if (is_copy_accelerator(wparam)) {
        return 0;
    }
    bool overwrite_modal_visible = false;
    bool license_modal_visible = false;
    {
        std::lock_guard lock(mutex_);
        overwrite_modal_visible = state_.extract_overwrite_prompt_visible;
        license_modal_visible = state_.license_notices_dialog_visible;
    }
    if (license_modal_visible) {
        return handle_license_notices_dialog_key(wparam) ? 0 : 0;
    }
    if (overwrite_modal_visible) {
        return handle_extract_overwrite_prompt_key(wparam) ? 0 : 0;
    }
    if (wparam == VK_ESCAPE) {
        close_active_dropdown();
        return 0;
    }
    if (wparam == VK_TAB) {
        return move_keyboard_focus((GetKeyState(VK_SHIFT) & 0x8000) != 0 ? -1 : 1) ? 0 : 0;
    }
    if (wparam == VK_UP || wparam == VK_DOWN || wparam == VK_LEFT || wparam == VK_RIGHT || wparam == VK_HOME ||
        wparam == VK_END) {
        return handle_navigation_key(wparam) ? 0 : 0;
    }
    if (wparam == VK_RETURN || wparam == VK_SPACE) {
        DropdownId active = DropdownId::None;
        UiState state;
        {
            std::lock_guard lock(mutex_);
            active = state_.active_dropdown;
            state = state_;
        }
        if (active != DropdownId::None) {
            const int selected =
                dropdown_keyboard_index_ >= 0 ? dropdown_keyboard_index_ : dropdown_selected_index(state, active);
            select_dropdown_option(active, selected);
            return 0;
        }
        const auto targets = focus_targets_for(content_rect(), state);
        if (normalize_focus_index(targets)) {
            (void)activate_focus_target(targets[static_cast<std::size_t>(keyboard_focus_index_)], wparam);
        }
        return 0;
    }
    return DefWindowProcW(hwnd_, WM_KEYDOWN, wparam, 0);
}

// Purpose: Handle clicks inside the content area.
// Inputs: `x` and `y` are physical-pixel mouse coordinates relative to the client area.
// Outputs: Returns true when a setting was changed and a repaint was queued.

}  // namespace superzip::app
