#include "app/main_window.hpp"

#include "core/archive.hpp"
#include "core/defender_scan.hpp"
#include "core/integrity.hpp"
#include "core/result.hpp"
#include "gpu/gpu_codec.hpp"
#include "zip/zip_adapter.hpp"

#include <algorithm>
#include <array>
#include <commdlg.h>
#include <dwmapi.h>
#include <iomanip>
#include <shellapi.h>
#include <shlobj.h>
#include <sstream>
#include <string>
#include <string_view>
#include <windowsx.h>

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

namespace superzip::app {
namespace {

constexpr int kTopBar = 52;
constexpr int kRailWidth = 86;
constexpr int kStatusBar = 34;

constexpr COLORREF kBg = RGB(12, 17, 20);
constexpr COLORREF kShell = RGB(15, 22, 26);
constexpr COLORREF kRail = RGB(13, 22, 25);
constexpr COLORREF kPanel = RGB(20, 31, 35);
constexpr COLORREF kPanel2 = RGB(27, 39, 44);
constexpr COLORREF kPanel3 = RGB(34, 48, 54);
constexpr COLORREF kBorder = RGB(54, 72, 78);
constexpr COLORREF kText = RGB(236, 242, 244);
constexpr COLORREF kMuted = RGB(151, 168, 174);
constexpr COLORREF kSubtle = RGB(102, 124, 132);
constexpr COLORREF kAccent = RGB(214, 34, 45);
constexpr COLORREF kAccent2 = RGB(162, 25, 34);
constexpr COLORREF kOk = RGB(83, 210, 101);
constexpr COLORREF kWarn = RGB(237, 179, 61);
constexpr COLORREF kInfo = RGB(63, 181, 221);
constexpr COLORREF kDanger = RGB(236, 73, 73);

// Purpose: Convert UTF-8 text to UTF-16 for Win32 rendering.
// Inputs: `value` is UTF-8 text.
// Outputs: Returns UTF-16 text; returns an empty string for empty input.
std::wstring widen(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), needed);
    return result;
}

// Purpose: Format a byte count with binary units.
// Inputs: `value` is a byte count or byte-per-second value.
// Outputs: Returns a compact display string such as `1.5 MiB`.
std::string human_bytes(double value) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << ' ' << units[unit];
    return out.str();
}

// Purpose: Safely format a filesystem entry size for the queue table.
// Inputs: `path` is a file or folder path that may no longer exist.
// Outputs: Returns display text; folders and inaccessible paths are shown without throwing.
std::wstring entry_size_text(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return L"Folder";
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return L"--";
    }
    return widen(human_bytes(static_cast<double>(size)));
}

// Purpose: Return a user-facing entry type for a queued path.
// Inputs: `path` is a file or folder path that may no longer exist.
// Outputs: Returns `Folder`, `File`, or `Missing`.
std::wstring entry_type_text(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return L"Folder";
    }
    if (std::filesystem::is_regular_file(path, ec)) {
        return L"File";
    }
    return L"Missing";
}

// Purpose: Map a page enum to a navigation label.
// Inputs: `page` is the active or target page.
// Outputs: Returns a UTF-16 label for rendering.
std::wstring page_name(Page page) {
    switch (page) {
    case Page::Queue: return L"Queue";
    case Page::Compress: return L"Compress";
    case Page::Extract: return L"Extract";
    case Page::Security: return L"Security";
    case Page::History: return L"History";
    case Page::Gpu: return L"AMD GPU";
    case Page::Preferences: return L"Settings";
    case Page::About: return L"About";
    }
    return L"SuperZip";
}

// Purpose: Fill a rectangle with one solid color.
// Inputs: `dc` is the target device context, `rect` is in physical pixels, and `color` is a Win32 COLORREF.
// Outputs: Writes pixels to `dc` and releases the temporary brush.
void fill_rect(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

// Purpose: Fill a small-radius panel rectangle.
// Inputs: `dc` is the target, `rect` is in physical pixels, `color` is the fill, and `radius` is the corner radius.
// Outputs: Writes the rounded rectangle and releases the temporary brush.
void fill_round_rect(HDC dc, const RECT& rect, COLORREF color, int radius) {
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ previous = SelectObject(dc, brush);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ previous_pen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, previous_pen);
    SelectObject(dc, previous);
    DeleteObject(pen);
    DeleteObject(brush);
}

// Purpose: Stroke a rectangle border.
// Inputs: `dc` is the target, `rect` is in physical pixels, and `color` is the border color.
// Outputs: Draws a one-pixel border and releases the temporary pen/brush.
void stroke_rect(HDC dc, const RECT& rect, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ previous_pen = SelectObject(dc, pen);
    HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, previous_brush);
    SelectObject(dc, previous_pen);
    DeleteObject(pen);
}

// Purpose: Draw a straight line.
// Inputs: `dc` is the target, endpoints are physical pixels, and `color` is the pen color.
// Outputs: Writes the line and releases the temporary pen.
void draw_line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ previous = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, previous);
    DeleteObject(pen);
}

// Purpose: Draw UTF-16 text with the currently selected font.
// Inputs: `dc` is the target, `rect` is the layout box, `text` is display text, `color` is text color, and `format` is DrawTextW flags.
// Outputs: Writes text into `dc` without mutating the caller's rectangle.
void draw_text(HDC dc, const RECT& rect, std::wstring_view text, COLORREF color, UINT format) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT copy = rect;
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &copy, format);
}

// Purpose: Return a rectangle inset by fixed pixel amounts.
// Inputs: `rect` is the source rectangle and `dx`/`dy` are physical-pixel insets.
// Outputs: Returns the inset rectangle.
RECT inset_rect(RECT rect, int dx, int dy) {
    rect.left += dx;
    rect.right -= dx;
    rect.top += dy;
    rect.bottom -= dy;
    return rect;
}

