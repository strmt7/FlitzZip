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

// Purpose: Load persisted per-user settings and publish the current applied snapshot.
// Inputs: None; reads the current user's Local AppData settings file when present.
// Outputs: Applies validated settings, creates defaults when needed, and logs nonfatal parse/write failures.
void MainWindow::initialize_settings() {
    try {
        ensure_app_storage();
        applied_settings_ = read_settings_file(settings_file_path());
        {
            std::lock_guard lock(mutex_);
            apply_settings_to_state(applied_settings_, state_);
            state_.status = "Settings loaded";
        }
        reset_performance_timer(applied_settings_.performance_update_seconds);
        write_settings_file(settings_file_path(), applied_settings_);
    } catch (const std::exception& error) {
        {
            std::lock_guard lock(mutex_);
            applied_settings_ = settings_from_state(state_);
            state_.status = std::string("Settings unavailable: ") + error.what();
        }
        append_log_entry(LogSeverity::Warning, "Settings file could not be loaded");
    }
}

// Purpose: Save the current Settings draft as the applied snapshot.
// Inputs: None; reads Settings fields from synchronized UI state.
// Outputs: Writes the per-user config atomically and updates the applied snapshot.
void MainWindow::apply_settings() {
    AppSettings draft;
    {
        std::lock_guard lock(mutex_);
        draft = settings_from_state(state_);
    }
    write_settings_file(settings_file_path(), draft);
    applied_settings_ = draft;
    {
        std::lock_guard lock(mutex_);
        apply_settings_to_state(applied_settings_, state_);
        state_.status = "Settings applied";
    }
    reset_performance_timer(applied_settings_.performance_update_seconds);
    append_history_entry("Settings", "Settings", settings_file_path().string(), "Applied for current session", true);
    append_log_entry(LogSeverity::Information, "Settings applied for current session");
}

// Purpose: Revert Settings page controls to the last applied snapshot.
// Inputs: None.
// Outputs: Mutates Settings-owned UI fields and re-arms dependent timers.
void MainWindow::revert_settings_draft() {
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        changed = !settings_equal(settings_from_state(state_), applied_settings_);
        apply_settings_to_state(applied_settings_, state_);
        if (changed) {
            state_.status = "Unapplied settings reverted";
        }
    }
    reset_performance_timer(applied_settings_.performance_update_seconds);
    if (changed) {
        append_log_entry(LogSeverity::Debug, "Unapplied settings reverted after leaving Settings");
    } else {
        request_repaint();
    }
}

// Purpose: Reset Settings draft and applied snapshot to safe defaults.
// Inputs: None.
// Outputs: Updates UI, applied snapshot, and the persisted config file.
void MainWindow::reset_settings_to_defaults() {
    applied_settings_ = {};
    write_settings_file(settings_file_path(), applied_settings_);
    {
        std::lock_guard lock(mutex_);
        apply_settings_to_state(applied_settings_, state_);
        state_.destination_directory.clear();
        normalize_queue_selection_locked();
        state_.selected_queue_index = state_.queued_paths.empty() ? -1 : 0;
        state_.history_operation_filter_index = 0;
        state_.history_status_filter_index = 0;
        state_.status = "Defaults restored";
    }
    reset_performance_timer(applied_settings_.performance_update_seconds);
    append_log_entry(LogSeverity::Information, "Default settings restored");
}

// Purpose: Dispatch timers owned by the main window.
// Inputs: `wparam` identifies the timer from `WM_TIMER`.
// Outputs: Runs animation, monitoring, or smoke shutdown work and returns the handled Win32 result.
LRESULT MainWindow::handle_timer(WPARAM wparam) {
    if (wparam == kAnimationTimer) {
        tick_animation();
        return 0;
    }
    if (wparam == kPerformanceTimer) {
        update_performance_sample();
        request_repaint();
        return 0;
    }
    if (wparam == kProgressHoldTimer) {
        clear_expired_progress();
        return 0;
    }
    if (wparam == kClockTimer) {
        const auto current = current_user_time_text();
        if (current != last_clock_text_) {
            last_clock_text_ = current;
            request_repaint();
        }
        return 0;
    }
    if (wparam == kTextTooltipTimer) {
        KillTimer(hwnd_, kTextTooltipTimer);
        RECT candidate_cell{};
        std::wstring candidate_text;
        const bool still_hovered = text_tooltip_candidate_at_mouse(candidate_cell, candidate_text);
        const bool stationary =
            text_tooltip_anchor_point_.x == mouse_position_.x && text_tooltip_anchor_point_.y == mouse_position_.y;
        text_tooltip_visible_ = still_hovered && stationary && text_tooltip_cell_active_ &&
                                EqualRect(&text_tooltip_cell_, &candidate_cell) != FALSE &&
                                text_tooltip_text_ == candidate_text;
        if (!text_tooltip_visible_) {
            text_tooltip_cell_active_ = false;
            text_tooltip_text_.clear();
        }
        request_repaint();
        return 0;
    }
    if (wparam == kSmokeAutoCloseTimer) {
        DestroyWindow(hwnd_);
        return 0;
    }
    if (wparam == kSmokeClosePollTimer) {
        // The marker file gives the smoke harness a second shutdown path that
        // does not depend on external window activation.
        if (smoke_close_requested()) {
            DestroyWindow(hwnd_);
        }
        return 0;
    }
    return DefWindowProcW(hwnd_, WM_TIMER, wparam, 0);
}

// Purpose: Stop timers and release monitor state before window destruction completes.
// Inputs: None.
// Outputs: Posts quit and returns the handled Win32 result.

}  // namespace superzip::app
