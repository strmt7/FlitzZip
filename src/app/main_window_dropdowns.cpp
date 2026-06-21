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

// Purpose: Open a dropdown menu and initialize its keyboard selection.
// Inputs: `id` identifies the dropdown to open.
// Outputs: Updates dropdown state and queues repaint.
void MainWindow::open_dropdown(DropdownId id) {
    {
        std::lock_guard lock(mutex_);
        state_.active_dropdown = id;
        state_.status = "Dropdown opened";
        dropdown_keyboard_index_ = dropdown_selected_index(state_, id);
    }
    request_repaint();
}

// Purpose: Close any expanded dropdown menu.
// Inputs: None.
// Outputs: Clears dropdown state and queues a repaint when needed.
void MainWindow::close_active_dropdown() {
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        changed = state_.active_dropdown != DropdownId::None;
        state_.active_dropdown = DropdownId::None;
        dropdown_keyboard_index_ = -1;
    }
    if (changed) {
        request_repaint();
    }
}

struct DropdownSelectionFeedback {
    int performance_seconds = 0;
    bool io_drive_changed = false;
    bool add_log = false;
    LogSeverity log_severity = LogSeverity::Information;
    std::string log_message;
};

// Purpose: Apply a non-Settings dropdown choice to synchronized UI state.
// Inputs: `state` is locked UI state, `id` is the dropdown, `option_index` is the selected row, and `feedback` receives
// side effects. Outputs: Mutates `state`; unsupported IDs are ignored.
void apply_primary_dropdown_selection(UiState& state, DropdownId id, int option_index,
                                      DropdownSelectionFeedback& feedback) {
    switch (id) {
    case DropdownId::CompressFormat:
        state.compression_format_index = std::clamp(option_index, 0, kCompressionFormatMaxIndex);
        state.status = "Format changed";
        break;
    case DropdownId::CompressLevel:
        state.compression_level_index = std::clamp(option_index, 0, 4);
        state.status = "Compression level changed";
        break;
    case DropdownId::CompressMethod:
        state.gpu_required = option_index == 0;
        state.status = "Compression method changed";
        break;
    case DropdownId::CompressBlockSize:
        state.compression_block_size_index = std::clamp(option_index, 0, kCompressionBlockSizeMaxIndex);
        state.status = "Block size changed";
        break;
    case DropdownId::ExtractOverwrite:
        state.overwrite = option_index == 1;
        state.status = "Overwrite policy changed";
        break;
    case DropdownId::HistoryOperation:
        state.history_operation_filter_index = std::clamp(option_index, 0, 4);
        state.status = "History operation filter changed";
        break;
    case DropdownId::HistoryStatus:
        state.history_status_filter_index = std::clamp(option_index, 0, 2);
        state.status = "History status filter changed";
        break;
    case DropdownId::GpuUpdateSpeed:
        state.performance_update_seconds = kPerformanceUpdateSecondsOptions[static_cast<std::size_t>(
            std::clamp(option_index, 0, static_cast<int>(kPerformanceUpdateSecondsOptions.size()) - 1))];
        feedback.performance_seconds = state.performance_update_seconds;
        state.status = "Refresh interval changed";
        break;
    case DropdownId::SystemIoDrive:
        state.io_drive_index = normalize_io_drive_index(option_index);
        feedback.io_drive_changed = true;
        state.status = "I/O drive changed";
        break;
    default:
        break;
    }
}

// Purpose: Apply a Settings dropdown choice to synchronized UI state.
// Inputs: `state` is locked UI state, `id` is the dropdown, `option_index` is the selected row, and `feedback` receives
// log data. Outputs: Mutates `state`; unsupported IDs are ignored.
void apply_settings_dropdown_selection(UiState& state, DropdownId id, int option_index,
                                       DropdownSelectionFeedback& feedback) {
    switch (id) {
    case DropdownId::SettingsMemoryPolicy:
        state.memory_policy_index = std::clamp(option_index, 0, 2);
        state.status = "Memory policy changed";
        feedback.add_log = true;
        feedback.log_severity = LogSeverity::Debug;
        feedback.log_message = "Memory policy changed";
        break;
    case DropdownId::SettingsLogLevel:
        state.log_level_index = std::clamp(option_index, 0, 2);
        state.status = "Log level changed";
        feedback.add_log = true;
        if (state.log_level_index == 1) {
            feedback.log_severity = LogSeverity::Warning;
            feedback.log_message = "Log level changed to Warning";
        } else if (state.log_level_index == 2) {
            feedback.log_severity = LogSeverity::Debug;
            feedback.log_message = "Log level changed to Debug";
        } else {
            feedback.log_severity = LogSeverity::Information;
            feedback.log_message = "Log level changed to Information";
        }
        break;
    case DropdownId::SettingsLogRetention:
        state.log_retention_index = std::clamp(option_index, 0, 2);
        prune_log_entries(state.logs, std::chrono::system_clock::now(), state.log_retention_index, kMaxLogEntries);
        state.status = "Log retention changed";
        feedback.add_log = true;
        feedback.log_severity = LogSeverity::Debug;
        feedback.log_message = "Log retention changed to ";
        feedback.log_message += log_retention_log_text(state.log_retention_index);
        break;
    default:
        break;
    }
}

// Purpose: Apply one selected dropdown option to the matching UI state field.
// Inputs: `id` identifies the active dropdown and `option_index` is the zero-based row selected by the user.
// Outputs: Mutates UI state, closes the dropdown, updates status text, and requests repaint.
void MainWindow::select_dropdown_option(DropdownId id, int option_index) {
    int performance_seconds = 0;
    bool io_drive_changed = false;
    bool add_log = false;
    LogSeverity log_severity = LogSeverity::Information;
    std::string log_message;
    {
        std::lock_guard lock(mutex_);
        DropdownSelectionFeedback feedback;
        apply_primary_dropdown_selection(state_, id, option_index, feedback);
        apply_settings_dropdown_selection(state_, id, option_index, feedback);
        state_.active_dropdown = DropdownId::None;
        dropdown_keyboard_index_ = -1;
        performance_seconds = feedback.performance_seconds;
        io_drive_changed = feedback.io_drive_changed;
        add_log = feedback.add_log;
        log_severity = feedback.log_severity;
        log_message = std::move(feedback.log_message);
    }
    if (id == DropdownId::HistoryOperation || id == DropdownId::HistoryStatus) {
        history_scroll_first_row_ = 0;
        history_details_scroll_pixels_ = 0;
    }
    if (io_drive_changed) {
        reset_disk_performance_monitor();
    }
    if (performance_seconds > 0) {
        reset_performance_timer(performance_seconds);
    }
    if (add_log) {
        append_log_entry(log_severity, std::move(log_message));
        return;
    }
    request_repaint();
}

// Purpose: Change the active application page and discard unapplied Settings drafts when leaving Settings.
// Inputs: `page` is the destination page.
// Outputs: Mutates page state and restores the last applied Settings snapshot if needed.

}  // namespace superzip::app
