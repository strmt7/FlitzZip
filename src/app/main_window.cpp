#include "app/main_window.hpp"

#include "core/archive.hpp"
#include "core/defender_scan.hpp"
#include "core/integrity.hpp"
#include "core/result.hpp"
#include "gpu/gpu_codec.hpp"
#include "zip/zip_adapter.hpp"

#include <algorithm>
#include <commdlg.h>
#include <iomanip>
#include <shellapi.h>
#include <sstream>
#include <string>
#include <windowsx.h>

namespace superzip::app {
namespace {

constexpr COLORREF kBg = RGB(18, 20, 24);
constexpr COLORREF kPanel = RGB(31, 34, 40);
constexpr COLORREF kPanel2 = RGB(42, 46, 54);
constexpr COLORREF kText = RGB(238, 241, 245);
constexpr COLORREF kMuted = RGB(156, 164, 175);
constexpr COLORREF kAccent = RGB(218, 55, 64);
constexpr COLORREF kOk = RGB(61, 178, 108);
constexpr COLORREF kWarn = RGB(230, 168, 70);

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

// Purpose: Convert UTF-16 Win32 text to UTF-8.
// Inputs: `value` is UTF-16 text.
// Outputs: Returns UTF-8 text; returns an empty string for empty input.
std::string narrow(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), needed, nullptr, nullptr);
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

// Purpose: Map a page enum to a navigation label.
// Inputs: `page` is the active or target page.
// Outputs: Returns a UTF-16 label for rendering.
std::wstring page_name(Page page) {
    switch (page) {
    case Page::Queue: return L"Queue";
    case Page::Compress: return L"Compress";
    case Page::Extract: return L"Extract";
    case Page::Security: return L"Security Review";
    case Page::History: return L"History";
    case Page::Gpu: return L"AMD GPU";
    case Page::Preferences: return L"Preferences";
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

// Purpose: Draw UTF-16 text with the currently selected font.
// Inputs: `dc` is the target, `rect` is the layout box, `text` is display text, `color` is the text color, and `format` is the DrawTextW flags mask.
// Outputs: Writes text into `dc` without mutating the caller's rectangle.
void draw_text(HDC dc, const RECT& rect, const std::wstring& text, COLORREF color, UINT format) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT copy = rect;
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &copy, format);
}

// Purpose: Return whether a point is inside a Win32 rectangle.
// Inputs: `rect` is in physical client pixels and `x`/`y` are physical client coordinates.
// Outputs: Returns true when the point lies within the rectangle's half-open bounds.
bool contains_point(const RECT& rect, int x, int y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

}  // namespace

MainWindow::MainWindow() = default;

MainWindow::~MainWindow() {
    if (worker_.joinable()) {
        worker_.join();
    }
    if (title_font_ != nullptr) {
        DeleteObject(title_font_);
    }
    if (body_font_ != nullptr) {
        DeleteObject(body_font_);
    }
}

int MainWindow::run(HINSTANCE instance, int show_command) {
    refresh_gpu_status();
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
        1180,
        760,
        nullptr,
        nullptr,
        instance,
        this);
    if (hwnd_ == nullptr) {
        return 1;
    }

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
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const int sidebar_width = scale(188);
        const int content_left = sidebar_width;
        if (x < sidebar_width && y >= scale(112)) {
            const int item_height = scale(48);
            const int item = item_height > 0 ? (y - scale(112)) / item_height : -1;
            if (item >= 0 && item <= 6) {
                set_page(static_cast<Page>(item));
            }
        } else if (y > scale(90) && y < scale(132)) {
            if (x > content_left + scale(34) && x < content_left + scale(160)) {
                add_files();
            } else if (x > content_left + scale(178) && x < content_left + scale(318)) {
                start_compress();
            } else if (x > content_left + scale(333) && x < content_left + scale(473)) {
                start_extract();
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
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
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
    fill_rect(dc, rect, kBg);
    SelectObject(dc, body_font_);
    RECT sidebar{0, 0, scale(188), rect.bottom};
    RECT content{scale(188), 0, rect.right, rect.bottom};
    draw_sidebar(dc, sidebar);
    draw_content(dc, content);
}

void MainWindow::draw_sidebar(HDC dc, const RECT& rect) {
    fill_rect(dc, rect, RGB(13, 15, 19));
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{scale(22), scale(24), rect.right - scale(12), scale(62)}, L"SuperZip", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, body_font_);
    draw_text(dc, RECT{scale(22), scale(58), rect.right - scale(12), scale(86)}, L"AMD GPU archive engine", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    UiState state;
    {
        std::lock_guard lock(mutex_);
        state = state_;
    }
    for (int i = 0; i < 7; ++i) {
        RECT item{scale(16), scale(112 + i * 48), rect.right - scale(16), scale(150 + i * 48)};
        draw_button(dc, item, page_name(static_cast<Page>(i)).c_str(), state.page == static_cast<Page>(i));
    }
}

void MainWindow::draw_button(HDC dc, const RECT& rect, const wchar_t* text, bool active) {
    fill_rect(dc, rect, active ? kAccent : kPanel);
    draw_text(dc, RECT{rect.left + scale(14), rect.top, rect.right - scale(8), rect.bottom}, text, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void MainWindow::draw_toggle(HDC dc, const RECT& rect, const wchar_t* text, bool enabled) {
    fill_rect(dc, rect, kPanel2);
    RECT box{rect.left + scale(12), rect.top + scale(9), rect.left + scale(34), rect.top + scale(31)};
    fill_rect(dc, box, enabled ? kOk : RGB(82, 88, 100));
    if (enabled) {
        draw_text(dc, box, L"ON", RGB(10, 18, 14), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    draw_text(dc, RECT{rect.left + scale(48), rect.top, rect.right - scale(12), rect.bottom}, text, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void MainWindow::draw_content(HDC dc, const RECT& rect) {
    UiState state;
    {
        std::lock_guard lock(mutex_);
        state = state_;
    }
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{rect.left + scale(34), scale(26), rect.right - scale(24), scale(62)}, page_name(state.page), kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, body_font_);

    draw_button(dc, RECT{rect.left + scale(34), scale(90), rect.left + scale(160), scale(132)}, L"Add files", false);
    draw_button(dc, RECT{rect.left + scale(178), scale(90), rect.left + scale(318), scale(132)}, L"Compress", true);
    draw_button(dc, RECT{rect.left + scale(333), scale(90), rect.left + scale(473), scale(132)}, L"Extract", false);

    RECT status_rect{rect.left + scale(34), scale(150), rect.right - scale(34), scale(205)};
    fill_rect(dc, status_rect, kPanel);
    std::ostringstream status;
    status << state.status << " | " << state.gpu_status;
    draw_text(dc, RECT{status_rect.left + scale(16), status_rect.top, status_rect.right - scale(16), status_rect.bottom}, widen(status.str()), kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT body{rect.left + scale(34), scale(226), rect.right - scale(34), rect.bottom - scale(34)};
    fill_rect(dc, body, kPanel);
    RECT inner{body.left + scale(22), body.top + scale(18), body.right - scale(22), body.bottom - scale(18)};

    if (state.page == Page::Queue || state.page == Page::Compress || state.page == Page::Extract) {
        draw_text(dc, inner, L"Queued paths", kText, DT_LEFT | DT_TOP);
        int y = inner.top + scale(34);
        if (state.queued_paths.empty()) {
            draw_text(dc, RECT{inner.left, y, inner.right, y + scale(48)}, L"Drop files or folders here. Work starts on a background thread and the UI stays responsive.", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
        for (const auto& path : state.queued_paths) {
            draw_text(dc, RECT{inner.left, y, inner.right, y + scale(28)}, path.wstring(), kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += scale(30);
            if (y > inner.bottom - scale(80)) {
                break;
            }
        }
        const auto pct = state.progress.total_bytes == 0
            ? 0.0
            : std::clamp(static_cast<double>(state.progress.processed_bytes) / static_cast<double>(state.progress.total_bytes), 0.0, 1.0);
        RECT progress_bg{inner.left, inner.bottom - scale(52), inner.right, inner.bottom - scale(28)};
        fill_rect(dc, progress_bg, kPanel2);
        RECT progress_fg = progress_bg;
        progress_fg.right = progress_fg.left + static_cast<LONG>((progress_bg.right - progress_bg.left) * pct);
        fill_rect(dc, progress_fg, kOk);
        const auto speed = human_bytes(state.progress.throughput_bytes_per_second) + "/s";
        draw_text(dc, RECT{inner.left, inner.bottom - scale(26), inner.right, inner.bottom}, widen(speed), kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else if (state.page == Page::Security) {
        draw_text(dc, inner, L"Extraction is blocked unless every archive path stays inside the selected destination. SuperZip rejects absolute paths, drive-rooted paths, traversal, reserved Windows names, malformed block metadata, CRC mismatches, and overwrite attempts unless enabled.", kText, DT_LEFT | DT_TOP | DT_WORDBREAK);
        const int row_top = inner.top + scale(110);
        draw_toggle(dc, RECT{inner.left, row_top, inner.right, row_top + scale(40)}, L"Opt in to Microsoft Defender scan for archives and extracted output", state.defender_scan_opt_in);
        draw_toggle(dc, RECT{inner.left, row_top + scale(48), inner.right, row_top + scale(88)}, L"Opt in to SHA-256 archive integrity hash", state.integrity_hash_opt_in);
        draw_toggle(dc, RECT{inner.left, row_top + scale(96), inner.right, row_top + scale(136)}, L"Allow overwrite during extraction", state.overwrite);
    } else if (state.page == Page::History) {
        int y = inner.top;
        for (const auto& line : state.history) {
            draw_text(dc, RECT{inner.left, y, inner.right, y + scale(28)}, widen(line), kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += scale(30);
        }
    } else if (state.page == Page::Gpu) {
        draw_text(dc, inner, widen(state.gpu_status), state.gpu_status.find("ready") != std::string::npos ? kOk : kWarn, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else if (state.page == Page::Preferences) {
        draw_text(dc, inner, L"Operation defaults", kText, DT_LEFT | DT_TOP);
        const int row_top = inner.top + scale(42);
        draw_toggle(dc, RECT{inner.left, row_top, inner.right, row_top + scale(40)}, L"Require AMD GPU acceleration for native .szip jobs", state.gpu_required);
        draw_toggle(dc, RECT{inner.left, row_top + scale(48), inner.right, row_top + scale(88)}, L"Enable SHA-256 archive integrity hash after jobs", state.integrity_hash_opt_in);
        draw_toggle(dc, RECT{inner.left, row_top + scale(96), inner.right, row_top + scale(136)}, L"Enable Microsoft Defender scan after jobs", state.defender_scan_opt_in);
        draw_toggle(dc, RECT{inner.left, row_top + scale(144), inner.right, row_top + scale(184)}, L"Allow overwrite during extraction", state.overwrite);
    }
}

bool MainWindow::handle_content_click(int x, int y) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    RECT body{scale(188) + scale(34), scale(226), client.right - scale(34), client.bottom - scale(34)};
    RECT inner{body.left + scale(22), body.top + scale(18), body.right - scale(22), body.bottom - scale(18)};

    std::lock_guard lock(mutex_);
    bool changed = false;
    if (state_.page == Page::Preferences) {
        const int row_top = inner.top + scale(42);
        const RECT gpu{inner.left, row_top, inner.right, row_top + scale(40)};
        const RECT sha{inner.left, row_top + scale(48), inner.right, row_top + scale(88)};
        const RECT defender{inner.left, row_top + scale(96), inner.right, row_top + scale(136)};
        const RECT overwrite{inner.left, row_top + scale(144), inner.right, row_top + scale(184)};
        if (contains_point(gpu, x, y)) {
            state_.gpu_required = !state_.gpu_required;
            changed = true;
        } else if (contains_point(sha, x, y)) {
            state_.integrity_hash_opt_in = !state_.integrity_hash_opt_in;
            changed = true;
        } else if (contains_point(defender, x, y)) {
            state_.defender_scan_opt_in = !state_.defender_scan_opt_in;
            changed = true;
        } else if (contains_point(overwrite, x, y)) {
            state_.overwrite = !state_.overwrite;
            changed = true;
        }
    } else if (state_.page == Page::Security) {
        const int row_top = inner.top + scale(110);
        const RECT defender{inner.left, row_top, inner.right, row_top + scale(40)};
        const RECT sha{inner.left, row_top + scale(48), inner.right, row_top + scale(88)};
        const RECT overwrite{inner.left, row_top + scale(96), inner.right, row_top + scale(136)};
        if (contains_point(defender, x, y)) {
            state_.defender_scan_opt_in = !state_.defender_scan_opt_in;
            changed = true;
        } else if (contains_point(sha, x, y)) {
            state_.integrity_hash_opt_in = !state_.integrity_hash_opt_in;
            changed = true;
        } else if (contains_point(overwrite, x, y)) {
            state_.overwrite = !state_.overwrite;
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

void MainWindow::start_compress() {
    std::vector<std::filesystem::path> sources;
    bool gpu_required = true;
    bool integrity = false;
    bool defender = false;
    {
        std::lock_guard lock(mutex_);
        sources = state_.queued_paths;
        gpu_required = state_.gpu_required;
        integrity = state_.integrity_hash_opt_in;
        defender = state_.defender_scan_opt_in;
    }
    if (sources.empty()) {
        return;
    }
    const auto output = std::filesystem::current_path() / "SuperZip-output.szip";
    run_job([this, sources, output, gpu_required, integrity, defender] {
        CompressOptions options;
        options.gpu_required = gpu_required;
        auto stats = compress_szip(sources, output, options, [this](const ProgressSnapshot& snapshot) {
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
        auto stats = extract_szip(archive, output, options, [this](const ProgressSnapshot& snapshot) {
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
    if (title_font_ != nullptr) {
        DeleteObject(title_font_);
        title_font_ = nullptr;
    }
    if (body_font_ != nullptr) {
        DeleteObject(body_font_);
        body_font_ = nullptr;
    }
    title_font_ = CreateFontW(-MulDiv(20, static_cast<int>(dpi_), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    body_font_ = CreateFontW(-MulDiv(11, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, L"Segoe UI");
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
