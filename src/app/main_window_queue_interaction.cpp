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

// Purpose: Dispatch a content-area click to the active page or modal prompt.
// Inputs: `x` and `y` are client mouse coordinates.
// Outputs: Returns true when a content control consumed the click.
bool MainWindow::handle_content_click(int x, int y) {
    Page page;
    {
        std::lock_guard lock(mutex_);
        page = state_.page;
    }
    const RECT content = content_rect();
    switch (page) {
    case Page::Queue:
        return handle_queue_click(content, x, y);
    case Page::Compress:
        return handle_compress_click(content, x, y);
    case Page::Extract:
        return handle_extract_click(content, x, y);
    case Page::Security:
        return handle_security_click(content, x, y);
    case Page::History:
        return handle_history_click(content, x, y);
    case Page::Gpu:
        return handle_gpu_click(content, x, y);
    case Page::Settings:
        return handle_settings_click(content, x, y);
    case Page::About:
        return handle_about_click(content, x, y);
    }
    return false;
}

// Purpose: Handle mouse activation while the overwrite confirmation modal is active.
// Inputs: `x` and `y` are client coordinates.
// Outputs: Consumes every click, continuing or cancelling only when an action button is hit.
bool MainWindow::handle_extract_overwrite_prompt_click(int x, int y) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const RECT workspace{content_rect().left, content_rect().top, client.right, content_rect().bottom};
    const auto buttons = extract_overwrite_prompt_buttons(extract_overwrite_prompt_rect(workspace));
    if (contains_point(buttons[0], x, y)) {
        modal_focus_index_ = 0;
        continue_extract_overwrite_prompt();
        return true;
    }
    if (contains_point(buttons[1], x, y)) {
        modal_focus_index_ = 1;
        cancel_extract_overwrite_prompt();
        return true;
    }
    return true;
}

// Purpose: Handle keyboard activation while the overwrite confirmation modal is active.
// Inputs: `key` is the pressed virtual key.
// Outputs: Consumes modal navigation, Continue, Cancel, and Escape actions.
bool MainWindow::handle_extract_overwrite_prompt_key(WPARAM key) {
    if (key == VK_ESCAPE) {
        cancel_extract_overwrite_prompt();
        return true;
    }
    if (key == VK_TAB || key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN) {
        modal_focus_index_ = modal_focus_index_ == 0 ? 1 : 0;
        request_repaint();
        return true;
    }
    if (key == VK_RETURN || key == VK_SPACE) {
        if (modal_focus_index_ == 0) {
            continue_extract_overwrite_prompt();
        } else {
            cancel_extract_overwrite_prompt();
        }
        return true;
    }
    return true;
}

// Purpose: Toggle a boolean UI-state member with the standard animated toggle feedback.
// Inputs: `member` selects the state field and `id` identifies the visual toggle to animate.
// Outputs: Mutates the state, starts the toggle animation, queues repaint, and returns true.
bool MainWindow::toggle_bool_setting(bool UiState::* member, ToggleId id) {
    bool previous = false;
    bool next = false;
    {
        std::lock_guard lock(mutex_);
        previous = state_.*member;
        state_.*member = !previous;
        next = state_.*member;
    }
    start_toggle_animation(id, previous, next);
    request_repaint();
    return true;
}

// Purpose: Toggle a checkbox-style boolean UI-state member without knob animation.
// Inputs: `member` selects the state field and `status` is the visible status-line text.
// Outputs: Mutates the state, writes status text, queues repaint, and returns true.
bool MainWindow::checkbox_bool_setting(bool UiState::* member, const char* status) {
    {
        std::lock_guard lock(mutex_);
        state_.*member = !(state_.*member);
        state_.status = status;
    }
    request_repaint();
    return true;
}

// Purpose: Keep queue enable flags one-for-one with queued paths while the UI mutex is held.
// Inputs: None; reads and mutates `state_`.
// Outputs: Normalizes enable flags and selected row bounds.
void MainWindow::normalize_queue_selection_locked() {
    if (state_.queued_enabled.size() < state_.queued_paths.size()) {
        state_.queued_enabled.resize(state_.queued_paths.size(), true);
    } else if (state_.queued_enabled.size() > state_.queued_paths.size()) {
        state_.queued_enabled.resize(state_.queued_paths.size());
    }
    if (state_.queued_paths.empty()) {
        state_.selected_queue_index = -1;
    } else if (state_.selected_queue_index < 0 ||
               state_.selected_queue_index >= static_cast<int>(state_.queued_paths.size())) {
        state_.selected_queue_index = 0;
    }
}

