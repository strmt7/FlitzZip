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

// Purpose: Construct a main window controller with default UI state.
// Inputs: None.
// Outputs: Initializes fields only; the native HWND is created by `run`.
MainWindow::MainWindow() = default;

// Purpose: Join worker threads and release GDI/GDI+ handles.
// Inputs: None.
// Outputs: Blocks until active work finishes, then frees owned native resources.
MainWindow::~MainWindow() {
    if (worker_.joinable()) {
        worker_.join();
    }
    shutdown_performance_monitor();
    for (HFONT font : {title_font_, body_font_, small_font_, tiny_font_, mono_font_}) {
        if (font != nullptr) {
            DeleteObject(font);
        }
    }
    if (gdiplus_token_ != 0) {
        Gdiplus::GdiplusShutdown(gdiplus_token_);
        gdiplus_token_ = 0;
    }
}

// Purpose: Register and show the native Win32 SuperZip window.
// Inputs: `instance` is the process HINSTANCE and `show_command` is the Windows show mode from `wWinMain`.
// Outputs: Runs the message loop and returns the process exit code.
int MainWindow::run(HINSTANCE instance, int show_command) {
    Gdiplus::GdiplusStartupInput gdiplus_startup_input{};
    if (Gdiplus::GdiplusStartup(&gdiplus_token_, &gdiplus_startup_input, nullptr) != Gdiplus::Ok) {
        gdiplus_token_ = 0;
        return 1;
    }

    refresh_gpu_status();
    const UINT initial_dpi = GetDpiForSystem();
    const DWORD style = window_style();
    RECT initial_window{0, 0, MulDiv(kDesignClientWidth, static_cast<int>(initial_dpi), 96),
                        MulDiv(kDesignClientHeight, static_cast<int>(initial_dpi), 96)};
    AdjustWindowRectExForDpi(&initial_window, style, FALSE, WS_EX_ACCEPTFILES, initial_dpi);
    const int initial_width = initial_window.right - initial_window.left;
    const int initial_height = initial_window.bottom - initial_window.top;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWindow::window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"SuperZipMainWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SUPERZIP_APP));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_SUPERZIP_APP), IMAGE_ICON,
                                               MulDiv(16, static_cast<int>(initial_dpi), 96),
                                               MulDiv(16, static_cast<int>(initial_dpi), 96), LR_DEFAULTCOLOR));
    HBRUSH class_background = CreateSolidBrush(kBg);
    wc.hbrBackground = class_background;
    if (RegisterClassExW(&wc) == 0) {
        if (class_background != nullptr) {
            DeleteObject(class_background);
        }
        return 1;
    }

    hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"SuperZip", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            initial_width, initial_height, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        UnregisterClassW(wc.lpszClassName, instance);
        if (class_background != nullptr) {
            DeleteObject(class_background);
        }
        return 1;
    }

    BOOL dark = TRUE;
    COLORREF caption = RGB(31, 31, 31);
    COLORREF caption_text = RGB(238, 241, 245);
    COLORREF border = RGB(54, 72, 78);
    (void)DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    (void)DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    (void)DwmSetWindowAttribute(hwnd_, DWMWA_TEXT_COLOR, &caption_text, sizeof(caption_text));
    (void)DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));

    dpi_ = GetDpiForWindow(hwnd_);
    rebuild_fonts();

    ShowWindow(hwnd_, show_command);
    UpdateWindow(hwnd_);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    const int exit_code = static_cast<int>(msg.wParam);
    UnregisterClassW(wc.lpszClassName, instance);
    if (class_background != nullptr) {
        DeleteObject(class_background);
    }
    return exit_code;
}

// Purpose: Route Win32 messages from the static window procedure to the C++ instance.
// Inputs: Standard Win32 `hwnd`, `message`, `wparam`, and `lparam` arguments.
// Outputs: Returns the message result expected by Win32.
LRESULT CALLBACK MainWindow::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    MainWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) {
        return self->handle_message(message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

// Purpose: Dispatch Win32 messages to narrow handlers while preserving default processing.
// Inputs: `message`, `wparam`, and `lparam` are the native window message payload.
// Outputs: Returns the Win32 message result for handled or defaulted messages.
LRESULT MainWindow::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_DPICHANGED: {
        // Keep the fixed design client area while honoring Windows' suggested
        // monitor position for the new DPI.
        dpi_ = HIWORD(wparam);
        rebuild_fonts();
        const auto* suggested = reinterpret_cast<RECT*>(lparam);
        RECT fixed_window{0, 0, MulDiv(kDesignClientWidth, static_cast<int>(dpi_), 96),
                          MulDiv(kDesignClientHeight, static_cast<int>(dpi_), 96)};
        AdjustWindowRectExForDpi(&fixed_window, window_style(), FALSE, WS_EX_ACCEPTFILES, dpi_);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, fixed_window.right - fixed_window.left,
                     fixed_window.bottom - fixed_window.top, SWP_NOZORDER | SWP_NOACTIVATE);
        request_repaint();
        return 0;
    }
    case WM_PAINT:
        paint();
        return 0;
    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) {
            window_was_minimized_ = true;
            repaint_queued_ = false;
            return 0;
        }
        if (window_was_minimized_) {
            window_was_minimized_ = false;
            repaint_queued_ = false;
            RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        }
        return 0;
    case WM_PRINT:
    case WM_PRINTCLIENT: {
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        layout_and_draw(reinterpret_cast<HDC>(wparam), rect);
        return 0;
    }
    case WM_ERASEBKGND: {
        return 1;
    }
    case WM_MOUSEMOVE:
        return handle_mouse_move(lparam);
    case WM_MOUSEWHEEL:
        return handle_mouse_wheel(wparam, lparam);
    case WM_MOUSELEAVE:
        return handle_mouse_leave();
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTTAB | DLGC_WANTCHARS;
    case WM_KEYDOWN:
        return handle_key_down(wparam, lparam);
    case WM_LBUTTONDOWN:
        return handle_primary_mouse_down(lparam);
    case WM_LBUTTONUP:
        return handle_primary_mouse_up(lparam);
    case WM_CAPTURECHANGED:
        return handle_capture_changed();
    case WM_DROPFILES:
        return handle_drop_files(wparam);
    case WM_CREATE:
        return handle_create();
    case WM_APP + 1:
        repaint_queued_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        return handle_timer(wparam);
    case WM_DESTROY:
        return handle_destroy();
    default:
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }
}

// Purpose: Track pointer hover state and arm native leave notifications.
// Inputs: `lparam` contains client-coordinate mouse position from `WM_MOUSEMOVE`.
// Outputs: Updates hover state and returns the handled Win32 result.

}  // namespace superzip::app