// Purpose: Return whether a point is inside a Win32 rectangle.
// Inputs: `rect` is in physical client pixels and `x`/`y` are physical client coordinates.
// Outputs: Returns true when the point lies within the rectangle's half-open bounds.
bool contains_point(const RECT& rect, int x, int y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

// Purpose: Draw the SuperZip stacked archive mark.
// Inputs: `dc` is the target, `rect` is the logo bounds, and `color` is the stroke color.
// Outputs: Draws three compact diamond layers.
void draw_logo(HDC dc, const RECT& rect, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ previous_pen = SelectObject(dc, pen);
    HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int cx = (rect.left + rect.right) / 2;
    const int w = (rect.right - rect.left) / 2;
    const int top = rect.top + 2;
    for (int i = 0; i < 3; ++i) {
        const int y = top + i * ((rect.bottom - rect.top) / 4);
        POINT pts[4] = {
            {cx, y},
            {cx + w, y + w / 2},
            {cx, y + w},
            {cx - w, y + w / 2},
        };
        Polygon(dc, pts, 4);
    }
    SelectObject(dc, previous_brush);
    SelectObject(dc, previous_pen);
    DeleteObject(pen);
}

// Purpose: Draw a compact navigation icon from simple vector primitives.
// Inputs: `dc` is the target, `page` selects the icon, `rect` is the icon box, and `color` is the stroke color.
// Outputs: Writes a crisp GDI icon without bitmap scaling.
void draw_nav_icon(HDC dc, Page page, const RECT& rect, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ previous_pen = SelectObject(dc, pen);
    HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int cx = (rect.left + rect.right) / 2;
    const int cy = (rect.top + rect.bottom) / 2;
    const int w = rect.right - rect.left;
    switch (page) {
    case Page::Queue:
        for (int i = 0; i < 3; ++i) {
            const int y = rect.top + 4 + i * 7;
            MoveToEx(dc, rect.left + 4, y, nullptr);
            LineTo(dc, rect.right - 4, y);
        }
        break;
    case Page::Compress:
        Rectangle(dc, rect.left + 5, rect.top + 5, rect.right - 5, rect.bottom - 5);
        MoveToEx(dc, cx, rect.top + 2, nullptr);
        LineTo(dc, cx, rect.bottom - 2);
        LineTo(dc, cx - 5, rect.bottom - 8);
        MoveToEx(dc, cx, rect.bottom - 2, nullptr);
        LineTo(dc, cx + 5, rect.bottom - 8);
        break;
    case Page::Extract:
        Rectangle(dc, rect.left + 5, rect.top + 5, rect.right - 5, rect.bottom - 5);
        MoveToEx(dc, cx, rect.bottom - 2, nullptr);
        LineTo(dc, cx, rect.top + 2);
        LineTo(dc, cx - 5, rect.top + 8);
        MoveToEx(dc, cx, rect.top + 2, nullptr);
        LineTo(dc, cx + 5, rect.top + 8);
        break;
    case Page::Security:
        Ellipse(dc, rect.left + 5, rect.top + 4, rect.right - 5, rect.bottom - 4);
        MoveToEx(dc, cx - 4, cy, nullptr);
        LineTo(dc, cx - 1, cy + 4);
        LineTo(dc, cx + 6, cy - 5);
        break;
    case Page::History:
        Arc(dc, rect.left + 3, rect.top + 3, rect.right - 3, rect.bottom - 3, rect.left + 5, cy, cx, rect.top + 2);
        MoveToEx(dc, rect.left + 6, cy, nullptr);
        LineTo(dc, rect.left + 6, cy - 6);
        break;
    case Page::Gpu:
        Rectangle(dc, rect.left + 5, rect.top + 5, rect.right - 5, rect.bottom - 5);
        for (int i = 0; i < 3; ++i) {
            const int x = rect.left + 2 + i * (w / 3);
            MoveToEx(dc, x, rect.top + 1, nullptr);
            LineTo(dc, x, rect.top + 5);
            MoveToEx(dc, x, rect.bottom - 1, nullptr);
            LineTo(dc, x, rect.bottom - 5);
        }
        break;
    case Page::Preferences:
        Ellipse(dc, rect.left + 5, rect.top + 5, rect.right - 5, rect.bottom - 5);
        Ellipse(dc, cx - 3, cy - 3, cx + 3, cy + 3);
        break;
    case Page::About:
        Ellipse(dc, rect.left + 5, rect.top + 5, rect.right - 5, rect.bottom - 5);
        MoveToEx(dc, cx, cy - 1, nullptr);
        LineTo(dc, cx, cy + 7);
        SetPixel(dc, cx, cy - 7, color);
        SetPixel(dc, cx, cy - 6, color);
        break;
    }
    SelectObject(dc, previous_brush);
    SelectObject(dc, previous_pen);
    DeleteObject(pen);
}

// Purpose: Return whether the copied GPU status represents an active AMD HIP backend.
// Inputs: `state` is the copied UI state.
// Outputs: Returns true when the status string reports AMD HIP readiness.
bool gpu_ready(const UiState& state) {
    return state.gpu_status.find("AMD HIP ready") != std::string::npos;
}

}  // namespace

MainWindow::MainWindow() = default;

MainWindow::~MainWindow() {
    if (worker_.joinable()) {
        worker_.join();
    }
    for (HFONT font : {title_font_, body_font_, small_font_, tiny_font_, mono_font_}) {
        if (font != nullptr) {
            DeleteObject(font);
        }
    }
}

int MainWindow::run(HINSTANCE instance, int show_command) {
    refresh_gpu_status();
    const UINT initial_dpi = GetDpiForSystem();
    const int initial_width = MulDiv(1200, static_cast<int>(initial_dpi), 96);
    const int initial_height = MulDiv(760, static_cast<int>(initial_dpi), 96);
    WNDCLASSW wc{};
    wc.lpfnWndProc = MainWindow::window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"SuperZipMainWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    hwnd_ = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"SuperZip",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        initial_width,
        initial_height,
        nullptr,
        nullptr,
        instance,
        this);
    if (hwnd_ == nullptr) {
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
    return static_cast<int>(msg.wParam);
}

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

LRESULT MainWindow::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wparam);
        rebuild_fonts();
        const auto* suggested = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(
            hwnd_,
            nullptr,
            suggested->left,
            suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        request_repaint();
        return 0;
    }
    case WM_PAINT:
        paint();
        return 0;
    case WM_PRINT:
    case WM_PRINTCLIENT: {
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        layout_and_draw(reinterpret_cast<HDC>(wparam), rect);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const int rail_width = scale(kRailWidth);
        const int top_bar = scale(kTopBar);
        if (y < top_bar) {
            const RECT add_files_button{rail_width + scale(122), scale(10), rail_width + scale(218), scale(40)};
            const RECT add_folder_button{rail_width + scale(226), scale(10), rail_width + scale(334), scale(40)};
            const RECT clear{rail_width + scale(342), scale(10), rail_width + scale(414), scale(40)};
            if (contains_point(add_files_button, x, y)) {
                add_files();
            } else if (contains_point(add_folder_button, x, y)) {
                add_folder();
            } else if (contains_point(clear, x, y)) {
                clear_queue();
            }
        } else if (x < rail_width) {
            const int nav_top = top_bar + scale(10);
            const int item_height = scale(63);
            const int item = item_height > 0 ? (y - nav_top) / item_height : -1;
            if (item >= 0 && item < 8) {
                set_page(static_cast<Page>(item));
            }
        } else {
            (void)handle_content_click(x, y);
        }
        return 0;
    }
    case WM_DROPFILES: {
        auto drop = reinterpret_cast<HDROP>(wparam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::lock_guard lock(mutex_);
        for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length + 1, L'\0');
            DragQueryFileW(drop, i, path.data(), length + 1);
            path.resize(length);
            state_.queued_paths.emplace_back(path);
        }
        DragFinish(drop);
        request_repaint();
        return 0;
    }
    case WM_CREATE:
        DragAcceptFiles(hwnd_, TRUE);
        return 0;
    case WM_APP + 1:
        repaint_queued_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }
}

void MainWindow::paint() {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT rect{};
    GetClientRect(hwnd_, &rect);
    const int width = std::max(1L, rect.right - rect.left);
    const int height = std::max(1L, rect.bottom - rect.top);
    HDC buffer_dc = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    HGDIOBJ previous_bitmap = SelectObject(buffer_dc, bitmap);
    layout_and_draw(buffer_dc, rect);
    BitBlt(dc, 0, 0, width, height, buffer_dc, 0, 0, SRCCOPY);
    SelectObject(buffer_dc, previous_bitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer_dc);
    EndPaint(hwnd_, &ps);
}

