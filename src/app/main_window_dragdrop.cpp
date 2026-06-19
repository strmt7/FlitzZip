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

class QueueDropTarget final : public IDropTarget {
  public:
    explicit QueueDropTarget(MainWindow& owner) : owner_(owner) {}

    // Purpose: Return the COM interfaces supported by the Queue drop target.
    // Inputs: `iid` selects the interface and `object` receives the result.
    // Outputs: Returns `S_OK` for IUnknown/IDropTarget or `E_NOINTERFACE`.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    // Purpose: Increment the COM lifetime count.
    // Inputs: None.
    // Outputs: Returns the new reference count.
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++ref_count_;
    }

    // Purpose: Decrement the COM lifetime count and delete when it reaches zero.
    // Inputs: None.
    // Outputs: Returns the remaining reference count.
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --ref_count_;
        if (remaining == 0U) {
            delete this;
        }
        return remaining;
    }

    // Purpose: Enter shell drag/drop tracking for the Queue table.
    // Inputs: `data`, key state, screen point, and requested effect are supplied by OLE.
    // Outputs: Enables copy only for HDROP data over the Queue table and updates highlight state.
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL point, DWORD* effect) override {
        data_object_has_files_ = data_has_hdrop(data);
        return update_effect(point, effect);
    }

    // Purpose: Update Queue table drag/drop highlight while the shell pointer moves.
    // Inputs: Screen point and requested effect are supplied by OLE.
    // Outputs: Enables copy only while still inside the Queue table.
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override {
        return update_effect(point, effect);
    }

    // Purpose: Clear Queue drag/drop highlighting when the shell drag leaves.
    // Inputs: None.
    // Outputs: Clears live highlighting.
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        owner_.set_queue_drop_highlight(false);
        data_object_has_files_ = false;
        return S_OK;
    }

    // Purpose: Accept a shell drop only inside the Queue table.
    // Inputs: `data`, key state, screen point, and requested effect are supplied by OLE.
    // Outputs: Queues dropped paths or rejects the drop with `DROPEFFECT_NONE`.
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL point, DWORD* effect) override {
        owner_.set_queue_drop_highlight(false);
        POINT client{point.x, point.y};
        ScreenToClient(owner_.hwnd_, &client);
        auto paths = paths_from_data_object(data);
        const bool accepted = owner_.accept_dropped_paths(std::move(paths), client);
        if (effect != nullptr) {
            *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }
        data_object_has_files_ = false;
        return S_OK;
    }

  private:
    // Purpose: Check whether an OLE data object offers shell file paths.
    // Inputs: `data` is the OLE data object from drag/drop.
    // Outputs: Returns true when `CF_HDROP` data can be queried.
    static bool data_has_hdrop(IDataObject* data) {
        if (data == nullptr) {
            return false;
        }
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return data->QueryGetData(&format) == S_OK;
    }

    // Purpose: Extract HDROP paths from an OLE data object.
    // Inputs: `data` is the OLE data object from a drop event.
    // Outputs: Returns shell paths and always releases OLE storage before returning.
    static std::vector<std::filesystem::path> paths_from_data_object(IDataObject* data) {
        if (data == nullptr) {
            return {};
        }
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        if (data->GetData(&format, &medium) != S_OK) {
            return {};
        }
        struct MediumGuard {
            // Purpose: Release OLE storage exactly once when extracting shell drop paths.
            // Inputs: `value` points to the STGMEDIUM returned by `IDataObject::GetData`.
            // Outputs: Stores the medium reference for scope-exit cleanup.
            explicit MediumGuard(STGMEDIUM& value) : medium(value) {}

            MediumGuard(const MediumGuard&) = delete;
            MediumGuard& operator=(const MediumGuard&) = delete;

            // Purpose: Release the OLE storage returned by `IDataObject::GetData`.
            // Inputs: None.
            // Outputs: Calls `ReleaseStgMedium` exactly once.
            ~MediumGuard() {
                ReleaseStgMedium(&medium);
            }

            STGMEDIUM& medium;
        } guard{medium};
        if (medium.tymed != TYMED_HGLOBAL || medium.hGlobal == nullptr) {
            return {};
        }
        std::vector<std::filesystem::path> paths = paths_from_dropfiles_global(medium.hGlobal);
        if (paths.empty()) {
            paths = paths_from_hdrop(reinterpret_cast<HDROP>(medium.hGlobal));
        }
        return paths;
    }

    // Purpose: Convert an OLE screen point into the current Queue-table copy/drop effect.
    // Inputs: `point` is in screen coordinates and `effect` receives the accepted operation.
    // Outputs: Updates table highlight and returns `S_OK`.
    HRESULT update_effect(POINTL point, DWORD* effect) {
        POINT client{point.x, point.y};
        ScreenToClient(owner_.hwnd_, &client);
        const bool allowed = data_object_has_files_ && owner_.queue_drop_target_contains(client);
        owner_.set_queue_drop_highlight(allowed);
        if (effect != nullptr) {
            *effect = allowed ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }
        return S_OK;
    }

    MainWindow& owner_;
    std::atomic_ulong ref_count_{1};
    bool data_object_has_files_ = false;
};

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
// Outputs: Returns true only on the Queue page and inside the Queue table.
bool MainWindow::queue_drop_target_contains(POINT point) {
    UiState state;
    {
        std::lock_guard lock(mutex_);
        state = state_;
    }
    if (state.page != Page::Queue) {
        return false;
    }
    return contains_point(queue_layout(content_rect()).table, point.x, point.y);
}