// Purpose: Append filesystem paths to the Queue through one shared mutation path.
// Inputs: `paths` are selected or dropped filesystem paths and `status` is the visible status after success.
// Outputs: Returns the number of nonempty paths appended, updates selection/scroll state, and repaints on success.
std::size_t MainWindow::append_queued_paths(std::vector<std::filesystem::path> paths, std::string status) {
    std::size_t added = 0;
    {
        std::lock_guard lock(mutex_);
        const bool was_empty = state_.queued_paths.empty();
        state_.queued_paths.reserve(state_.queued_paths.size() + paths.size());
        state_.queued_enabled.reserve(state_.queued_enabled.size() + paths.size());
        for (auto& path : paths) {
            if (path.empty()) {
                continue;
            }
            state_.queued_paths.emplace_back(std::move(path));
            state_.queued_enabled.push_back(true);
            ++added;
        }
        if (added == 0U) {
            return 0U;
        }
        normalize_queue_selection_locked();
        if (was_empty) {
            state_.selected_queue_index = 0;
            queue_scroll_first_row_ = 0;
            queue_wheel_delta_remainder_ = 0;
        }
        state_.status = std::move(status);
    }
    request_repaint();
    return added;
}

// Purpose: Test whether a copied Queue state has at least one checked row.
// Inputs: `state` is a stable UI snapshot with queue paths and checkbox flags.
// Outputs: Returns true only when at least one queued item is selected for operations.
bool MainWindow::has_selected_queue_items(const UiState& state) const {
    for (std::size_t index = 0; index < state.queued_paths.size(); ++index) {
        if (index >= state.queued_enabled.size() || state.queued_enabled[index]) {
            return true;
        }
    }
    return false;
}

// Purpose: Toggle all queued items from the header checkbox.
// Inputs: None.
// Outputs: Mutates every row enable flag and queues a repaint.
bool MainWindow::toggle_all_queue_items() {
    {
        std::lock_guard lock(mutex_);
        normalize_queue_selection_locked();
        if (state_.queued_paths.empty()) {
            state_.status = "Queue is empty";
            request_repaint();
            return true;
        }
        const bool all_enabled = std::all_of(state_.queued_enabled.begin(), state_.queued_enabled.end(),
                                             [](bool enabled) { return enabled; });
        std::fill(state_.queued_enabled.begin(), state_.queued_enabled.end(), !all_enabled);
        state_.status = all_enabled ? "All queue items deselected" : "All queue items selected";
    }
    request_repaint();
    return true;
}

// Purpose: Toggle one queued item checkbox.
// Inputs: `index` is the queue row to update.
// Outputs: Mutates the row enable flag, focuses the row, and queues a repaint.
bool MainWindow::toggle_queue_item(std::size_t index) {
    {
        std::lock_guard lock(mutex_);
        normalize_queue_selection_locked();
        if (index >= state_.queued_enabled.size()) {
            return false;
        }
        state_.queued_enabled[index] = !state_.queued_enabled[index];
        state_.selected_queue_index = static_cast<int>(index);
        state_.status = state_.queued_enabled[index] ? "Queue item selected" : "Queue item deselected";
    }
    request_repaint();
    return true;
}

// Purpose: Remove every checked Queue row.
// Inputs: None; reads the current queue and checkbox state.
// Outputs: Deletes selected queue entries, preserves unchecked entries, resets scroll bounds, and queues repaint.
bool MainWindow::remove_selected_queue_items() {
    std::size_t removed = 0;
    {
        std::lock_guard lock(mutex_);
        normalize_queue_selection_locked();
        std::vector<std::filesystem::path> remaining_paths;
        std::vector<bool> remaining_enabled;
        remaining_paths.reserve(state_.queued_paths.size());
        remaining_enabled.reserve(state_.queued_enabled.size());
        for (std::size_t index = 0; index < state_.queued_paths.size(); ++index) {
            const bool selected = index >= state_.queued_enabled.size() || state_.queued_enabled[index];
            if (selected) {
                ++removed;
                continue;
            }
            remaining_paths.push_back(state_.queued_paths[index]);
            remaining_enabled.push_back(false);
        }
        if (removed == 0) {
            state_.status = "No queue items selected";
            return true;
        }
        state_.queued_paths = std::move(remaining_paths);
        state_.queued_enabled = std::move(remaining_enabled);
        state_.selected_queue_index = state_.queued_paths.empty() ? -1 : 0;
        queue_scroll_first_row_ = 0;
        queue_wheel_delta_remainder_ = 0;
        state_.status = "Removed " + std::to_string(removed) + " selected queue item" + (removed == 1 ? "" : "s");
    }
    request_repaint();
    return true;
}

// Purpose: Start resizing adjacent queue data columns.
// Inputs: `separator` is the 0-2 data-column boundary and `x` is the initial mouse coordinate.
// Outputs: Stores resize baseline state.
void MainWindow::begin_queue_column_resize(int separator, int x) {
    queue_column_resize_separator_ = std::clamp(separator, 0, 2);
    queue_column_resize_start_x_ = x;
    queue_column_resize_start_ = queue_column_widths_;
    SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
}