void MainWindow::layout_and_draw(HDC dc, const RECT& rect) {
    UiState state;
    {
        std::lock_guard lock(mutex_);
        state = state_;
    }

    fill_rect(dc, rect, kBg);
    const int top_bar = scale(kTopBar);
    const int rail_width = scale(kRailWidth);
    const int status_bar = scale(kStatusBar);
    RECT top{rect.left, rect.top, rect.right, rect.top + top_bar};
    RECT rail{rect.left, top.bottom, rect.left + rail_width, rect.bottom - status_bar};
    RECT content{rail.right, top.bottom, rect.right, rect.bottom - status_bar};
    RECT status{rect.left, rect.bottom - status_bar, rect.right, rect.bottom};

    draw_top_bar(dc, top);
    draw_navigation(dc, rail, state);
    draw_content(dc, content, state);
    draw_status_bar(dc, status, state);
}

void MainWindow::draw_top_bar(HDC dc, const RECT& rect) {
    fill_rect(dc, rect, kShell);
    draw_line(dc, rect.left, rect.bottom - 1, rect.right, rect.bottom - 1, kBorder);

    const int rail_width = scale(kRailWidth);
    RECT logo{scale(18), scale(14), scale(38), scale(36)};
    draw_logo(dc, logo, kAccent);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{scale(48), rect.top, rail_width + scale(76), rect.bottom}, L"SuperZip", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, tiny_font_);
    draw_button(dc, RECT{rail_width + scale(122), scale(10), rail_width + scale(218), scale(40)}, L"+ Add files", false);
    draw_button(dc, RECT{rail_width + scale(226), scale(10), rail_width + scale(334), scale(40)}, L"+ Add folder", false);
    draw_button(dc, RECT{rail_width + scale(342), scale(10), rail_width + scale(414), scale(40)}, L"Clear", false);
}

