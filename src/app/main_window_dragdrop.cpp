#include "app/main_window_impl.hpp"
#include "app/queue_drop_target.hpp"

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
namespace flitzzip::app {

// Purpose: Append native shell-dropped paths to the queue.
// Inputs: `wparam` contains the HDROP handle from `WM_DROPFILES`.
// Outputs: Updates queue selection, releases the HDROP handle, and returns the handled Win32 result.
LRESULT MainWindow::handle_drop_files(WPARAM wparam) {
    // Native shell drag/drop is a queue regression boundary and is covered by
    // the GUI smoke harness with an injected HDROP payload.
    auto drop = reinterpret_cast<HDROP>(wparam);
    if (drop == nullptr) {
        return 0;
    }
    POINT drop_point{};
    DragQueryPoint(drop, &drop_point);
    try {
        (void)accept_dropped_paths(paths_from_hdrop(drop), drop_point);
    } catch (const std::exception& error) {
        DragFinish(drop);
        {
            std::lock_guard lock(mutex_);
            state_.status = std::string("Drop failed: ") + error.what();
        }
        append_log_entry(LogSeverity::Warning, "Shell drop failed");
        return 0;
    } catch (...) {
        DragFinish(drop);
        {
            std::lock_guard lock(mutex_);
            state_.status = "Drop failed";
        }
        append_log_entry(LogSeverity::Warning, "Shell drop failed");
        return 0;
    }
    DragFinish(drop);
    return 0;
}

// Purpose: Return whether a client point is inside the active Queue table drop target.
// Inputs: `point` is a client-coordinate point.
// Outputs: Returns true only on the Queue page, with no modal surface active, and inside the Queue table.
bool MainWindow::queue_drop_target_contains(POINT point) {
    UiState state;
    {
        std::lock_guard lock(mutex_);
        state = state_;
    }
    if (state.page != Page::Queue || state.extract_overwrite_prompt_visible || state.license_notices_dialog_visible) {
        return false;
    }
    return contains_point(queue_layout(content_rect()).table, point.x, point.y);
}

// Purpose: Append dropped paths to the Queue when the drop target is inside the Queue table.
// Inputs: `paths` are filesystem paths from shell drag/drop and `point` is the client drop coordinate.
// Outputs: Returns true and mutates queue state when the drop is accepted; otherwise reports rejection.
bool MainWindow::accept_dropped_paths(std::vector<std::filesystem::path> paths, POINT point) {
    set_queue_drop_highlight(false);
    const bool local_paths_only = std::ranges::all_of(paths, is_supported_local_drop_path);
    LogSeverity severity = LogSeverity::Debug;
    std::string message;
    bool may_append = false;
    {
        std::lock_guard lock(mutex_);
        if (state_.page != Page::Queue || state_.extract_overwrite_prompt_visible ||
            state_.license_notices_dialog_visible ||
            !contains_point(queue_layout(content_rect()).table, point.x, point.y)) {
            state_.status = "Drop files or folders inside the Queue box";
            severity = LogSeverity::Warning;
            message = "Shell drop rejected outside the Queue table";
        } else if (paths.empty()) {
            state_.status = "No drop items received";
            severity = LogSeverity::Warning;
            message = "Shell drop did not contain usable paths";
        } else if (!local_paths_only) {
            state_.status = "Only local absolute files and folders can be dropped";
            severity = LogSeverity::Warning;
            message = "Shell drop rejected a remote, device, relative, or malformed path";
        } else if (state_.queued_paths.size() >= kMaxQueueItems) {
            state_.status = "Queue limit reached (4,096 items)";
            severity = LogSeverity::Warning;
            message = "Shell drop rejected because the Queue is full";
        } else {
            may_append = true;
        }
    }
    if (!may_append) {
        append_log_entry(severity, std::move(message));
        return false;
    }

    const std::size_t added = append_queued_paths(std::move(paths), "Dropped items added");
    if (added == 0U) {
        {
            std::lock_guard lock(mutex_);
            state_.status = "No drop items received";
        }
        append_log_entry(LogSeverity::Warning, "Shell drop did not contain usable paths");
        return false;
    }
    append_log_entry(LogSeverity::Debug, "Shell drop added " + std::to_string(added) + " queued item(s)");
    return true;
}

// Purpose: Update live drag/drop highlighting for the Queue table.
// Inputs: `active` describes whether a drag is over the allowed table drop target.
// Outputs: Updates visual drag state and queues a repaint when changed.
void MainWindow::set_queue_drop_highlight(bool active) {
    if (queue_drop_highlight_ == active) {
        return;
    }
    queue_drop_highlight_ = active;
    request_repaint();
}

// Purpose: Initialize drag/drop, performance sampling, and smoke timers during window creation.
// Inputs: None.
// Outputs: Arms timers and returns the handled Win32 result.
LRESULT MainWindow::handle_create() {
    initialize_settings();
    const HRESULT ole_status = OleInitialize(nullptr);
    ole_initialized_ = SUCCEEDED(ole_status);
    if (ole_initialized_) {
        drop_target_ = new QueueDropTarget(
            hwnd_, {
                       [this](POINT point) { return queue_drop_target_contains(point); },
                       [this](std::vector<std::filesystem::path> paths, POINT point) {
                           return accept_dropped_paths(std::move(paths), point);
                       },
                       [this](bool active) { set_queue_drop_highlight(active); },
                       [this](const char* message) { append_operation_log(LogSeverity::Warning, "Queue", message); },
                   });
        if (RegisterDragDrop(hwnd_, drop_target_) != S_OK) {
            drop_target_->Release();
            drop_target_ = nullptr;
            append_log_entry(LogSeverity::Warning, "OLE Queue drag/drop registration could not be applied");
        }
    } else {
        append_log_entry(LogSeverity::Warning, "OLE Queue drag/drop initialization failed");
    }
    // Elevated windows intentionally keep UIPI defaults. OLE remains the only
    // interactive Queue drop surface and lower-integrity message injection fails closed.
    append_log_entry(LogSeverity::Information, "Application initialized");
    initialize_performance_monitor();
    update_performance_sample();
    reset_performance_timer(state_.performance_update_seconds);
    last_clock_text_ = current_user_time_text();
    SetTimer(hwnd_, kClockTimer, kClockPollMs, nullptr);
    if (const UINT auto_close_ms = smoke_auto_close_ms(); auto_close_ms > 0) {
        // Smoke-only auto-close prevents orphaned GUI windows if the harness
        // exits before it can post WM_CLOSE.
        SetTimer(hwnd_, kSmokeAutoCloseTimer, auto_close_ms, nullptr);
    }
    if (!smoke_close_marker_path().empty()) {
        SetTimer(hwnd_, kSmokeClosePollTimer, 250, nullptr);
    }
    return 0;
}

// Purpose: Stop timers and release monitor state before window destruction completes.
// Inputs: None.
// Outputs: Posts quit and returns the handled Win32 result.
LRESULT MainWindow::handle_destroy() {
    // Kill every timer owned by this window before shutdown so repaint,
    // telemetry sampling, and smoke cleanup cannot outlive the HWND.
    KillTimer(hwnd_, kAnimationTimer);
    KillTimer(hwnd_, kPerformanceTimer);
    KillTimer(hwnd_, kProgressHoldTimer);
    KillTimer(hwnd_, kClockTimer);
    KillTimer(hwnd_, kTextTooltipTimer);
    KillTimer(hwnd_, kDeferredCommandTimer);
    KillTimer(hwnd_, kSmokeAutoCloseTimer);
    KillTimer(hwnd_, kSmokeClosePollTimer);
    if (drop_target_ != nullptr) {
        RevokeDragDrop(hwnd_);
        drop_target_->Release();
        drop_target_ = nullptr;
    }
    if (ole_initialized_) {
        OleUninitialize();
        ole_initialized_ = false;
    }
    stop_folder_size_worker();
    shutdown_performance_monitor();
    PostQuitMessage(0);
    return 0;
}

}  // namespace flitzzip::app