// Purpose: Append dropped paths to the Queue when the drop target is inside the Queue table.
// Inputs: `paths` are filesystem paths from shell drag/drop and `point` is the client drop coordinate.
// Outputs: Returns true and mutates queue state when the drop is accepted; otherwise reports rejection.
bool MainWindow::accept_dropped_paths(std::vector<std::filesystem::path> paths, POINT point) {
    set_queue_drop_highlight(false);
    LogSeverity severity = LogSeverity::Debug;
    std::string message;
    bool may_append = false;
    {
        std::lock_guard lock(mutex_);
        if (state_.page != Page::Queue || !contains_point(queue_layout(content_rect()).table, point.x, point.y)) {
            state_.status = "Drop files or folders inside the Queue box";
            severity = LogSeverity::Warning;
            message = "Shell drop rejected outside the Queue table";
        } else if (paths.empty()) {
            state_.status = "No drop items received";
            severity = LogSeverity::Warning;
            message = "Shell drop did not contain usable paths";
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

// Purpose: Allow shell file-drop messages through UIPI for elevated windows.
// Inputs: None; applies only to the main HWND and only when the process is elevated.
// Outputs: Returns true when no extra filter is needed or the narrow filter was applied.
bool MainWindow::enable_elevated_drag_drop_messages() const {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return true;
    }
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL queried = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned) != FALSE;
    CloseHandle(token);
    if (!queried || elevation.TokenIsElevated == 0) {
        return true;
    }
    const BOOL drop_allowed = ChangeWindowMessageFilterEx(hwnd_, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    const BOOL copy_allowed = ChangeWindowMessageFilterEx(hwnd_, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    const BOOL query_allowed = ChangeWindowMessageFilterEx(hwnd_, kDragQueryMessage, MSGFLT_ALLOW, nullptr);
    return drop_allowed != FALSE && copy_allowed != FALSE && query_allowed != FALSE;
}

// Purpose: Initialize drag/drop, performance sampling, and smoke timers during window creation.
// Inputs: None.
// Outputs: Arms timers and returns the handled Win32 result.
LRESULT MainWindow::handle_create() {
    initialize_settings();
    DragAcceptFiles(hwnd_, TRUE);
    const HRESULT ole_status = OleInitialize(nullptr);
    ole_initialized_ = SUCCEEDED(ole_status);
    if (ole_initialized_) {
        drop_target_ = new QueueDropTarget(*this);
        if (RegisterDragDrop(hwnd_, drop_target_) != S_OK) {
            drop_target_->Release();
            drop_target_ = nullptr;
            append_log_entry(LogSeverity::Warning, "OLE Queue drag/drop registration could not be applied");
        }
    } else {
        append_log_entry(LogSeverity::Warning, "OLE Queue drag/drop initialization failed");
    }
    if (!enable_elevated_drag_drop_messages()) {
        {
            std::lock_guard lock(mutex_);
            state_.status = "Elevated drag/drop filter unavailable";
        }
        append_log_entry(LogSeverity::Warning, "Elevated shell drop message filter could not be applied");
    } else {
        append_log_entry(LogSeverity::Information, "Application initialized");
    }
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
    shutdown_performance_monitor();
    PostQuitMessage(0);
    return 0;
}

}  // namespace superzip::app