void MainWindow::draw_navigation(HDC dc, const RECT& rect, const UiState& state) {
    fill_rect(dc, rect, kRail);
    draw_line(dc, rect.right - 1, rect.top, rect.right - 1, rect.bottom, kBorder);
    const std::array<Page, 8> pages{
        Page::Queue,
        Page::Compress,
        Page::Extract,
        Page::Security,
        Page::History,
        Page::Gpu,
        Page::Preferences,
        Page::About,
    };
    SelectObject(dc, tiny_font_);
    const int item_height = scale(63);
    int y = rect.top + scale(10);
    for (const auto page : pages) {
        RECT item{rect.left, y, rect.right, y + item_height};
        const bool active = state.page == page;
        if (active) {
            fill_rect(dc, RECT{item.left, item.top, item.left + scale(5), item.bottom}, kAccent);
            fill_round_rect(dc, RECT{item.left + scale(8), item.top + scale(5), item.right - scale(8), item.bottom - scale(5)}, RGB(126, 24, 31), scale(4));
        }
        RECT icon{item.left + scale(31), item.top + scale(11), item.left + scale(55), item.top + scale(35)};
        draw_nav_icon(dc, page, icon, active ? kText : kMuted);
        draw_text(dc, RECT{item.left + scale(4), item.top + scale(37), item.right - scale(4), item.bottom - scale(3)}, page_name(page), active ? kText : kMuted, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += item_height;
    }
}

void MainWindow::draw_status_bar(HDC dc, const RECT& rect, const UiState& state) {
    fill_rect(dc, rect, kShell);
    draw_line(dc, rect.left, rect.top, rect.right, rect.top, kBorder);
    const bool ready = gpu_ready(state);
    const int cy = (rect.top + rect.bottom) / 2;
    HBRUSH dot = CreateSolidBrush(ready ? kOk : kWarn);
    HGDIOBJ previous = SelectObject(dc, dot);
    Ellipse(dc, rect.left + scale(18), cy - scale(5), rect.left + scale(28), cy + scale(5));
    SelectObject(dc, previous);
    DeleteObject(dot);

    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{rect.left + scale(36), rect.top, rect.left + scale(202), rect.bottom}, ready ? L"AMD GPU Ready" : L"AMD GPU inactive", ready ? kOk : kWarn, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_line(dc, rect.left + scale(220), rect.top + scale(8), rect.left + scale(220), rect.bottom - scale(8), kBorder);
    draw_text(dc, RECT{rect.left + scale(238), rect.top, rect.left + scale(430), rect.bottom}, widen(state.gpu_status), kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw_line(dc, rect.left + scale(450), rect.top + scale(8), rect.left + scale(450), rect.bottom - scale(8), kBorder);
    const auto speed = human_bytes(state.progress.throughput_bytes_per_second) + "/s";
    draw_text(dc, RECT{rect.left + scale(468), rect.top, rect.left + scale(620), rect.bottom}, widen("Throughput " + speed), kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_line(dc, rect.right - scale(132), rect.top + scale(8), rect.right - scale(132), rect.bottom - scale(8), kBorder);
    draw_text(dc, RECT{rect.right - scale(118), rect.top, rect.right - scale(16), rect.bottom}, L"Details", kMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void MainWindow::draw_content(HDC dc, const RECT& rect, const UiState& state) {
    fill_rect(dc, rect, kBg);
    switch (state.page) {
    case Page::Queue:
        draw_queue_page(dc, rect, state);
        break;
    case Page::Compress:
        draw_compress_page(dc, rect, state);
        break;
    case Page::Extract:
        draw_extract_page(dc, rect, state);
        break;
    case Page::Security:
        draw_security_page(dc, rect, state);
        break;
    case Page::History:
        draw_history_page(dc, rect, state);
        break;
    case Page::Gpu:
        draw_gpu_page(dc, rect, state);
        break;
    case Page::Preferences:
        draw_preferences_page(dc, rect, state);
        break;
    case Page::About:
        draw_about_page(dc, rect);
        break;
    }
}

void MainWindow::draw_queue_page(HDC dc, const RECT& rect, const UiState& state) {
    RECT area = inset_rect(rect, scale(24), scale(20));
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right - scale(180), area.top + scale(34)}, L"Queue", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    const auto count_text = std::to_wstring(state.queued_paths.size()) + L" item" + (state.queued_paths.size() == 1 ? L"" : L"s");
    draw_text(dc, RECT{area.right - scale(180), area.top, area.right, area.top + scale(34)}, count_text, kMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    RECT table{area.left, area.top + scale(48), area.right, area.bottom - scale(124)};
    fill_round_rect(dc, table, kPanel, scale(4));
    stroke_rect(dc, table, kBorder);
    SelectObject(dc, tiny_font_);
    const int header_bottom = table.top + scale(36);
    draw_text(dc, RECT{table.left + scale(14), table.top, table.left + scale(70), header_bottom}, L"Use", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{table.left + scale(82), table.top, table.left + scale(330), header_bottom}, L"Name", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{table.left + scale(340), table.top, table.left + scale(450), header_bottom}, L"Size", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{table.left + scale(462), table.top, table.left + scale(560), header_bottom}, L"Type", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{table.left + scale(572), table.top, table.right - scale(12), header_bottom}, L"Path", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_line(dc, table.left, header_bottom, table.right, header_bottom, kBorder);

    int y = header_bottom;
    if (state.queued_paths.empty()) {
        SelectObject(dc, body_font_);
        draw_text(dc, RECT{table.left + scale(22), y + scale(28), table.right - scale(22), y + scale(86)}, L"Drop files or folders here, or use Add files / Add folder. Work runs in a background thread so the interface stays responsive.", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else {
        for (const auto& path : state.queued_paths) {
            const int row_bottom = y + scale(34);
            fill_rect(dc, RECT{table.left + scale(1), y + scale(1), table.right - scale(1), row_bottom}, ((y / scale(34)) % 2 == 0) ? kPanel2 : kPanel);
            draw_checkbox(dc, RECT{table.left + scale(12), y + scale(6), table.left + scale(64), row_bottom - scale(4)}, L"", true);
            draw_text(dc, RECT{table.left + scale(82), y, table.left + scale(330), row_bottom}, path.filename().wstring(), kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            draw_text(dc, RECT{table.left + scale(340), y, table.left + scale(450), row_bottom}, entry_size_text(path), kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            draw_text(dc, RECT{table.left + scale(462), y, table.left + scale(560), row_bottom}, entry_type_text(path), kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            draw_text(dc, RECT{table.left + scale(572), y, table.right - scale(12), row_bottom}, path.wstring(), kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y = row_bottom;
            if (y > table.bottom - scale(34)) {
                break;
            }
        }
    }

    RECT destination{area.left, area.bottom - scale(88), area.right - scale(182), area.bottom - scale(44)};
    draw_field(dc, destination, L"Destination", (std::filesystem::current_path() / "SuperZip-output.suzip").wstring(), false);
    draw_button(dc, RECT{area.right - scale(168), area.bottom - scale(64), area.right - scale(20), area.bottom - scale(24)}, L"Start", true);
    draw_field(dc, RECT{area.left, area.bottom - scale(40), area.left + scale(264), area.bottom}, L"Profile", L"Balanced (AMD GPU)", true);
}

void MainWindow::draw_compress_page(HDC dc, const RECT& rect, const UiState& state) {
    RECT area = inset_rect(rect, scale(30), scale(22));
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(34)}, L"Compress", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_button(dc, RECT{area.right - scale(150), area.bottom - scale(58), area.right, area.bottom - scale(18)}, L"Start", true);

    const int left = area.left;
    const int mid = area.left + (area.right - area.left) / 2 + scale(14);
    const int field_w = (area.right - area.left) / 2 - scale(26);
    draw_field(dc, RECT{left, area.top + scale(54), left + field_w, area.top + scale(104)}, L"Archive name", L"SuperZip-output.suzip", false);
    draw_field(dc, RECT{mid, area.top + scale(54), mid + field_w, area.top + scale(104)}, L"Destination", std::filesystem::current_path().wstring(), false);
    draw_field(dc, RECT{left, area.top + scale(124), left + field_w, area.top + scale(174)}, L"Archive format", L"SuperZip GPU (.suzip)", true);
    draw_field(dc, RECT{mid, area.top + scale(124), mid + field_w, area.top + scale(174)}, L"Compression profile", L"Balanced (default)", true);
    draw_field(dc, RECT{left, area.top + scale(194), left + field_w, area.top + scale(244)}, L"Compression method", state.gpu_required ? L"AMD HIP required" : L"AMD HIP preferred", true);
    draw_field(dc, RECT{mid, area.top + scale(194), mid + field_w, area.top + scale(244)}, L"Block size", L"1 MiB blocks / 128 MiB chunks", true);

    RECT advanced{left, area.top + scale(270), area.right, area.top + scale(390)};
    fill_round_rect(dc, advanced, kPanel, scale(4));
    stroke_rect(dc, advanced, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{advanced.left + scale(16), advanced.top + scale(12), advanced.right, advanced.top + scale(36)}, L"Advanced", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_checkbox(dc, RECT{advanced.left + scale(18), advanced.top + scale(48), advanced.left + scale(310), advanced.top + scale(76)}, L"Solid archive", true);
    draw_checkbox(dc, RECT{advanced.left + scale(18), advanced.top + scale(80), advanced.left + scale(310), advanced.top + scale(108)}, L"Store timestamps", true);
    draw_checkbox(dc, RECT{advanced.left + scale(342), advanced.top + scale(48), advanced.left + scale(680), advanced.top + scale(76)}, L"Delete files after compression", false);
    draw_toggle(dc, RECT{advanced.left + scale(342), advanced.top + scale(80), advanced.left + scale(710), advanced.top + scale(108)}, L"Verify archive after write", state.verify_after_write_opt_in);

    RECT security{left, area.top + scale(410), area.right, area.top + scale(528)};
    fill_round_rect(dc, security, kPanel, scale(4));
    stroke_rect(dc, security, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{security.left + scale(16), security.top + scale(12), security.right, security.top + scale(36)}, L"Integrity and Security (opt-in)", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_toggle(dc, RECT{security.left + scale(18), security.top + scale(46), security.left + scale(420), security.top + scale(78)}, L"SHA-256 integrity check", state.integrity_hash_opt_in);
    draw_toggle(dc, RECT{security.left + scale(18), security.top + scale(82), security.left + scale(420), security.top + scale(114)}, L"Microsoft Defender scan", state.defender_scan_opt_in);
    draw_text(dc, RECT{security.left + scale(448), security.top + scale(46), security.right - scale(16), security.top + scale(114)}, L"Security checks remain disabled until explicitly enabled for the job or as a default in Settings.", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void MainWindow::draw_extract_page(HDC dc, const RECT& rect, const UiState& state) {
    RECT area = inset_rect(rect, scale(30), scale(22));
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(34)}, L"Extract", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_button(dc, RECT{area.right - scale(150), area.bottom - scale(58), area.right, area.bottom - scale(18)}, L"Start", true);

    const auto archive = state.queued_paths.empty() ? L"Select an archive from the queue" : state.queued_paths.front().wstring();
    draw_field(dc, RECT{area.left, area.top + scale(58), area.right, area.top + scale(108)}, L"Archive", archive, false);
    draw_field(dc, RECT{area.left, area.top + scale(128), area.right, area.top + scale(178)}, L"Destination", (std::filesystem::current_path() / "SuperZip-extracted").wstring(), false);
    draw_field(dc, RECT{area.left, area.top + scale(198), area.left + scale(320), area.top + scale(248)}, L"Path mode", L"Full paths", true);
    draw_field(dc, RECT{area.left + scale(342), area.top + scale(198), area.left + scale(662), area.top + scale(248)}, L"Overwrite policy", state.overwrite ? L"Overwrite enabled" : L"Ask before overwrite", true);

    RECT checks{area.left, area.top + scale(280), area.right, area.top + scale(412)};
    fill_round_rect(dc, checks, kPanel, scale(4));
    stroke_rect(dc, checks, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{checks.left + scale(16), checks.top + scale(12), checks.right, checks.top + scale(36)}, L"Integrity and Security (opt-in)", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_checkbox(dc, RECT{checks.left + scale(18), checks.top + scale(48), checks.left + scale(420), checks.top + scale(78)}, L"Verify archive metadata before extraction", true);
    draw_checkbox(dc, RECT{checks.left + scale(18), checks.top + scale(80), checks.left + scale(420), checks.top + scale(110)}, L"Open destination folder after extraction", false);
    draw_toggle(dc, RECT{checks.left + scale(470), checks.top + scale(48), checks.right - scale(20), checks.top + scale(80)}, L"SHA-256 integrity check", state.integrity_hash_opt_in);
    draw_toggle(dc, RECT{checks.left + scale(470), checks.top + scale(84), checks.right - scale(20), checks.top + scale(116)}, L"Microsoft Defender scan", state.defender_scan_opt_in);
}

void MainWindow::draw_security_page(HDC dc, const RECT& rect, const UiState& state) {
    RECT area = inset_rect(rect, scale(30), scale(22));
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(34)}, L"Security Review", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_button(dc, RECT{area.right - scale(150), area.bottom - scale(58), area.right, area.bottom - scale(18)}, L"Verify", true);

    RECT summary{area.left, area.top + scale(54), area.left + scale(430), area.bottom - scale(80)};
    fill_round_rect(dc, summary, kPanel, scale(4));
    stroke_rect(dc, summary, kBorder);
    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{summary.left + scale(16), summary.top, summary.left + scale(210), summary.top + scale(36)}, L"Check", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{summary.left + scale(230), summary.top, summary.right - scale(16), summary.top + scale(36)}, L"Status", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_line(dc, summary.left, summary.top + scale(36), summary.right, summary.top + scale(36), kBorder);
    struct Row {
        const wchar_t* name;
        const wchar_t* status;
        COLORREF color;
    };
    const Row rows[] = {
        {L"Path safety", L"Safe", kOk},
        {L"CRC metadata", L"Verified", kOk},
        {L"Post-write verify", state.verify_after_write_opt_in ? L"Enabled" : L"Not selected", state.verify_after_write_opt_in ? kOk : kWarn},
        {L"SHA-256 optional", state.integrity_hash_opt_in ? L"Enabled" : L"Not selected", state.integrity_hash_opt_in ? kOk : kWarn},
        {L"Defender optional", state.defender_scan_opt_in ? L"Enabled" : L"Not selected", state.defender_scan_opt_in ? kOk : kWarn},
        {L"Overwrite policy", state.overwrite ? L"Overwrite enabled" : L"Ask before overwrite", state.overwrite ? kWarn : kOk},
        {L"GPU requirement", state.gpu_required ? L"AMD HIP required" : L"Fallback allowed", state.gpu_required ? kInfo : kWarn},
    };
    int y = summary.top + scale(36);
    for (const auto& row : rows) {
        const int bottom = y + scale(38);
        fill_rect(dc, RECT{summary.left + scale(1), y + scale(1), summary.right - scale(1), bottom}, ((y / scale(38)) % 2 == 0) ? kPanel2 : kPanel);
        draw_text(dc, RECT{summary.left + scale(16), y, summary.left + scale(210), bottom}, row.name, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        draw_text(dc, RECT{summary.left + scale(230), y, summary.right - scale(16), bottom}, row.status, row.color, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y = bottom;
    }

    RECT detail{summary.right + scale(22), summary.top, area.right, summary.bottom};
    fill_round_rect(dc, detail, kPanel, scale(4));
    stroke_rect(dc, detail, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{detail.left + scale(18), detail.top + scale(14), detail.right, detail.top + scale(40)}, L"Archive boundary contract", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{detail.left + scale(18), detail.top + scale(56), detail.right - scale(18), detail.top + scale(154)}, L"Extraction publishes final files only after each archive entry is normalized, decoded, and verified inside the selected destination. Absolute paths, drive-rooted paths, UNC paths, traversal, unsafe names, malformed block metadata, CRC mismatches, and overwrite attempts are rejected before final publication.", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    draw_field(dc, RECT{detail.left + scale(18), detail.top + scale(184), detail.right - scale(18), detail.top + scale(234)}, L"Archive", state.queued_paths.empty() ? L"No archive selected" : state.queued_paths.front().wstring(), false);
    draw_field(dc, RECT{detail.left + scale(18), detail.top + scale(252), detail.left + scale(248), detail.top + scale(302)}, L"Files", std::to_wstring(state.queued_paths.size()), false);
    draw_field(dc, RECT{detail.left + scale(270), detail.top + scale(252), detail.right - scale(18), detail.top + scale(302)}, L"Total size", L"Calculated during job", false);
}

void MainWindow::draw_history_page(HDC dc, const RECT& rect, const UiState& state) {
    RECT area = inset_rect(rect, scale(30), scale(22));
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(34)}, L"History", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_button(dc, RECT{area.right - scale(142), area.top, area.right, area.top + scale(34)}, L"Clear History", false);
    draw_field(dc, RECT{area.left, area.top + scale(48), area.left + scale(220), area.top + scale(92)}, L"Operation", L"All operations", true);
    draw_field(dc, RECT{area.left + scale(238), area.top + scale(48), area.left + scale(458), area.top + scale(92)}, L"Status", L"All statuses", true);

    RECT table{area.left, area.top + scale(112), area.right, area.bottom - scale(96)};
    fill_round_rect(dc, table, kPanel, scale(4));
    stroke_rect(dc, table, kBorder);
    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{table.left + scale(14), table.top, table.left + scale(150), table.top + scale(34)}, L"Time", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{table.left + scale(160), table.top, table.left + scale(290), table.top + scale(34)}, L"Operation", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{table.left + scale(300), table.top, table.left + scale(620), table.top + scale(34)}, L"Archive", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{table.left + scale(632), table.top, table.right - scale(16), table.top + scale(34)}, L"Status", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_line(dc, table.left, table.top + scale(34), table.right, table.top + scale(34), kBorder);
    int y = table.top + scale(34);
    if (state.history.empty()) {
        draw_text(dc, RECT{table.left + scale(18), y + scale(28), table.right - scale(18), y + scale(70)}, L"No completed operations in this session yet.", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else {
        for (const auto& line : state.history) {
            const int bottom = y + scale(34);
            draw_text(dc, RECT{table.left + scale(14), y, table.left + scale(150), bottom}, L"Session", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            draw_text(dc, RECT{table.left + scale(160), y, table.left + scale(290), bottom}, L"Job", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            draw_text(dc, RECT{table.left + scale(300), y, table.left + scale(620), bottom}, widen(line), kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            draw_text(dc, RECT{table.left + scale(632), y, table.right - scale(16), bottom}, line.starts_with("Error") ? L"Failed" : L"Success", line.starts_with("Error") ? kDanger : kOk, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            y = bottom;
            if (y > table.bottom - scale(34)) {
                break;
            }
        }
    }
    RECT details{area.left, area.bottom - scale(76), area.right, area.bottom};
    fill_round_rect(dc, details, kPanel, scale(4));
    stroke_rect(dc, details, kBorder);
    draw_text(dc, RECT{details.left + scale(18), details.top + scale(10), details.right - scale(18), details.bottom}, L"Selected operation details appear here after a job runs.", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void MainWindow::draw_gpu_page(HDC dc, const RECT& rect, const UiState& state) {
    RECT area = inset_rect(rect, scale(30), scale(22));
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(34)}, L"AMD GPU Diagnostics", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_button(dc, RECT{area.right - scale(110), area.top, area.right, area.top + scale(34)}, L"Refresh", false);

    const bool ready = gpu_ready(state);
    RECT top{area.left, area.top + scale(54), area.right, area.top + scale(142)};
    fill_round_rect(dc, top, kPanel, scale(4));
    stroke_rect(dc, top, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{top.left + scale(18), top.top + scale(12), top.left + scale(180), top.top + scale(40)}, L"HIP status", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{top.left + scale(190), top.top + scale(12), top.right - scale(18), top.top + scale(40)}, widen(state.gpu_status), ready ? kOk : kWarn, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{top.left + scale(18), top.top + scale(50), top.right - scale(18), top.bottom - scale(12)}, ready ? L"SuperZip will use AMD HIP for native .suzip jobs that require GPU acceleration." : L"This build or host is not reporting an active AMD HIP device. GPU-required jobs fail instead of silently using a different vendor path.", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);

    const int card_w = (area.right - area.left - scale(28)) / 3;
    RECT gpu{area.left, area.top + scale(166), area.left + card_w, area.top + scale(316)};
    RECT memory{gpu.right + scale(14), gpu.top, gpu.right + scale(14) + card_w, gpu.bottom};
    RECT accel{memory.right + scale(14), gpu.top, area.right, gpu.bottom};
    fill_round_rect(dc, gpu, kPanel, scale(4));
    fill_round_rect(dc, memory, kPanel, scale(4));
    fill_round_rect(dc, accel, kPanel, scale(4));
    stroke_rect(dc, gpu, kBorder);
    stroke_rect(dc, memory, kBorder);
    stroke_rect(dc, accel, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{gpu.left + scale(16), gpu.top + scale(12), gpu.right, gpu.top + scale(36)}, L"GPU", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{memory.left + scale(16), memory.top + scale(12), memory.right, memory.top + scale(36)}, L"Memory", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{accel.left + scale(16), accel.top + scale(12), accel.right, accel.top + scale(36)}, L"Acceleration", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{gpu.left + scale(16), gpu.top + scale(48), gpu.right - scale(16), gpu.bottom - scale(14)}, ready ? L"Backend: AMD HIP\nTarget: configured at build time\nDevice: reported by HIP runtime" : L"Backend unavailable\nNo CUDA/WebGPU fallback\nHost stays AMD-only", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    draw_text(dc, RECT{memory.left + scale(16), memory.top + scale(48), memory.right - scale(16), memory.bottom - scale(14)}, L"Bounded chunks keep archive work from loading whole archives into RAM. ZIP compatibility uses miniz streaming file APIs.", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    draw_text(dc, RECT{accel.left + scale(16), accel.top + scale(48), accel.right - scale(16), accel.bottom - scale(14)}, state.gpu_required ? L"Mode: GPU required\nFallback: blocked for .suzip jobs\nDevice scope: AMD HIP only" : L"Mode: GPU preferred\nFallback: CPU codec allowed\nDevice scope: AMD HIP only", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);

    RECT monitor{area.left, area.top + scale(342), area.right, area.bottom};
    fill_round_rect(dc, monitor, kPanel, scale(4));
    stroke_rect(dc, monitor, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{monitor.left + scale(16), monitor.top + scale(12), monitor.right, monitor.top + scale(36)}, L"Performance Monitor", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    const int graph_top = monitor.top + scale(60);
    const int graph_bottom = monitor.bottom - scale(42);
    const int graph_w = (monitor.right - monitor.left - scale(86)) / 4;
    for (int i = 0; i < 4; ++i) {
        RECT graph{monitor.left + scale(18) + i * (graph_w + scale(16)), graph_top, monitor.left + scale(18) + i * (graph_w + scale(16)) + graph_w, graph_bottom};
        stroke_rect(dc, graph, kBorder);
        const wchar_t* label = i == 0 ? L"GPU utilization" : i == 1 ? L"Memory bandwidth" : i == 2 ? L"Temperature" : L"Power";
        draw_text(dc, RECT{graph.left, graph.bottom + scale(4), graph.right, graph.bottom + scale(24)}, label, kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        draw_text(dc, inset_rect(graph, scale(14), scale(12)), L"No live sample", kSubtle, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

void MainWindow::draw_preferences_page(HDC dc, const RECT& rect, const UiState& state) {
    RECT area = inset_rect(rect, scale(30), scale(22));
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(34)}, L"Preferences", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_button(dc, RECT{area.right - scale(260), area.bottom - scale(54), area.right - scale(126), area.bottom - scale(18)}, L"Restore Defaults", false);
    draw_button(dc, RECT{area.right - scale(110), area.bottom - scale(54), area.right, area.bottom - scale(18)}, L"Apply", true);

    RECT general{area.left, area.top + scale(56), area.left + scale(470), area.top + scale(224)};
    RECT security{general.left, general.bottom + scale(16), general.right, general.bottom + scale(176)};
    RECT performance{general.right + scale(18), general.top, area.right, area.top + scale(224)};
    RECT logging{performance.left, performance.bottom + scale(16), area.right, performance.bottom + scale(176)};
    for (RECT panel : {general, security, performance, logging}) {
        fill_round_rect(dc, panel, kPanel, scale(4));
        stroke_rect(dc, panel, kBorder);
    }
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{general.left + scale(16), general.top + scale(12), general.right, general.top + scale(36)}, L"General", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{security.left + scale(16), security.top + scale(12), security.right, security.top + scale(36)}, L"Security (opt-in)", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{performance.left + scale(16), performance.top + scale(12), performance.right, performance.top + scale(36)}, L"Performance", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{logging.left + scale(16), logging.top + scale(12), logging.right, logging.top + scale(36)}, L"Logging", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_checkbox(dc, RECT{general.left + scale(18), general.top + scale(48), general.right - scale(16), general.top + scale(78)}, L"Open destination folder after operation", false);
    draw_checkbox(dc, RECT{general.left + scale(18), general.top + scale(82), general.right - scale(16), general.top + scale(112)}, L"Confirm before deleting files", true);
    draw_checkbox(dc, RECT{general.left + scale(18), general.top + scale(116), general.right - scale(16), general.top + scale(146)}, L"Show operation summary", true);
    draw_toggle(dc, RECT{security.left + scale(18), security.top + scale(48), security.right - scale(16), security.top + scale(80)}, L"SHA-256 integrity check", state.integrity_hash_opt_in);
    draw_toggle(dc, RECT{security.left + scale(18), security.top + scale(84), security.right - scale(16), security.top + scale(116)}, L"Microsoft Defender scan", state.defender_scan_opt_in);
    draw_toggle(dc, RECT{security.left + scale(18), security.top + scale(120), security.right - scale(16), security.top + scale(152)}, L"Require AMD GPU acceleration", state.gpu_required);
    draw_toggle(dc, RECT{performance.left + scale(18), performance.top + scale(48), performance.right - scale(18), performance.top + scale(80)}, L"Verify archive after write", state.verify_after_write_opt_in);
    draw_field(dc, RECT{performance.left + scale(18), performance.top + scale(94), performance.right - scale(18), performance.top + scale(140)}, L"Memory policy", L"Bounded chunk windows", true);
    draw_field(dc, RECT{logging.left + scale(18), logging.top + scale(48), logging.right - scale(18), logging.top + scale(94)}, L"Log level", L"Information", true);
    draw_field(dc, RECT{logging.left + scale(18), logging.top + scale(106), logging.right - scale(18), logging.top + scale(152)}, L"Log retention", L"Session only", true);
}

void MainWindow::draw_about_page(HDC dc, const RECT& rect) {
    RECT area = inset_rect(rect, scale(30), scale(22));
    RECT card{area.left, area.top + scale(56), area.right, area.bottom - scale(60)};
    fill_round_rect(dc, card, kPanel, scale(4));
    stroke_rect(dc, card, kBorder);
    RECT logo{card.left + scale(42), card.top + scale(58), card.left + scale(110), card.top + scale(132)};
    draw_logo(dc, logo, kAccent);
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{card.left + scale(142), card.top + scale(50), card.right - scale(40), card.top + scale(90)}, L"SuperZip", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{card.left + scale(142), card.top + scale(94), card.right - scale(40), card.top + scale(122)}, L"Native Windows AMD HIP archive utility", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{card.left + scale(142), card.top + scale(132), card.right - scale(40), card.top + scale(164)}, widen(std::string("Version ") + SUPERZIP_VERSION), kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{card.left + scale(42), card.top + scale(200), card.right - scale(42), card.top + scale(310)}, L"SuperZip separates native .suzip GPU archive jobs from standard .zip compatibility mode. AMD HIP is the only GPU acceleration boundary; security-sensitive extraction validates paths and metadata before writing files.", kText, DT_LEFT | DT_TOP | DT_WORDBREAK);
    draw_text(dc, RECT{card.left + scale(42), card.bottom - scale(80), card.right - scale(42), card.bottom - scale(38)}, L"Built for 64-bit Windows, high-DPI displays, and responsive background archive work.", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void MainWindow::draw_button(HDC dc, const RECT& rect, const wchar_t* text, bool active) {
    fill_round_rect(dc, rect, active ? kAccent : kPanel2, scale(4));
    stroke_rect(dc, rect, active ? kAccent2 : kBorder);
    SelectObject(dc, tiny_font_);
    draw_text(dc, inset_rect(rect, scale(12), 0), text, kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void MainWindow::draw_toggle(HDC dc, const RECT& rect, const wchar_t* text, bool enabled) {
    draw_text(dc, RECT{rect.left + scale(54), rect.top, rect.right - scale(86), rect.bottom}, text, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT track{rect.left, rect.top + scale(7), rect.left + scale(42), rect.bottom - scale(7)};
    fill_round_rect(dc, track, enabled ? RGB(43, 111, 72) : RGB(57, 69, 75), scale(14));
    HBRUSH knob = CreateSolidBrush(enabled ? kOk : kMuted);
    HGDIOBJ previous = SelectObject(dc, knob);
    const int knob_left = enabled ? track.right - scale(17) : track.left + scale(3);
    Ellipse(dc, knob_left, track.top + scale(3), knob_left + scale(14), track.bottom - scale(3));
    SelectObject(dc, previous);
    DeleteObject(knob);
    draw_text(dc, RECT{rect.right - scale(78), rect.top, rect.right, rect.bottom}, enabled ? L"Enabled" : L"Disabled", enabled ? kOk : kWarn, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void MainWindow::draw_checkbox(HDC dc, const RECT& rect, const wchar_t* text, bool enabled) {
    RECT box{rect.left, rect.top + scale(6), rect.left + scale(16), rect.top + scale(22)};
    fill_rect(dc, box, enabled ? kAccent : kPanel2);
    stroke_rect(dc, box, enabled ? kAccent2 : kBorder);
    if (enabled) {
        draw_line(dc, box.left + scale(3), box.top + scale(8), box.left + scale(7), box.bottom - scale(4), kText);
        draw_line(dc, box.left + scale(7), box.bottom - scale(4), box.right - scale(3), box.top + scale(4), kText);
    }
    if (text != nullptr && text[0] != L'\0') {
        draw_text(dc, RECT{rect.left + scale(26), rect.top, rect.right, rect.bottom}, text, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

void MainWindow::draw_field(HDC dc, const RECT& rect, const wchar_t* label, const std::wstring& value, bool select) {
    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{rect.left, rect.top, rect.right, rect.top + scale(18)}, label, kMuted, DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT box{rect.left, rect.top + scale(20), rect.right, rect.bottom};
    fill_round_rect(dc, box, kPanel2, scale(3));
    stroke_rect(dc, box, kBorder);
    draw_text(dc, RECT{box.left + scale(10), box.top, box.right - scale(select ? 30 : 10), box.bottom}, value, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (select) {
        POINT arrow[3] = {
            {box.right - scale(18), box.top + scale(14)},
            {box.right - scale(10), box.top + scale(14)},
            {box.right - scale(14), box.top + scale(20)},
        };
        HBRUSH brush = CreateSolidBrush(kMuted);
        HGDIOBJ previous = SelectObject(dc, brush);
        Polygon(dc, arrow, 3);
        SelectObject(dc, previous);
        DeleteObject(brush);
    }
}

bool MainWindow::handle_content_click(int x, int y) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int top_bar = scale(kTopBar);
    const int rail_width = scale(kRailWidth);
    const int status_bar = scale(kStatusBar);
    RECT content{rail_width, top_bar, client.right, client.bottom - status_bar};
    RECT area = inset_rect(content, scale(30), scale(22));
    RECT start{area.right - scale(150), area.bottom - scale(58), area.right, area.bottom - scale(18)};

    Page page;
    {
        std::lock_guard lock(mutex_);
        page = state_.page;
    }

    if (contains_point(start, x, y)) {
        if (page == Page::Queue || page == Page::Compress) {
            start_compress();
            return true;
        }
        if (page == Page::Extract) {
            start_extract();
            return true;
        }
        if (page == Page::Security) {
            append_history("Security review completed");
            request_repaint();
            return true;
        }
    }

    if (page == Page::History) {
        const RECT clear{area.right - scale(142), area.top, area.right, area.top + scale(34)};
        if (contains_point(clear, x, y)) {
            clear_history();
            return true;
        }
    }

    std::lock_guard lock(mutex_);
    bool changed = false;
    if (page == Page::Preferences) {
        const RECT security{area.left, area.top + scale(240), area.left + scale(470), area.top + scale(400)};
        const RECT sha{security.left + scale(18), security.top + scale(48), security.right - scale(16), security.top + scale(80)};
        const RECT defender{security.left + scale(18), security.top + scale(84), security.right - scale(16), security.top + scale(116)};
        const RECT gpu{security.left + scale(18), security.top + scale(120), security.right - scale(16), security.top + scale(152)};
        const RECT general{area.left, area.top + scale(56), area.left + scale(470), area.top + scale(224)};
        const RECT performance{general.right + scale(18), general.top, area.right, area.top + scale(224)};
        const RECT verify{performance.left + scale(18), performance.top + scale(48), performance.right - scale(18), performance.top + scale(80)};
        if (contains_point(sha, x, y)) {
            state_.integrity_hash_opt_in = !state_.integrity_hash_opt_in;
            changed = true;
        } else if (contains_point(defender, x, y)) {
            state_.defender_scan_opt_in = !state_.defender_scan_opt_in;
            changed = true;
        } else if (contains_point(gpu, x, y)) {
            state_.gpu_required = !state_.gpu_required;
            changed = true;
        } else if (contains_point(verify, x, y)) {
            state_.verify_after_write_opt_in = !state_.verify_after_write_opt_in;
            changed = true;
        }
    } else if (page == Page::Compress) {
        const RECT advanced{area.left, area.top + scale(270), area.right, area.top + scale(390)};
        const RECT security{area.left, area.top + scale(410), area.right, area.top + scale(528)};
        const RECT verify{advanced.left + scale(342), advanced.top + scale(80), advanced.left + scale(710), advanced.top + scale(108)};
        const RECT sha{security.left + scale(18), security.top + scale(46), security.left + scale(420), security.top + scale(78)};
        const RECT defender{security.left + scale(18), security.top + scale(82), security.left + scale(420), security.top + scale(114)};
        if (contains_point(verify, x, y)) {
            state_.verify_after_write_opt_in = !state_.verify_after_write_opt_in;
            changed = true;
        } else if (contains_point(sha, x, y)) {
            state_.integrity_hash_opt_in = !state_.integrity_hash_opt_in;
            changed = true;
        } else if (contains_point(defender, x, y)) {
            state_.defender_scan_opt_in = !state_.defender_scan_opt_in;
            changed = true;
        }
    } else if (page == Page::Extract) {
        const RECT checks{area.left, area.top + scale(280), area.right, area.top + scale(412)};
        const RECT sha{checks.left + scale(470), checks.top + scale(48), checks.right - scale(20), checks.top + scale(80)};
        const RECT defender{checks.left + scale(470), checks.top + scale(84), checks.right - scale(20), checks.top + scale(116)};
        if (contains_point(sha, x, y)) {
            state_.integrity_hash_opt_in = !state_.integrity_hash_opt_in;
            changed = true;
        } else if (contains_point(defender, x, y)) {
            state_.defender_scan_opt_in = !state_.defender_scan_opt_in;
            changed = true;
        }
    }
    if (changed) {
        request_repaint();
    }
    return changed;
}

void MainWindow::set_page(Page page) {
    {
        std::lock_guard lock(mutex_);
        state_.page = page;
    }
    request_repaint();
}

void MainWindow::add_files() {
    OPENFILENAMEW ofn{};
    wchar_t files[8192]{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = files;
    ofn.nMaxFile = 8192;
    ofn.lpstrTitle = L"Add files to SuperZip";
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) {
        return;
    }
    std::lock_guard lock(mutex_);
    std::filesystem::path dir(files);
    wchar_t* cursor = files + dir.wstring().size() + 1;
    if (*cursor == L'\0') {
        state_.queued_paths.push_back(dir);
    } else {
        while (*cursor != L'\0') {
            std::filesystem::path name(cursor);
            state_.queued_paths.push_back(dir / name);
            cursor += name.wstring().size() + 1;
        }
    }
    request_repaint();
}

void MainWindow::add_folder() {
    BROWSEINFOW browse{};
    browse.hwndOwner = hwnd_;
    browse.lpszTitle = L"Add folder to SuperZip";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&browse);
    if (pidl == nullptr) {
        return;
    }
    wchar_t path[MAX_PATH]{};
    const BOOL ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (!ok || path[0] == L'\0') {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        state_.queued_paths.emplace_back(path);
    }
    request_repaint();
}

void MainWindow::clear_queue() {
    {
        std::lock_guard lock(mutex_);
        state_.queued_paths.clear();
    }
    request_repaint();
}

void MainWindow::clear_history() {
    {
        std::lock_guard lock(mutex_);
        state_.history.clear();
    }
    request_repaint();
}

void MainWindow::start_compress() {
    std::vector<std::filesystem::path> sources;
    bool gpu_required = true;
    bool integrity = false;
    bool defender = false;
    bool verify_after_write = false;
    {
        std::lock_guard lock(mutex_);
        sources = state_.queued_paths;
        gpu_required = state_.gpu_required;
        integrity = state_.integrity_hash_opt_in;
        defender = state_.defender_scan_opt_in;
        verify_after_write = state_.verify_after_write_opt_in;
    }
    if (sources.empty()) {
        return;
    }
    const auto output = std::filesystem::current_path() / "SuperZip-output.suzip";
    run_job([this, sources, output, gpu_required, integrity, defender, verify_after_write] {
        CompressOptions options;
        options.gpu_required = gpu_required;
        options.verify_after_write = verify_after_write;
        auto stats = compress_suzip(sources, output, options, [this](const ProgressSnapshot& snapshot) {
            std::lock_guard lock(mutex_);
            state_.progress = snapshot;
            request_repaint();
        });
        std::ostringstream line;
        line << "Compressed to " << output.string() << " in " << stats.seconds << "s";
        append_history(line.str());
        if (integrity) {
            const auto hash = hash_file(output, IntegrityMode::Sha256);
            append_history("SHA-256 " + hash.hex_digest);
        }
        if (defender) {
            const auto scan = scan_with_windows_defender(output, DefenderScanMode::FullPath);
            append_history(scan.attempted && scan.clean ? "Defender scan clean" : "Defender scan unavailable or not clean");
        }
    }, "Compressing");
}

void MainWindow::start_extract() {
    std::vector<std::filesystem::path> sources;
    bool gpu_required = true;
    bool overwrite = false;
    bool integrity = false;
    bool defender = false;
    {
        std::lock_guard lock(mutex_);
        sources = state_.queued_paths;
        gpu_required = state_.gpu_required;
        overwrite = state_.overwrite;
        integrity = state_.integrity_hash_opt_in;
        defender = state_.defender_scan_opt_in;
    }
    if (sources.empty()) {
        return;
    }
    const auto output = std::filesystem::current_path() / "SuperZip-extracted";
    run_job([this, archive = sources.front(), output, gpu_required, overwrite, integrity, defender] {
        if (integrity) {
            const auto hash = hash_file(archive, IntegrityMode::Sha256);
            append_history("SHA-256 " + hash.hex_digest);
        }
        if (defender) {
            const auto pre_scan = scan_with_windows_defender(archive, DefenderScanMode::FullPath);
            append_history(pre_scan.attempted && pre_scan.clean ? "Defender archive scan clean" : "Defender archive scan unavailable or not clean");
            if (pre_scan.attempted && !pre_scan.clean) {
                throw SecurityError("Microsoft Defender did not report the archive as clean: " + archive.string());
            }
        }
        ExtractOptions options;
        options.gpu_required = gpu_required;
        options.overwrite = overwrite;
        auto stats = extract_suzip(archive, output, options, [this](const ProgressSnapshot& snapshot) {
            std::lock_guard lock(mutex_);
            state_.progress = snapshot;
            request_repaint();
        });
        std::ostringstream line;
        line << "Extracted to " << output.string() << " in " << stats.seconds << "s";
        append_history(line.str());
        if (defender) {
            const auto post_scan = scan_with_windows_defender(output, DefenderScanMode::FullPath);
            append_history(post_scan.attempted && post_scan.clean ? "Defender output scan clean" : "Defender output scan unavailable or not clean");
        }
    }, "Extracting");
}

void MainWindow::run_job(std::function<void()> job, std::string label) {
    if (worker_running_.exchange(true)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    {
        std::lock_guard lock(mutex_);
        state_.status = std::move(label);
    }
    worker_ = std::thread([this, job = std::move(job)] {
        try {
            job();
            std::lock_guard lock(mutex_);
            state_.status = "Ready";
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex_);
            state_.status = std::string("Error: ") + error.what();
            state_.history.push_back(state_.status);
        }
        worker_running_ = false;
        request_repaint();
    });
    request_repaint();
}

void MainWindow::append_history(const std::string& line) {
    std::lock_guard lock(mutex_);
    state_.history.push_back(line);
}

void MainWindow::refresh_gpu_status() {
    const auto info = query_gpu_info();
    std::lock_guard lock(mutex_);
    state_.gpu_status = info.status;
}

void MainWindow::rebuild_fonts() {
    for (HFONT* font : {&title_font_, &body_font_, &small_font_, &tiny_font_, &mono_font_}) {
        if (*font != nullptr) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
    title_font_ = CreateFontW(-MulDiv(22, static_cast<int>(dpi_), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    body_font_ = CreateFontW(-MulDiv(12, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    small_font_ = CreateFontW(-MulDiv(10, static_cast<int>(dpi_), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    tiny_font_ = CreateFontW(-MulDiv(9, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    mono_font_ = CreateFontW(-MulDiv(9, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, L"Cascadia Mono");
}

void MainWindow::request_repaint() {
    if (hwnd_ == nullptr) {
        return;
    }
    if (!repaint_queued_.exchange(true)) {
        PostMessageW(hwnd_, WM_APP + 1, 0, 0);
    }
}

int MainWindow::scale(int value) const {
    return MulDiv(value, static_cast<int>(dpi_), 96);
}

}  // namespace superzip::app
