#pragma once

#include "core/progress.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace superzip::app {

enum class Page {
    Queue,
    Compress,
    Extract,
    Security,
    History,
    Gpu,
    Preferences,
    About,
};

struct UiState {
    Page page = Page::Queue;
    std::vector<std::filesystem::path> queued_paths;
    std::vector<std::string> history;
    std::string status = "Ready";
    std::string gpu_status;
    ProgressSnapshot progress;
    bool prefer_szip = true;
    bool gpu_required = true;
    bool overwrite = false;
    bool integrity_hash_opt_in = false;
    bool defender_scan_opt_in = false;
};

class MainWindow {
public:
    // Purpose: Construct a main window controller with default UI state.
    // Inputs: None.
    // Outputs: Initializes fields only; the native HWND is created by `run`.
    MainWindow();

    // Purpose: Join worker threads and release GDI font handles.
    // Inputs: None.
    // Outputs: Blocks until any active worker finishes, then frees owned resources.
    ~MainWindow();

    // Purpose: Register and show the native Win32 SuperZip window.
    // Inputs: `instance` is the process HINSTANCE and `show_command` is the Windows show mode from `wWinMain`.
    // Outputs: Runs the message loop and returns the process exit code.
    int run(HINSTANCE instance, int show_command);

private:
    // Purpose: Route Win32 messages from the static window procedure to the C++ instance.
    // Inputs: Standard Win32 `hwnd`, `message`, `wparam`, and `lparam` arguments.
    // Outputs: Returns the message result expected by Win32.
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    // Purpose: Handle window, input, DPI, drag/drop, repaint, and lifecycle messages.
    // Inputs: Standard Win32 message payload.
    // Outputs: Returns zero for handled messages or delegates to `DefWindowProcW`.
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

    // Purpose: Paint the full client area using an off-screen buffer.
    // Inputs: None; obtains paint state from Win32.
    // Outputs: Renders one crisp frame and validates the paint region.
    void paint();

    // Purpose: Draw the current frame using DPI-scaled layout regions.
    // Inputs: `dc` is the off-screen device context and `rect` is the client rectangle in physical pixels.
    // Outputs: Writes the shell, navigation, active page, and status strip into `dc`.
    void layout_and_draw(HDC dc, const RECT& rect);

    // Purpose: Draw the top command/title strip.
    // Inputs: `dc` is the target and `rect` is the full client rectangle.
    // Outputs: Renders brand, command buttons, and the active page title.
    void draw_top_bar(HDC dc, const RECT& rect);

