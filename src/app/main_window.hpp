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
    // Outputs: Writes background, sidebar, and content pixels into `dc`.
    void layout_and_draw(HDC dc, const RECT& rect);

    // Purpose: Draw the navigation sidebar.
    // Inputs: `dc` is the target device context and `rect` is the sidebar rectangle in physical pixels.
    // Outputs: Renders navigation state into `dc`.
    void draw_sidebar(HDC dc, const RECT& rect);

    // Purpose: Draw the active content page.
    // Inputs: `dc` is the target device context and `rect` is the content rectangle in physical pixels.
    // Outputs: Renders queue, security, history, GPU, or preferences content into `dc`.
    void draw_content(HDC dc, const RECT& rect);

    // Purpose: Draw a simple DPI-scaled command or navigation button.
    // Inputs: `dc` is the target, `rect` is the button rectangle, `text` is display text, and `active` selects accent styling.
    // Outputs: Renders the button into `dc`; text is ellipsized rather than overflowing.
    void draw_button(HDC dc, const RECT& rect, const wchar_t* text, bool active);

    // Purpose: Draw a DPI-scaled opt-in settings toggle row.
    // Inputs: `dc` is the target, `rect` is the row rectangle, `text` is display text, and `enabled` selects checked styling.
    // Outputs: Renders the toggle and label into `dc`.
    void draw_toggle(HDC dc, const RECT& rect, const wchar_t* text, bool enabled);

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
    UINT dpi_ = 96;
    std::mutex mutex_;
    UiState state_;
    std::thread worker_;
    std::atomic_bool worker_running_ = false;
    std::atomic_bool repaint_queued_ = false;
};

}  // namespace superzip::app