// Purpose: Update queue data-column widths while preserving readable minimums.
// Inputs: `x` is the current mouse coordinate.
// Outputs: Resizes the two columns adjacent to the active separator.
void MainWindow::update_queue_column_resize(int x) {
    if (queue_column_resize_separator_ < 0) {
        return;
    }
    constexpr std::array<int, 4> minimums{90, 70, 70, 120};
    const int left_index = queue_column_resize_separator_;
    const int right_index = left_index + 1;
    const int delta =
        std::lround(static_cast<double>(x - queue_column_resize_start_x_) * 96.0 / static_cast<double>(dpi_));
    const int pair_total = queue_column_resize_start_[left_index] + queue_column_resize_start_[right_index];
    const int left = std::clamp(queue_column_resize_start_[left_index] + delta, minimums[left_index],
                                pair_total - minimums[right_index]);
    queue_column_widths_[left_index] = left;
    queue_column_widths_[right_index] = pair_total - left;
    request_repaint();
}

// Purpose: End an active queue data-column resize.
// Inputs: None.
// Outputs: Clears resize state and queues one final repaint.
void MainWindow::end_queue_column_resize() {
    if (queue_column_resize_separator_ < 0) {
        return;
    }
    queue_column_resize_separator_ = -1;
    request_repaint();
}

// Purpose: Start resizing adjacent History data columns.
// Inputs: `separator` is the 0-2 data-column boundary and `x` is the initial mouse coordinate.
// Outputs: Stores resize baseline state.
void MainWindow::begin_history_column_resize(int separator, int x) {
    history_column_resize_separator_ = std::clamp(separator, 0, 3);
    history_column_resize_start_x_ = x;
    history_column_resize_start_ = history_column_widths_;
    SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
}

// Purpose: Update History data-column widths while preserving readable minimums.
// Inputs: `x` is the current mouse coordinate.
// Outputs: Resizes the two columns adjacent to the active separator.
void MainWindow::update_history_column_resize(int x) {
    if (history_column_resize_separator_ < 0) {
        return;
    }
    constexpr std::array<int, 5> minimums{82, 78, 140, 160, 74};
    const int left_index = history_column_resize_separator_;
    const int right_index = left_index + 1;
    const int delta =
        std::lround(static_cast<double>(x - history_column_resize_start_x_) * 96.0 / static_cast<double>(dpi_));
    const int pair_total = history_column_resize_start_[left_index] + history_column_resize_start_[right_index];
    const int left = std::clamp(history_column_resize_start_[left_index] + delta, minimums[left_index],
                                pair_total - minimums[right_index]);
    history_column_widths_[left_index] = left;
    history_column_widths_[right_index] = pair_total - left;
    request_repaint();
}

// Purpose: Test whether a History row passes the active operation and status filters.
// Inputs: `state` supplies the filter values and `entry` is one history row.
// Outputs: Returns true when the row should be visible.
bool MainWindow::history_entry_matches_filters(const UiState& state, const HistoryEntry& entry) const {
    const bool operation_match = state.history_operation_filter_index == 0 ||
                                 (state.history_operation_filter_index == 1 && entry.operation == "Compress") ||
                                 (state.history_operation_filter_index == 2 && entry.operation == "Extract") ||
                                 (state.history_operation_filter_index == 3 && entry.operation == "Security");
    const bool status_match = state.history_status_filter_index == 0 ||
                              (state.history_status_filter_index == 1 && entry.success) ||
                              (state.history_status_filter_index == 2 && !entry.success);
    return operation_match && status_match;
}

// Purpose: Build the visible History row index list for the active filters.
// Inputs: `state` is a stable UI snapshot.
// Outputs: Returns indexes into `state.history` in display order.
std::vector<std::size_t> MainWindow::filtered_history_indices(const UiState& state) const {
    std::vector<std::size_t> indices;
    indices.reserve(state.history.size());
    for (std::size_t index = 0; index < state.history.size(); ++index) {
        if (history_entry_matches_filters(state, state.history[index])) {
            indices.push_back(index);
        }
    }
    return indices;
}

// Purpose: End an active History data-column resize.
// Inputs: None.
// Outputs: Clears resize state and queues one final repaint.
void MainWindow::end_history_column_resize() {
    if (history_column_resize_separator_ < 0) {
        return;
    }
    history_column_resize_separator_ = -1;
    request_repaint();
}

// Purpose: Handle Queue page hit-testing and commands.
// Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
// Outputs: Returns true when a queue control or row selection consumed the click.

}  // namespace superzip::app