    // Purpose: Draw the compact navigation rail.
    // Inputs: `dc` is the target, `rect` is the rail rectangle, and `state` is the copied UI state.
    // Outputs: Renders all primary pages with active-page highlighting.
    void draw_navigation(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw the persistent AMD GPU and operation status strip.
    // Inputs: `dc` is the target, `rect` is the status-strip rectangle, and `state` is the copied UI state.
    // Outputs: Renders backend status, throughput, VRAM placeholder, and details affordance.
    void draw_status_bar(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw the active content page.
    // Inputs: `dc` is the target device context and `rect` is the content rectangle in physical pixels.
    // Outputs: Dispatches to the active page renderer.
    void draw_content(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw the queue page with command table, destination, profile, and progress controls.
    // Inputs: `dc` is the target, `rect` is the content area, and `state` is copied UI state.
    // Outputs: Renders the active queue and start controls.
    void draw_queue_page(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw the compression settings page.
    // Inputs: `dc` is the target, `rect` is the content area, and `state` contains opt-in settings.
    // Outputs: Renders archive destination, format/profile, advanced options, integrity toggles, and start control.
    void draw_compress_page(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw the extraction settings page.
    // Inputs: `dc` is the target, `rect` is the content area, and `state` contains opt-in settings.
    // Outputs: Renders archive/destination fields, overwrite policy, integrity toggles, and start control.
    void draw_extract_page(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw the security review page.
    // Inputs: `dc` is the target, `rect` is the content area, and `state` contains security choices.
    // Outputs: Renders path, CRC, integrity, Defender, and overwrite checks with explicit status.
    void draw_security_page(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw the operation history page.
    // Inputs: `dc` is the target, `rect` is the content area, and `state` contains session history.
    // Outputs: Renders filters, history rows, and selected-operation details.
    void draw_history_page(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw the AMD GPU diagnostics page.
    // Inputs: `dc` is the target, `rect` is the content area, and `state` contains backend status.
    // Outputs: Renders HIP status, device metadata, acceleration mode, and monitoring panels.
    void draw_gpu_page(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw the preferences page.
    // Inputs: `dc` is the target, `rect` is the content area, and `state` contains toggled defaults.
    // Outputs: Renders general, security, performance, and logging settings.
    void draw_preferences_page(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw the about page.
    // Inputs: `dc` is the target and `rect` is the content area.
    // Outputs: Renders product identity, version, and project scope.
    void draw_about_page(HDC dc, const RECT& rect);

    // Purpose: Draw a simple DPI-scaled command or navigation button.
    // Inputs: `dc` is the target, `rect` is the button rectangle, `text` is display text, and `active` selects accent styling.
    // Outputs: Renders the button into `dc`; text is ellipsized rather than overflowing.
    void draw_button(HDC dc, const RECT& rect, const wchar_t* text, bool active);

    // Purpose: Draw a DPI-scaled opt-in settings toggle row.
    // Inputs: `dc` is the target, `rect` is the row rectangle, `text` is display text, and `enabled` selects checked styling.
    // Outputs: Renders the toggle and label into `dc`.
    void draw_toggle(HDC dc, const RECT& rect, const wchar_t* text, bool enabled);

    // Purpose: Draw a DPI-scaled checkbox row.
    // Inputs: `dc` is the target, `rect` is the row rectangle, `text` is display text, and `enabled` selects checked styling.
    // Outputs: Renders a checkbox and label into `dc`.
    void draw_checkbox(HDC dc, const RECT& rect, const wchar_t* text, bool enabled);

    // Purpose: Draw a form field or select-style value box.
    // Inputs: `dc` is the target, `rect` is the box, `label` names the field, `value` is the current display value, and `select` adds an affordance.
    // Outputs: Renders label and bordered field with ellipsized value.
    void draw_field(HDC dc, const RECT& rect, const wchar_t* label, const std::wstring& value, bool select);

    // Purpose: Handle clicks inside the settings/security content area.
    // Inputs: `x` and `y` are physical-pixel mouse coordinates relative to the client area.
    // Outputs: Returns true when a setting was changed and a repaint was queued.
    bool handle_content_click(int x, int y);

    // Purpose: Change the active application page.
    // Inputs: `page` is the destination page enum value.
    // Outputs: Mutates UI state and queues a repaint.
    void set_page(Page page);

    // Purpose: Open the Windows file picker and append selected files to the queue.
    // Inputs: None; user selection comes from the common dialog.
    // Outputs: Mutates queued paths and queues a repaint when files are selected.
    void add_files();

    // Purpose: Open the Windows folder picker and append a selected folder to the queue.
    // Inputs: None; user selection comes from the shell folder picker.
    // Outputs: Mutates queued paths and queues a repaint when a folder is selected.
    void add_folder();

    // Purpose: Clear queued paths.
    // Inputs: None.
    // Outputs: Empties the queue and queues a repaint.
    void clear_queue();

    // Purpose: Clear in-memory operation history.
    // Inputs: None.
    // Outputs: Empties the session history and queues a repaint.
    void clear_history();

    // Purpose: Start a background SuperZip compression job from queued paths.
    // Inputs: None; reads queued paths from UI state.
    // Outputs: Launches a worker thread or no-ops when the queue is empty or another worker is running.
    void start_compress();

    // Purpose: Start a background SuperZip extraction job from the first queued archive.
    // Inputs: None; reads queued paths from UI state.
    // Outputs: Launches a worker thread or no-ops when the queue is empty or another worker is running.
    void start_extract();

    // Purpose: Run an archive operation on a single background worker.
    // Inputs: `job` is the work closure and `label` is the status text shown while it runs.
    // Outputs: Updates status/history/progress and queues repaints; catches worker exceptions into UI state.
    void run_job(std::function<void()> job, std::string label);

    // Purpose: Add an operation result line to the in-memory session history.
    // Inputs: `line` is a display string and should not include secrets.
    // Outputs: Mutates history state.
    void append_history(const std::string& line);

    // Purpose: Refresh visible AMD GPU status text.
    // Inputs: None.
    // Outputs: Mutates GPU status state using `query_gpu_info`.
    void refresh_gpu_status();

    // Purpose: Recreate native fonts at the current monitor DPI.
    // Inputs: None; reads `dpi_`.
    // Outputs: Replaces owned GDI font handles.
    void rebuild_fonts();

    // Purpose: Coalesce paint requests from worker and UI threads.
    // Inputs: None.
    // Outputs: Posts at most one pending repaint message until the UI thread processes it.
    void request_repaint();

    // Purpose: Scale design-space pixels to current physical pixels.
    // Inputs: `value` is a 96-DPI design coordinate or size.
    // Outputs: Returns the nearest Win32 `MulDiv` scaled integer for crisp layout.
    [[nodiscard]] int scale(int value) const;

    HWND hwnd_ = nullptr;
    HFONT title_font_ = nullptr;
    HFONT body_font_ = nullptr;
    HFONT small_font_ = nullptr;
    HFONT tiny_font_ = nullptr;
    HFONT mono_font_ = nullptr;
    UINT dpi_ = 96;
    std::mutex mutex_;
    UiState state_;
    std::thread worker_;
    std::atomic_bool worker_running_ = false;
    std::atomic_bool repaint_queued_ = false;
};

}  // namespace superzip::app
