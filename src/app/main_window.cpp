#include "app/main_window.hpp"

#include "app/resource.h"
#include "core/archive.hpp"
#include "core/defender_scan.hpp"
#include "core/integrity.hpp"
#include "core/result.hpp"
#include "gpu/gpu_codec.hpp"
#include "zip/zip_adapter.hpp"

#include <algorithm>
#include <array>
#include <commdlg.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <iomanip>
#include <psapi.h>
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
constexpr int kDesignClientWidth = 1200;
constexpr int kDesignClientHeight = 760;
constexpr int kPageInsetX = 30;
constexpr int kPageInsetY = 22;
constexpr int kPageHeaderHeight = 34;
constexpr UINT_PTR kAnimationTimer = 7;
constexpr UINT_PTR kSmokeAutoCloseTimer = 9;
constexpr UINT_PTR kSmokeClosePollTimer = 10;
constexpr int kPageTransitionMs = 120;
constexpr int kToggleTransitionMs = 105;

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
constexpr UINT_PTR kPerformanceTimer = 8;
constexpr UINT kPerformanceSampleMs = 250;
constexpr UINT kGpuMemorySampleMs = 1000;

// Purpose: Return the fixed release window style.
// Inputs: None.
// Outputs: Returns a non-resizable Win32 overlapped-window style.
DWORD window_style() {
    return WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
}

// Purpose: Read the optional GUI-smoke auto-close timeout.
// Inputs: Uses `SUPERZIP_GUI_SMOKE_AUTO_CLOSE_MS` from the process environment.
// Outputs: Returns a bounded timer interval in milliseconds, or zero when disabled.
UINT smoke_auto_close_ms() {
    wchar_t buffer[32]{};
    constexpr DWORD capacity = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    const DWORD length = GetEnvironmentVariableW(L"SUPERZIP_GUI_SMOKE_AUTO_CLOSE_MS", buffer, capacity);
    if (length == 0 || length >= capacity) {
        return 0;
    }
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(buffer, &end, 10);
    if (end == buffer || parsed == 0) {
        return 0;
    }
    return static_cast<UINT>(std::clamp<unsigned long>(parsed, 5000UL, 300000UL));
}

// Purpose: Read the optional GUI-smoke close-marker file path.
// Inputs: Uses `SUPERZIP_GUI_SMOKE_CLOSE_FILE` from the process environment.
// Outputs: Returns the marker path, or an empty path when smoke close polling is disabled.
std::filesystem::path smoke_close_marker_path() {
    wchar_t buffer[32768]{};
    constexpr DWORD capacity = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    const DWORD length = GetEnvironmentVariableW(L"SUPERZIP_GUI_SMOKE_CLOSE_FILE", buffer, capacity);
    if (length == 0 || length >= capacity) {
        return {};
    }
    return std::filesystem::path(buffer);
}

// Purpose: Check whether the GUI smoke harness requested in-process shutdown.
// Inputs: Uses the smoke close-marker path from the process environment.
// Outputs: Returns true only when the configured marker file exists.
bool smoke_close_requested() {
    const auto marker = smoke_close_marker_path();
    if (marker.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(marker, ec);
}

// Purpose: Return the default working directory without throwing into paint code.
// Inputs: None.
// Outputs: Returns the process current directory, or `.` if Windows reports an error.
std::filesystem::path safe_current_path() {
    std::error_code ec;
    auto path = std::filesystem::current_path(ec);
    if (ec) {
        return L".";
    }
    return path;
}

// Purpose: Resolve the selected destination directory or the process directory.
// Inputs: `state` is the copied UI state.
// Outputs: Returns a usable destination folder path for display and jobs.
std::filesystem::path destination_directory_or_default(const UiState& state) {
    if (!state.destination_directory.empty()) {
        return state.destination_directory;
    }
    return safe_current_path();
}

// Purpose: Resolve the visible compression output path.
// Inputs: `state` is the copied UI state.
// Outputs: Returns the destination archive path using the native `.suzip` extension.
std::filesystem::path compression_output_path_for(const UiState& state) {
    return destination_directory_or_default(state) / L"SuperZip-output.suzip";
}

// Purpose: Resolve the visible extraction output path.
// Inputs: `state` is the copied UI state.
// Outputs: Returns the selected destination, or a default extraction folder when none is selected.
std::filesystem::path extraction_output_path_for(const UiState& state) {
    if (!state.destination_directory.empty()) {
        return state.destination_directory;
    }
    return safe_current_path() / L"SuperZip-extracted";
}

// Purpose: Return the user-facing compression profile label.
// Inputs: `index` is the mutable profile index in UI state.
// Outputs: Returns a stable label for the profile field.
std::wstring compression_profile_text(int index) {
    constexpr std::array<std::wstring_view, 3> labels{
        L"Balanced (AMD GPU)",
        L"Maximum speed",
        L"Maximum compression",
    };
    const auto normalized = static_cast<std::size_t>((index % static_cast<int>(labels.size()) + static_cast<int>(labels.size())) % static_cast<int>(labels.size()));
    return std::wstring(labels[normalized]);
}

// Purpose: Return the compression block-size labels shown in the Compress page.
// Inputs: `index` is the mutable block-size selection in UI state.
// Outputs: Returns a meaningful tuning label that maps to a supported archive block size.
std::wstring compression_block_size_text(int index) {
    constexpr std::array<std::wstring_view, 4> labels{
        L"256 KiB blocks",
        L"1 MiB blocks",
        L"4 MiB blocks",
        L"16 MiB blocks",
    };
    const auto normalized = static_cast<std::size_t>((index % static_cast<int>(labels.size()) + static_cast<int>(labels.size())) % static_cast<int>(labels.size()));
    return std::wstring(labels[normalized]);
}

// Purpose: Map a compression block-size selection to bytes accepted by the archive core.
// Inputs: `index` is the mutable block-size selection in UI state.
// Outputs: Returns a block size between `kMinArchiveBlockBytes` and `kMaxArchiveBlockBytes`.
std::uint32_t compression_block_size_bytes(int index) {
    constexpr std::array<std::uint32_t, 4> sizes{
        256U * 1024U,
        superzip::kDefaultArchiveBlockBytes,
        4U * 1024U * 1024U,
        superzip::kMaxArchiveBlockBytes,
    };
    const auto normalized = static_cast<std::size_t>((index % static_cast<int>(sizes.size()) + static_cast<int>(sizes.size())) % static_cast<int>(sizes.size()));
    return sizes[normalized];
}

// Purpose: Return a label from a circular option list.
// Inputs: `index` is the current selection and `labels` contains display choices.
// Outputs: Returns the normalized display label.
template <std::size_t Count>
std::wstring option_text(int index, const std::array<std::wstring_view, Count>& labels) {
    static_assert(Count > 0);
    const auto normalized = static_cast<std::size_t>((index % static_cast<int>(Count) + static_cast<int>(Count)) % static_cast<int>(Count));
    return std::wstring(labels[normalized]);
}

// Purpose: Return the visible memory-policy label.
// Inputs: `index` is the mutable preferences selection.
// Outputs: Returns a session preference label.
std::wstring memory_policy_text(int index) {
    constexpr std::array<std::wstring_view, 3> labels{
        L"Bounded chunk windows",
        L"Throughput priority",
        L"Conservative RAM cap",
    };
    return option_text(index, labels);
}

// Purpose: Return the visible log-level label.
// Inputs: `index` is the mutable preferences selection.
// Outputs: Returns a session preference label.
std::wstring log_level_text(int index) {
    constexpr std::array<std::wstring_view, 3> labels{
        L"Information",
        L"Warnings",
        L"Verbose diagnostics",
    };
    return option_text(index, labels);
}

// Purpose: Return the visible log-retention label.
// Inputs: `index` is the mutable preferences selection.
// Outputs: Returns a session preference label.
std::wstring log_retention_text(int index) {
    constexpr std::array<std::wstring_view, 3> labels{
        L"Session only",
        L"7 days",
        L"30 days",
    };
    return option_text(index, labels);
}

// Purpose: Return the visible history operation filter label.
// Inputs: `index` is the mutable history filter selection.
// Outputs: Returns a session filter label.
std::wstring history_operation_filter_text(int index) {
    constexpr std::array<std::wstring_view, 4> labels{
        L"All operations",
        L"Compress",
        L"Extract",
        L"Security",
    };
    return option_text(index, labels);
}

// Purpose: Return the visible history status filter label.
// Inputs: `index` is the mutable history filter selection.
// Outputs: Returns a session filter label.
std::wstring history_status_filter_text(int index) {
    constexpr std::array<std::wstring_view, 3> labels{
        L"All statuses",
        L"Success",
        L"Failed",
    };
    return option_text(index, labels);
}

// Purpose: Return the display options for an expanded dropdown.
// Inputs: `id` identifies the dropdown control.
// Outputs: Returns ordered labels matching the selectable rows.
std::vector<std::wstring> dropdown_options(DropdownId id) {
    switch (id) {
    case DropdownId::CompressProfile:
        return {compression_profile_text(0), compression_profile_text(1), compression_profile_text(2)};
    case DropdownId::CompressMethod:
        return {L"AMD HIP required", L"AMD HIP preferred"};
    case DropdownId::CompressBlockSize:
        return {
            compression_block_size_text(0),
            compression_block_size_text(1),
            compression_block_size_text(2),
            compression_block_size_text(3),
        };
    case DropdownId::ExtractOverwrite:
        return {L"Ask before overwrite", L"Overwrite enabled"};
    case DropdownId::HistoryOperation:
        return {history_operation_filter_text(0), history_operation_filter_text(1), history_operation_filter_text(2), history_operation_filter_text(3)};
    case DropdownId::HistoryStatus:
        return {history_status_filter_text(0), history_status_filter_text(1), history_status_filter_text(2)};
    case DropdownId::PreferencesMemoryPolicy:
        return {memory_policy_text(0), memory_policy_text(1), memory_policy_text(2)};
    case DropdownId::PreferencesLogLevel:
        return {log_level_text(0), log_level_text(1), log_level_text(2)};
    case DropdownId::PreferencesLogRetention:
        return {log_retention_text(0), log_retention_text(1), log_retention_text(2)};
    case DropdownId::None:
        return {};
    }
    return {};
}

// Purpose: Return the currently selected option row for a dropdown.
// Inputs: `state` is the copied UI state and `id` identifies the dropdown.
// Outputs: Returns a zero-based row index, clamped by renderers before use.
int dropdown_selected_index(const UiState& state, DropdownId id) {
    switch (id) {
    case DropdownId::CompressProfile:
        return state.compression_profile_index;
    case DropdownId::CompressMethod:
        return state.gpu_required ? 0 : 1;
    case DropdownId::CompressBlockSize:
        return state.compression_block_size_index;
    case DropdownId::ExtractOverwrite:
        return state.overwrite ? 1 : 0;
    case DropdownId::HistoryOperation:
        return state.history_operation_filter_index;
    case DropdownId::HistoryStatus:
        return state.history_status_filter_index;
    case DropdownId::PreferencesMemoryPolicy:
        return state.memory_policy_index;
    case DropdownId::PreferencesLogLevel:
        return state.log_level_index;
    case DropdownId::PreferencesLogRetention:
        return state.log_retention_index;
    case DropdownId::None:
        return 0;
    }
    return 0;
}

// Purpose: Blend two Win32 colors linearly.
// Inputs: `from` and `to` are colors and `t` is clamped interpolation progress.
// Outputs: Returns the blended color.
COLORREF blend_color(COLORREF from, COLORREF to, double t) {
    const double clamped = std::clamp(t, 0.0, 1.0);
    const auto blend = [clamped](int a, int b) {
        return static_cast<int>(std::round(static_cast<double>(a) + (static_cast<double>(b - a) * clamped)));
    };
    return RGB(blend(GetRValue(from), GetRValue(to)), blend(GetGValue(from), GetGValue(to)), blend(GetBValue(from), GetBValue(to)));
}

// Purpose: Ease a short UI animation without changing duration.
// Inputs: `t` is normalized progress.
// Outputs: Returns a smooth normalized progress value.
double ease_out(double t) {
    const double clamped = std::clamp(t, 0.0, 1.0);
    return 1.0 - ((1.0 - clamped) * (1.0 - clamped));
}

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

// Purpose: Convert a Windows FILETIME into 100 ns ticks.
// Inputs: `value` is a FILETIME returned by a Win32 process-time API.
// Outputs: Returns the unsigned 64-bit tick value used for interval math.
std::uint64_t filetime_ticks(const FILETIME& value) {
    ULARGE_INTEGER ticks{};
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    return ticks.QuadPart;
}

// Purpose: Format a percentage for compact monitor cards.
// Inputs: `value` is a percent value already clamped by the caller when needed.
// Outputs: Returns a one-decimal UTF-16 percentage string.
std::wstring percentage_text(double value) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(1) << value << L"%";
    return out.str();
}

// Purpose: Format a throughput value for compact monitor cards.
// Inputs: `bytes_per_second` is a nonnegative byte rate.
// Outputs: Returns a UTF-16 string with `/s` suffix.
std::wstring rate_text(double bytes_per_second) {
    return widen(human_bytes(bytes_per_second) + "/s");
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
    case Page::Gpu: return L"GPU";
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
// Outputs: Draws three anti-aliased compact diamond layers.
void draw_logo(HDC dc, const RECT& rect, COLORREF color) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    const float width = static_cast<float>(rect.right - rect.left);
    const float height = static_cast<float>(rect.bottom - rect.top);
    const float size = std::max(1.0f, std::min(width, height));
    const float origin_x = static_cast<float>(rect.left) + (width - size) / 2.0f;
    const float origin_y = static_cast<float>(rect.top) + (height - size) / 2.0f;
    auto px = [origin_x, size](float value) { return origin_x + (value * size / 30.0f); };
    auto py = [origin_y, size](float value) { return origin_y + (value * size / 30.0f); };

    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)), std::max(1.6f, size / 12.0f));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush fill(Gdiplus::Color(32, GetRValue(color), GetGValue(color), GetBValue(color)));
    for (int i = 0; i < 3; ++i) {
        const float y = 5.0f + static_cast<float>(i) * 6.9f;
        Gdiplus::PointF points[] = {
            {px(15.0f), py(y)},
            {px(26.0f), py(y + 4.3f)},
            {px(15.0f), py(y + 8.6f)},
            {px(4.0f), py(y + 4.3f)},
        };
        Gdiplus::GraphicsPath layer;
        layer.AddPolygon(points, 4);
        layer.CloseFigure();
        graphics.FillPath(&fill, &layer);
        graphics.DrawPath(&pen, &layer);
    }
}

// Purpose: Convert a Win32 COLORREF into a GDI+ color.
// Inputs: `color` is a Win32 RGB value and `alpha` is the target opacity.
// Outputs: Returns an ARGB GDI+ color with channel order corrected.
Gdiplus::Color gp_color(COLORREF color, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

// Purpose: Configure a GDI+ pen for polished small-icon strokes.
// Inputs: `pen` is the pen being used for the vector icon.
// Outputs: Applies rounded caps and joins so high-DPI navigation icons stay smooth.
void configure_icon_pen(Gdiplus::Pen& pen) {
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
}

// Purpose: Append a rounded rectangle to a GDI+ path.
// Inputs: `path` is the destination path and the remaining values are design-space bounds.
// Outputs: Adds a closed rounded-rectangle figure.
void add_rounded_rect_path(Gdiplus::GraphicsPath& path, float x, float y, float width, float height, float radius) {
    const float r = std::min(radius, std::min(width, height) / 2.0f);
    path.AddArc(x, y, r * 2.0f, r * 2.0f, 180.0f, 90.0f);
    path.AddArc(x + width - r * 2.0f, y, r * 2.0f, r * 2.0f, 270.0f, 90.0f);
    path.AddArc(x + width - r * 2.0f, y + height - r * 2.0f, r * 2.0f, r * 2.0f, 0.0f, 90.0f);
    path.AddArc(x, y + height - r * 2.0f, r * 2.0f, r * 2.0f, 90.0f, 90.0f);
    path.CloseFigure();
}

// Purpose: Draw a compact anti-aliased navigation icon from native vector primitives.
// Inputs: `dc` is the target, `page` selects the icon, `rect` is the icon box, and `color` is the stroke color.
// Outputs: Writes a crisp GDI+ icon without bitmap scaling.
void draw_nav_icon(HDC dc, Page page, const RECT& rect, COLORREF color) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    // Use one normalized coordinate system so every rail glyph has the same optical size.
    const float rect_width = static_cast<float>(rect.right - rect.left);
    const float rect_height = static_cast<float>(rect.bottom - rect.top);
    const float size = std::max(1.0f, std::min(rect_width, rect_height));
    const float origin_x = static_cast<float>(rect.left) + (rect_width - size) / 2.0f;
    const float origin_y = static_cast<float>(rect.top) + (rect_height - size) / 2.0f;
    auto px = [origin_x, size](float value) { return origin_x + (value * size / 30.0f); };
    auto py = [origin_y, size](float value) { return origin_y + (value * size / 30.0f); };
    auto rectf = [&](float left, float top, float right, float bottom) {
        return Gdiplus::RectF(px(left), py(top), px(right) - px(left), py(bottom) - py(top));
    };

    // Keep dense icon details legible by separating normal, fine, and emphasis strokes.
    Gdiplus::Pen pen(gp_color(color), std::max(2.0f, size / 13.5f));
    configure_icon_pen(pen);
    Gdiplus::Pen fine_pen(gp_color(color, 220), std::max(1.25f, size / 22.0f));
    configure_icon_pen(fine_pen);
    Gdiplus::Pen strong_pen(gp_color(color), std::max(2.3f, size / 11.5f));
    configure_icon_pen(strong_pen);
    Gdiplus::SolidBrush soft_fill(gp_color(color, 28));
    Gdiplus::SolidBrush firm_fill(gp_color(color, 245));

    // Each glyph uses a small number of features so it remains recognizable at high DPI.
    switch (page) {
    case Page::Queue:
        for (int i = 0; i < 5; ++i) {
            const float row = 5.7f + static_cast<float>(i) * 4.8f;
            graphics.FillEllipse(&firm_fill, rectf(6.0f, row - 1.0f, 8.0f, row + 1.0f));
            graphics.DrawLine(&pen, px(11.0f), py(row), px(24.0f), py(row));
        }
        break;
    case Page::Compress:
        {
            Gdiplus::GraphicsPath tray;
            add_rounded_rect_path(tray, px(6.2f), py(16.2f), px(23.8f) - px(6.2f), py(24.2f) - py(16.2f), size / 12.0f);
            graphics.FillPath(&soft_fill, &tray);
            graphics.DrawPath(&pen, &tray);
            graphics.DrawLine(&strong_pen, px(15.0f), py(5.2f), px(15.0f), py(15.0f));
            Gdiplus::PointF arrow[] = {{px(10.4f), py(11.2f)}, {px(15.0f), py(15.8f)}, {px(19.6f), py(11.2f)}};
            graphics.DrawLines(&strong_pen, arrow, 3);
            graphics.DrawLine(&fine_pen, px(10.0f), py(20.0f), px(20.0f), py(20.0f));
        }
        break;
    case Page::Extract:
        {
            Gdiplus::GraphicsPath tray;
            add_rounded_rect_path(tray, px(6.2f), py(16.2f), px(23.8f) - px(6.2f), py(24.2f) - py(16.2f), size / 12.0f);
            graphics.FillPath(&soft_fill, &tray);
            graphics.DrawPath(&pen, &tray);
            graphics.DrawLine(&strong_pen, px(15.0f), py(15.8f), px(15.0f), py(5.2f));
            Gdiplus::PointF arrow[] = {{px(10.4f), py(9.8f)}, {px(15.0f), py(5.2f)}, {px(19.6f), py(9.8f)}};
            graphics.DrawLines(&strong_pen, arrow, 3);
            graphics.DrawLine(&fine_pen, px(10.0f), py(20.0f), px(20.0f), py(20.0f));
        }
        break;
    case Page::Security:
        {
            Gdiplus::PointF shield[] = {
                {px(15.0f), py(4.3f)},
                {px(23.3f), py(7.3f)},
                {px(22.2f), py(17.3f)},
                {px(15.0f), py(25.2f)},
                {px(7.8f), py(17.3f)},
                {px(6.7f), py(7.3f)},
            };
            Gdiplus::GraphicsPath path;
            path.AddPolygon(shield, 6);
            path.CloseFigure();
            graphics.FillPath(&soft_fill, &path);
            graphics.DrawPath(&pen, &path);
            Gdiplus::PointF check[] = {{px(10.7f), py(15.2f)}, {px(14.0f), py(18.5f)}, {px(20.2f), py(11.2f)}};
            graphics.DrawLines(&strong_pen, check, 3);
        }
        break;
    case Page::History:
        {
            graphics.DrawEllipse(&pen, rectf(5.0f, 5.0f, 25.0f, 25.0f));
            graphics.DrawLine(&fine_pen, px(15.0f), py(8.0f), px(15.0f), py(10.6f));
            graphics.DrawLine(&fine_pen, px(15.0f), py(19.4f), px(15.0f), py(22.0f));
            graphics.DrawLine(&fine_pen, px(8.0f), py(15.0f), px(10.6f), py(15.0f));
            graphics.DrawLine(&fine_pen, px(19.4f), py(15.0f), px(22.0f), py(15.0f));
            graphics.DrawLine(&pen, px(15.0f), py(15.0f), px(15.0f), py(10.0f));
            graphics.DrawLine(&pen, px(15.0f), py(15.0f), px(19.2f), py(17.8f));
            graphics.FillEllipse(&firm_fill, rectf(13.7f, 13.7f, 16.3f, 16.3f));
        }
        break;
    case Page::Gpu:
        {
            // The GPU glyph is intentionally a horizontal add-in card, not a square chip.
            Gdiplus::GraphicsPath card;
            add_rounded_rect_path(card, px(7.0f), py(8.3f), px(25.0f) - px(7.0f), py(21.4f) - py(8.3f), size / 12.0f);
            graphics.FillPath(&soft_fill, &card);
            graphics.DrawPath(&pen, &card);
            graphics.DrawLine(&pen, px(4.3f), py(10.5f), px(7.0f), py(10.5f));
            graphics.DrawLine(&pen, px(4.3f), py(10.5f), px(4.3f), py(19.2f));
            graphics.DrawLine(&pen, px(4.3f), py(19.2f), px(7.0f), py(19.2f));
            graphics.DrawEllipse(&pen, rectf(11.0f, 10.6f, 19.8f, 19.4f));
            graphics.FillEllipse(&firm_fill, rectf(14.0f, 13.6f, 16.8f, 16.4f));
            graphics.DrawLine(&fine_pen, px(15.4f), py(10.8f), px(15.4f), py(19.2f));
            graphics.DrawLine(&fine_pen, px(11.2f), py(15.0f), px(19.6f), py(15.0f));
            graphics.DrawLine(&fine_pen, px(21.8f), py(12.0f), px(24.1f), py(12.0f));
            graphics.DrawLine(&fine_pen, px(21.8f), py(16.0f), px(24.1f), py(16.0f));
            for (int i = 0; i < 4; ++i) {
                const float finger_x = 10.0f + static_cast<float>(i) * 3.0f;
                graphics.DrawLine(&fine_pen, px(finger_x), py(21.4f), px(finger_x), py(24.3f));
            }
        }
        break;
    case Page::Preferences:
        {
            std::array<Gdiplus::PointF, 16> teeth{};
            for (int i = 0; i < static_cast<int>(teeth.size()); ++i) {
                const double angle = (-3.14159265358979323846 / 2.0) + (3.14159265358979323846 * 2.0 * static_cast<double>(i) / static_cast<double>(teeth.size()));
                const float radius = (i % 2 == 0) ? 10.8f : 8.4f;
                teeth[static_cast<std::size_t>(i)] = {
                    px(15.0f + std::cos(angle) * radius),
                    py(15.0f + std::sin(angle) * radius),
                };
            }
            Gdiplus::GraphicsPath gear;
            gear.AddPolygon(teeth.data(), static_cast<INT>(teeth.size()));
            gear.CloseFigure();
            graphics.FillPath(&soft_fill, &gear);
            graphics.DrawPath(&pen, &gear);
            graphics.DrawEllipse(&pen, rectf(10.6f, 10.6f, 19.4f, 19.4f));
            graphics.FillEllipse(&soft_fill, rectf(13.0f, 13.0f, 17.0f, 17.0f));
        }
        break;
    case Page::About:
        graphics.DrawEllipse(&pen, rectf(6.0f, 6.0f, 24.0f, 24.0f));
        graphics.DrawLine(&strong_pen, px(15.0f), py(13.8f), px(15.0f), py(20.2f));
        graphics.FillEllipse(&firm_fill, rectf(13.7f, 9.4f, 16.3f, 12.0f));
        break;
    }
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

int MainWindow::run(HINSTANCE instance, int show_command) {
    Gdiplus::GdiplusStartupInput gdiplus_startup_input{};
    if (Gdiplus::GdiplusStartup(&gdiplus_token_, &gdiplus_startup_input, nullptr) != Gdiplus::Ok) {
        gdiplus_token_ = 0;
        return 1;
    }

    refresh_gpu_status();
    const UINT initial_dpi = GetDpiForSystem();
    const DWORD style = window_style();
    RECT initial_window{0, 0, MulDiv(kDesignClientWidth, static_cast<int>(initial_dpi), 96), MulDiv(kDesignClientHeight, static_cast<int>(initial_dpi), 96)};
    AdjustWindowRectExForDpi(&initial_window, style, FALSE, 0, initial_dpi);
    const int initial_width = initial_window.right - initial_window.left;
    const int initial_height = initial_window.bottom - initial_window.top;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWindow::window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"SuperZipMainWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SUPERZIP_APP));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_SUPERZIP_APP), IMAGE_ICON, MulDiv(16, static_cast<int>(initial_dpi), 96), MulDiv(16, static_cast<int>(initial_dpi), 96), LR_DEFAULTCOLOR));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"SuperZip",
        style,
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
        RECT fixed_window{0, 0, MulDiv(kDesignClientWidth, static_cast<int>(dpi_), 96), MulDiv(kDesignClientHeight, static_cast<int>(dpi_), 96)};
        AdjustWindowRectExForDpi(&fixed_window, window_style(), FALSE, 0, dpi_);
        SetWindowPos(
            hwnd_,
            nullptr,
            suggested->left,
            suggested->top,
            fixed_window.right - fixed_window.left,
            fixed_window.bottom - fixed_window.top,
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
            close_active_dropdown();
        } else if (x < rail_width) {
            const int nav_top = top_bar + scale(10);
            const int item_height = scale(63);
            const int item = item_height > 0 ? (y - nav_top) / item_height : -1;
            if (item >= 0 && item < 8) {
                close_active_dropdown();
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
        const bool was_empty = state_.queued_paths.empty();
        for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length + 1, L'\0');
            DragQueryFileW(drop, i, path.data(), length + 1);
            path.resize(length);
            state_.queued_paths.emplace_back(path);
        }
        if (was_empty && !state_.queued_paths.empty()) {
            state_.selected_queue_index = 0;
        }
        DragFinish(drop);
        request_repaint();
        return 0;
    }
    case WM_CREATE:
        DragAcceptFiles(hwnd_, TRUE);
        initialize_performance_monitor();
        update_performance_sample();
        SetTimer(hwnd_, kPerformanceTimer, kPerformanceSampleMs, nullptr);
        if (const UINT auto_close_ms = smoke_auto_close_ms(); auto_close_ms > 0) {
            SetTimer(hwnd_, kSmokeAutoCloseTimer, auto_close_ms, nullptr);
        }
        if (!smoke_close_marker_path().empty()) {
            SetTimer(hwnd_, kSmokeClosePollTimer, 250, nullptr);
        }
        return 0;
    case WM_APP + 1:
        repaint_queued_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (wparam == kAnimationTimer) {
            tick_animation();
            return 0;
        }
        if (wparam == kPerformanceTimer) {
            update_performance_sample();
            request_repaint();
            return 0;
        }
        if (wparam == kSmokeAutoCloseTimer) {
            DestroyWindow(hwnd_);
            return 0;
        }
        if (wparam == kSmokeClosePollTimer) {
            if (smoke_close_requested()) {
                DestroyWindow(hwnd_);
            }
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    case WM_DESTROY:
        KillTimer(hwnd_, kAnimationTimer);
        KillTimer(hwnd_, kPerformanceTimer);
        KillTimer(hwnd_, kSmokeAutoCloseTimer);
        KillTimer(hwnd_, kSmokeClosePollTimer);
        shutdown_performance_monitor();
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
    draw_tab_transition(dc, content);
    draw_active_dropdown(dc, content, state);
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
        RECT icon{item.left + scale(28), item.top + scale(8), item.left + scale(58), item.top + scale(38)};
        draw_nav_icon(dc, page, icon, active ? kText : kMuted);
        draw_text(dc, RECT{item.left + scale(4), item.top + scale(40), item.right - scale(4), item.bottom - scale(3)}, page_name(page), active ? kText : kMuted, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
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

MainWindow::QueueLayout MainWindow::queue_layout(const RECT& rect) const {
    QueueLayout layout{};
    layout.area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    const int header_top = layout.area.top;
    const int header_bottom = header_top + scale(kPageHeaderHeight);
    layout.clear = RECT{layout.area.right - scale(72), header_top, layout.area.right, header_bottom};
    layout.add_folder = RECT{layout.clear.left - scale(12) - scale(108), header_top, layout.clear.left - scale(12), header_bottom};
    layout.add_files = RECT{layout.add_folder.left - scale(12) - scale(96), header_top, layout.add_folder.left - scale(12), header_bottom};
    layout.table = RECT{layout.area.left, layout.area.top + scale(56), layout.area.right, layout.area.bottom};
    return layout;
}

MainWindow::CompressLayout MainWindow::compress_layout(const RECT& rect) const {
    CompressLayout layout{};
    layout.area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    layout.start = RECT{layout.area.right - scale(150), layout.area.bottom - scale(58), layout.area.right, layout.area.bottom - scale(18)};
    const int left = layout.area.left;
    const int mid = layout.area.left + (layout.area.right - layout.area.left) / 2 + scale(14);
    const int field_w = (layout.area.right - layout.area.left) / 2 - scale(26);
    layout.archive_name = RECT{left, layout.area.top + scale(54), left + field_w, layout.area.top + scale(104)};
    layout.destination = RECT{mid, layout.area.top + scale(54), mid + field_w, layout.area.top + scale(104)};
    layout.format = RECT{left, layout.area.top + scale(124), left + field_w, layout.area.top + scale(174)};
    layout.profile = RECT{mid, layout.area.top + scale(124), mid + field_w, layout.area.top + scale(174)};
    layout.method = RECT{left, layout.area.top + scale(194), left + field_w, layout.area.top + scale(244)};
    layout.block_size = RECT{mid, layout.area.top + scale(194), mid + field_w, layout.area.top + scale(244)};
    layout.advanced = RECT{left, layout.area.top + scale(270), layout.area.right, layout.area.top + scale(390)};
    layout.solid_archive = RECT{layout.advanced.left + scale(18), layout.advanced.top + scale(48), layout.advanced.left + scale(310), layout.advanced.top + scale(76)};
    layout.store_timestamps = RECT{layout.advanced.left + scale(18), layout.advanced.top + scale(80), layout.advanced.left + scale(310), layout.advanced.top + scale(108)};
    layout.delete_after_compression = RECT{layout.advanced.left + scale(342), layout.advanced.top + scale(48), layout.advanced.left + scale(680), layout.advanced.top + scale(76)};
    layout.verify = RECT{layout.advanced.left + scale(342), layout.advanced.top + scale(80), layout.advanced.left + scale(710), layout.advanced.top + scale(108)};
    layout.security = RECT{left, layout.area.top + scale(410), layout.area.right, layout.area.top + scale(528)};
    layout.sha = RECT{layout.security.left + scale(18), layout.security.top + scale(46), layout.security.left + scale(420), layout.security.top + scale(78)};
    layout.defender = RECT{layout.security.left + scale(18), layout.security.top + scale(82), layout.security.left + scale(420), layout.security.top + scale(114)};
    return layout;
}

MainWindow::ExtractLayout MainWindow::extract_layout(const RECT& rect) const {
    ExtractLayout layout{};
    layout.area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    layout.start = RECT{layout.area.right - scale(150), layout.area.bottom - scale(58), layout.area.right, layout.area.bottom - scale(18)};
    layout.archive = RECT{layout.area.left, layout.area.top + scale(58), layout.area.right, layout.area.top + scale(108)};
    layout.destination = RECT{layout.area.left, layout.area.top + scale(128), layout.area.right, layout.area.top + scale(178)};
    layout.path_mode = RECT{layout.area.left, layout.area.top + scale(198), layout.area.left + scale(320), layout.area.top + scale(248)};
    layout.overwrite_policy = RECT{layout.area.left + scale(342), layout.area.top + scale(198), layout.area.left + scale(662), layout.area.top + scale(248)};
    layout.checks = RECT{layout.area.left, layout.area.top + scale(280), layout.area.right, layout.area.top + scale(412)};
    layout.verify_metadata = RECT{layout.checks.left + scale(18), layout.checks.top + scale(48), layout.checks.left + scale(420), layout.checks.top + scale(78)};
    layout.open_destination_after_extract = RECT{layout.checks.left + scale(18), layout.checks.top + scale(80), layout.checks.left + scale(420), layout.checks.top + scale(110)};
    layout.sha = RECT{layout.checks.left + scale(470), layout.checks.top + scale(48), layout.checks.right - scale(20), layout.checks.top + scale(80)};
    layout.defender = RECT{layout.checks.left + scale(470), layout.checks.top + scale(84), layout.checks.right - scale(20), layout.checks.top + scale(116)};
    return layout;
}

MainWindow::PreferencesLayout MainWindow::preferences_layout(const RECT& rect) const {
    PreferencesLayout layout{};
    layout.area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    layout.restore_defaults = RECT{layout.area.right - scale(260), layout.area.bottom - scale(54), layout.area.right - scale(126), layout.area.bottom - scale(18)};
    layout.apply = RECT{layout.area.right - scale(110), layout.area.bottom - scale(54), layout.area.right, layout.area.bottom - scale(18)};
    layout.general = RECT{layout.area.left, layout.area.top + scale(56), layout.area.left + scale(470), layout.area.top + scale(224)};
    layout.security = RECT{layout.general.left, layout.general.bottom + scale(16), layout.general.right, layout.general.bottom + scale(176)};
    layout.performance = RECT{layout.general.right + scale(18), layout.general.top, layout.area.right, layout.area.top + scale(224)};
    layout.logging = RECT{layout.performance.left, layout.performance.bottom + scale(16), layout.area.right, layout.performance.bottom + scale(176)};
    layout.sha = RECT{layout.security.left + scale(18), layout.security.top + scale(48), layout.security.right - scale(16), layout.security.top + scale(80)};
    layout.defender = RECT{layout.security.left + scale(18), layout.security.top + scale(84), layout.security.right - scale(16), layout.security.top + scale(116)};
    layout.gpu = RECT{layout.security.left + scale(18), layout.security.top + scale(120), layout.security.right - scale(16), layout.security.top + scale(152)};
    layout.verify = RECT{layout.performance.left + scale(18), layout.performance.top + scale(48), layout.performance.right - scale(18), layout.performance.top + scale(80)};
    const int performance_half_right = layout.performance.left + (layout.performance.right - layout.performance.left) / 2;
    const int logging_half_right = layout.logging.left + (layout.logging.right - layout.logging.left) / 2;
    layout.memory_policy = RECT{layout.performance.left + scale(18), layout.performance.top + scale(94), performance_half_right, layout.performance.top + scale(140)};
    layout.log_level = RECT{layout.logging.left + scale(18), layout.logging.top + scale(48), logging_half_right, layout.logging.top + scale(94)};
    layout.log_retention = RECT{layout.logging.left + scale(18), layout.logging.top + scale(106), logging_half_right, layout.logging.top + scale(152)};
    layout.open_destination_after_operation = RECT{layout.general.left + scale(18), layout.general.top + scale(48), layout.general.right - scale(16), layout.general.top + scale(78)};
    layout.confirm_before_deleting = RECT{layout.general.left + scale(18), layout.general.top + scale(82), layout.general.right - scale(16), layout.general.top + scale(112)};
    layout.show_operation_summary = RECT{layout.general.left + scale(18), layout.general.top + scale(116), layout.general.right - scale(16), layout.general.top + scale(146)};
    return layout;
}

void MainWindow::draw_queue_page(HDC dc, const RECT& rect, const UiState& state) {
    const auto layout = queue_layout(rect);
    const RECT area = layout.area;
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, layout.add_files.left - scale(18), area.top + scale(kPageHeaderHeight)}, L"Queue", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_button(dc, layout.add_files, L"+ Add files", false);
    draw_button(dc, layout.add_folder, L"+ Add folder", false);
    draw_button(dc, layout.clear, L"Clear", false);
    SelectObject(dc, tiny_font_);
    const auto count_text = std::to_wstring(state.queued_paths.size()) + L" item" + (state.queued_paths.size() == 1 ? L"" : L"s");
    draw_text(dc, RECT{layout.add_files.left - scale(120), area.top, layout.add_files.left - scale(18), area.top + scale(kPageHeaderHeight)}, count_text, kMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    RECT table = layout.table;
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
        int row_index = 0;
        for (const auto& path : state.queued_paths) {
            const int row_bottom = y + scale(34);
            const bool selected = row_index == state.selected_queue_index;
            fill_rect(dc, RECT{table.left + scale(1), y + scale(1), table.right - scale(1), row_bottom}, selected ? kPanel3 : (((y / scale(34)) % 2 == 0) ? kPanel2 : kPanel));
            draw_checkbox(dc, RECT{table.left + scale(12), y + scale(6), table.left + scale(64), row_bottom - scale(4)}, L"", true);
            draw_text(dc, RECT{table.left + scale(82), y, table.left + scale(330), row_bottom}, path.filename().wstring(), kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            draw_text(dc, RECT{table.left + scale(340), y, table.left + scale(450), row_bottom}, entry_size_text(path), kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            draw_text(dc, RECT{table.left + scale(462), y, table.left + scale(560), row_bottom}, entry_type_text(path), kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            draw_text(dc, RECT{table.left + scale(572), y, table.right - scale(12), row_bottom}, path.wstring(), kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y = row_bottom;
            ++row_index;
            if (y > table.bottom - scale(34)) {
                break;
            }
        }
    }
}

void MainWindow::draw_compress_page(HDC dc, const RECT& rect, const UiState& state) {
    const auto layout = compress_layout(rect);
    const RECT area = layout.area;
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(34)}, L"Compress", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_button(dc, layout.start, L"Start", true);

    draw_field(dc, layout.archive_name, L"Archive name", L"SuperZip-output.suzip", false);
    draw_field(dc, layout.destination, L"Destination", destination_directory_or_default(state).wstring(), false);
    draw_field(dc, layout.format, L"Archive format", L"SuperZip GPU (.suzip)", false);
    draw_field(dc, layout.profile, L"Compression profile", compression_profile_text(state.compression_profile_index), true);
    draw_field(dc, layout.method, L"Compression method", state.gpu_required ? L"AMD HIP required" : L"AMD HIP preferred", true);
    draw_field(dc, layout.block_size, L"Block size", compression_block_size_text(state.compression_block_size_index), true);

    RECT advanced = layout.advanced;
    fill_round_rect(dc, advanced, kPanel, scale(4));
    stroke_rect(dc, advanced, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{advanced.left + scale(16), advanced.top + scale(12), advanced.right, advanced.top + scale(36)}, L"Advanced", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_checkbox(dc, layout.solid_archive, L"Solid archive", state.solid_archive);
    draw_checkbox(dc, layout.store_timestamps, L"Store timestamps", state.store_timestamps);
    draw_checkbox(dc, layout.delete_after_compression, L"Delete files after compression", state.delete_after_compression);
    draw_toggle(dc, layout.verify, L"Verify archive after write", state.verify_after_write_opt_in, ToggleId::VerifyAfterWrite);

    RECT security = layout.security;
    fill_round_rect(dc, security, kPanel, scale(4));
    stroke_rect(dc, security, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{security.left + scale(16), security.top + scale(12), security.right, security.top + scale(36)}, L"Integrity and Security", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_toggle(dc, layout.sha, L"SHA-256 integrity check", state.integrity_hash_opt_in, ToggleId::IntegrityHash);
    draw_toggle(dc, layout.defender, L"Microsoft Defender scan", state.defender_scan_opt_in, ToggleId::DefenderScan);
    draw_text(dc, RECT{security.left + scale(448), security.top + scale(46), security.right - scale(16), security.top + scale(114)}, L"Security checks remain disabled until explicitly enabled for the job or as a default in Settings.", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void MainWindow::draw_extract_page(HDC dc, const RECT& rect, const UiState& state) {
    const auto layout = extract_layout(rect);
    const RECT area = layout.area;
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(34)}, L"Extract", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_button(dc, layout.start, L"Start", true);

    const auto archive = state.queued_paths.empty() ? L"Select an archive from the queue" : state.queued_paths.front().wstring();
    draw_field(dc, layout.archive, L"Archive", archive, false);
    draw_field(dc, layout.destination, L"Destination", extraction_output_path_for(state).wstring(), false);
    draw_field(dc, layout.path_mode, L"Path mode", L"Full paths", false);
    draw_field(dc, layout.overwrite_policy, L"Overwrite policy", state.overwrite ? L"Overwrite enabled" : L"Ask before overwrite", true);

    RECT checks = layout.checks;
    fill_round_rect(dc, checks, kPanel, scale(4));
    stroke_rect(dc, checks, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{checks.left + scale(16), checks.top + scale(12), checks.right, checks.top + scale(36)}, L"Integrity and Security", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_checkbox(dc, layout.verify_metadata, L"Verify archive metadata before extraction", state.verify_metadata_before_extract);
    draw_checkbox(dc, layout.open_destination_after_extract, L"Open destination folder after extraction", state.open_destination_after_extract);
    draw_toggle(dc, layout.sha, L"SHA-256 integrity check", state.integrity_hash_opt_in, ToggleId::IntegrityHash);
    draw_toggle(dc, layout.defender, L"Microsoft Defender scan", state.defender_scan_opt_in, ToggleId::DefenderScan);
}

void MainWindow::draw_security_page(HDC dc, const RECT& rect, const UiState& state) {
    RECT area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
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
        {L"Post-write verify", state.verify_after_write_opt_in ? L"Selected" : L"Not selected", state.verify_after_write_opt_in ? kOk : kWarn},
        {L"SHA-256 optional", state.integrity_hash_opt_in ? L"Selected" : L"Not selected", state.integrity_hash_opt_in ? kOk : kWarn},
        {L"Defender optional", state.defender_scan_opt_in ? L"Selected" : L"Not selected", state.defender_scan_opt_in ? kOk : kWarn},
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
    RECT area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(34)}, L"History", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_button(dc, RECT{area.right - scale(142), area.top, area.right, area.top + scale(34)}, L"Clear History", false);
    draw_field(dc, RECT{area.left, area.top + scale(48), area.left + scale(220), area.top + scale(92)}, L"Operation", history_operation_filter_text(state.history_operation_filter_index), true);
    draw_field(dc, RECT{area.left + scale(238), area.top + scale(48), area.left + scale(458), area.top + scale(92)}, L"Status", history_status_filter_text(state.history_status_filter_index), true);

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
    RECT area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
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
    const std::wstring gpu_detail = ready
        ? (std::wstring(L"Backend: AMD HIP\nDevice: ") +
              (state.gpu_device_name.empty() ? L"Detected by HIP runtime" : widen(state.gpu_device_name)) +
              L"\nArchitecture: " +
              (state.gpu_arch.empty() ? L"Runtime default" : widen(state.gpu_arch)))
        : L"Backend unavailable\nNo CUDA/WebGPU fallback\nHost stays AMD-only";
    draw_text(dc, RECT{gpu.left + scale(16), gpu.top + scale(48), gpu.right - scale(16), gpu.bottom - scale(14)}, gpu_detail, kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    draw_text(dc, RECT{memory.left + scale(16), memory.top + scale(48), memory.right - scale(16), memory.bottom - scale(14)}, L"Bounded chunks keep archive work from loading whole archives into RAM. ZIP compatibility uses miniz streaming file APIs.", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    draw_text(dc, RECT{accel.left + scale(16), accel.top + scale(48), accel.right - scale(16), accel.bottom - scale(14)}, state.gpu_required ? L"Mode: GPU required\nFallback: blocked for .suzip jobs\nDevice scope: AMD HIP only" : L"Mode: GPU preferred\nFallback: CPU codec allowed\nDevice scope: AMD HIP only", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);

    draw_performance_monitor(dc, RECT{area.left, area.top + scale(342), area.right, area.bottom}, state.performance);
}

void MainWindow::draw_performance_monitor_card(
    HDC dc,
    const RECT& graph,
    const wchar_t* label,
    const std::wstring& value,
    const std::wstring& detail,
    double ratio,
    COLORREF color) {
    fill_round_rect(dc, graph, kPanel2, scale(4));
    stroke_rect(dc, graph, kBorder);
    RECT value_rect{graph.left + scale(12), graph.top + scale(10), graph.right - scale(12), graph.top + scale(42)};
    RECT detail_rect{graph.left + scale(12), graph.top + scale(44), graph.right - scale(12), graph.bottom - scale(28)};
    RECT bar_track{graph.left + scale(12), graph.bottom - scale(20), graph.right - scale(12), graph.bottom - scale(12)};
    const int bar_width = std::max(0, static_cast<int>(bar_track.right - bar_track.left));
    RECT bar_fill = bar_track;
    bar_fill.right = bar_fill.left + static_cast<int>(std::round(static_cast<double>(bar_width) * std::clamp(ratio, 0.0, 1.0)));
    fill_round_rect(dc, bar_track, RGB(35, 50, 56), scale(4));
    if (bar_fill.right > bar_fill.left) {
        fill_round_rect(dc, bar_fill, color, scale(4));
    }
    draw_text(dc, value_rect, value, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw_text(dc, detail_rect, detail, kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    draw_text(dc, RECT{graph.left, graph.bottom + scale(4), graph.right, graph.bottom + scale(24)}, label, kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void MainWindow::draw_performance_monitor(HDC dc, const RECT& monitor, const PerformanceMonitorSample& sample) {
    fill_round_rect(dc, monitor, kPanel, scale(4));
    stroke_rect(dc, monitor, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{monitor.left + scale(16), monitor.top + scale(12), monitor.right, monitor.top + scale(36)}, L"Performance Monitor", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    const int graph_top = monitor.top + scale(60);
    const int graph_bottom = monitor.bottom - scale(42);
    const int graph_w = (monitor.right - monitor.left - scale(86)) / 4;
    const double io_total = sample.io_read_bytes_per_second + sample.io_write_bytes_per_second;
    for (int i = 0; i < 4; ++i) {
        RECT graph{monitor.left + scale(18) + i * (graph_w + scale(16)), graph_top, monitor.left + scale(18) + i * (graph_w + scale(16)) + graph_w, graph_bottom};
        if (!sample.live) {
            draw_performance_monitor_card(dc, graph, i == 0 ? L"CPU" : i == 1 ? L"Memory" : i == 2 ? L"I/O" : L"GPU", L"Collecting", L"Waiting for the first live sample.", 0.0, kSubtle);
            continue;
        }
        if (i == 0) {
            draw_performance_monitor_card(
                dc,
                graph,
                L"CPU",
                percentage_text(sample.cpu_percent),
                L"Process utilization across logical CPUs.",
                sample.cpu_percent / 100.0,
                kInfo);
        } else if (i == 1) {
            const auto detail = std::wstring(L"Private ") + widen(human_bytes(static_cast<double>(sample.private_bytes))) + L"\nSystem " + percentage_text(sample.system_memory_percent);
            draw_performance_monitor_card(
                dc,
                graph,
                L"Memory",
                widen(human_bytes(static_cast<double>(sample.private_bytes))),
                detail,
                sample.system_memory_percent / 100.0,
                kOk);
        } else if (i == 2) {
            const auto detail = std::wstring(L"Read ") + rate_text(sample.io_read_bytes_per_second) + L"\nWrite " + rate_text(sample.io_write_bytes_per_second);
            draw_performance_monitor_card(
                dc,
                graph,
                L"I/O",
                rate_text(io_total),
                detail,
                std::min(1.0, io_total / (1024.0 * 1024.0 * 1024.0)),
                kWarn);
        } else {
            const bool has_vram = sample.vram_total_bytes > 0;
            const double vram_ratio = has_vram
                ? static_cast<double>(sample.vram_total_bytes - sample.vram_free_bytes) / static_cast<double>(sample.vram_total_bytes)
                : 0.0;
            const std::wstring gpu_value = sample.gpu_utilization_available ? percentage_text(sample.gpu_utilization_percent) : L"Unavailable";
            const std::wstring detail = has_vram
                ? (std::wstring(L"VRAM free ") + widen(human_bytes(static_cast<double>(sample.vram_free_bytes))) + L"\nTotal " + widen(human_bytes(static_cast<double>(sample.vram_total_bytes))))
                : L"Windows GPU counter or HIP VRAM is not exposed.";
            draw_performance_monitor_card(
                dc,
                graph,
                L"GPU",
                gpu_value,
                detail,
                sample.gpu_utilization_available ? sample.gpu_utilization_percent / 100.0 : vram_ratio,
                kAccent);
        }
    }
}

void MainWindow::draw_preferences_page(HDC dc, const RECT& rect, const UiState& state) {
    const auto layout = preferences_layout(rect);
    RECT area = layout.area;
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(34)}, L"Preferences", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_button(dc, layout.restore_defaults, L"Restore Defaults", false);
    draw_button(dc, layout.apply, L"Apply", true);

    RECT general = layout.general;
    RECT security = layout.security;
    RECT performance = layout.performance;
    RECT logging = layout.logging;
    for (RECT panel : {general, security, performance, logging}) {
        fill_round_rect(dc, panel, kPanel, scale(4));
        stroke_rect(dc, panel, kBorder);
    }
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{general.left + scale(16), general.top + scale(12), general.right, general.top + scale(36)}, L"General", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{security.left + scale(16), security.top + scale(12), security.right, security.top + scale(36)}, L"Security", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{performance.left + scale(16), performance.top + scale(12), performance.right, performance.top + scale(36)}, L"Performance", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{logging.left + scale(16), logging.top + scale(12), logging.right, logging.top + scale(36)}, L"Logging", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_checkbox(dc, layout.open_destination_after_operation, L"Open destination folder after operation", state.open_destination_after_operation);
    draw_checkbox(dc, layout.confirm_before_deleting, L"Confirm before deleting files", state.confirm_before_deleting);
    draw_checkbox(dc, layout.show_operation_summary, L"Show operation summary", state.show_operation_summary);
    draw_toggle(dc, layout.sha, L"SHA-256 integrity check", state.integrity_hash_opt_in, ToggleId::IntegrityHash);
    draw_toggle(dc, layout.defender, L"Microsoft Defender scan", state.defender_scan_opt_in, ToggleId::DefenderScan);
    draw_toggle(dc, layout.gpu, L"Require AMD GPU acceleration", state.gpu_required, ToggleId::GpuRequired);
    draw_toggle(dc, layout.verify, L"Verify archive after write", state.verify_after_write_opt_in, ToggleId::VerifyAfterWrite);
    draw_field(dc, layout.memory_policy, L"Memory policy", memory_policy_text(state.memory_policy_index), true);
    draw_field(dc, layout.log_level, L"Log level", log_level_text(state.log_level_index), true);
    draw_field(dc, layout.log_retention, L"Log retention", log_retention_text(state.log_retention_index), true);
}

void MainWindow::draw_about_page(HDC dc, const RECT& rect) {
    RECT area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
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

void MainWindow::draw_toggle(HDC dc, const RECT& rect, const wchar_t* text, bool enabled, ToggleId id) {
    draw_text(dc, RECT{rect.left + scale(54), rect.top, rect.right, rect.bottom}, text, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const int cy = (rect.top + rect.bottom) / 2;
    const int track_height = scale(18);
    RECT track{rect.left, cy - track_height / 2, rect.left + scale(42), cy + track_height / 2};
    const double position = toggle_visual_position(id, enabled);
    fill_round_rect(dc, track, blend_color(RGB(57, 69, 75), RGB(43, 111, 72), position), scale(14));
    HBRUSH knob = CreateSolidBrush(blend_color(kMuted, kOk, position));
    HGDIOBJ previous = SelectObject(dc, knob);
    const int knob_size = scale(14);
    const int knob_travel = (track.right - track.left) - knob_size - scale(6);
    const int knob_left = track.left + scale(3) + static_cast<int>(std::round(position * static_cast<double>(std::max(0, knob_travel))));
    Ellipse(dc, knob_left, cy - knob_size / 2, knob_left + knob_size, cy + knob_size / 2);
    SelectObject(dc, previous);
    DeleteObject(knob);
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
        const int arrow_cx = box.right - scale(15);
        const int arrow_cy = (box.top + box.bottom) / 2;
        POINT arrow[3] = {
            {arrow_cx - scale(5), arrow_cy - scale(2)},
            {arrow_cx + scale(5), arrow_cy - scale(2)},
            {arrow_cx, arrow_cy + scale(4)},
        };
        HBRUSH brush = CreateSolidBrush(kMuted);
        HGDIOBJ previous = SelectObject(dc, brush);
        Polygon(dc, arrow, 3);
        SelectObject(dc, previous);
        DeleteObject(brush);
    }
}

void MainWindow::draw_active_dropdown(HDC dc, const RECT& content, const UiState& state) {
    if (state.active_dropdown == DropdownId::None) {
        return;
    }
    const auto options = dropdown_options(state.active_dropdown);
    if (options.empty()) {
        return;
    }
    const RECT menu = dropdown_menu_rect(state.active_dropdown, content);
    if (menu.right <= menu.left || menu.bottom <= menu.top) {
        return;
    }
    const RECT shadow{menu.left + scale(3), menu.top + scale(3), menu.right + scale(3), menu.bottom + scale(3)};
    fill_round_rect(dc, shadow, RGB(6, 10, 12), scale(4));
    fill_round_rect(dc, menu, kPanel2, scale(4));
    stroke_rect(dc, menu, kAccent);

    const int row_height = scale(32);
    const int selected = dropdown_selected_index(state, state.active_dropdown);
    SelectObject(dc, tiny_font_);
    for (int index = 0; index < static_cast<int>(options.size()); ++index) {
        const RECT row{menu.left + scale(1), menu.top + scale(1) + (index * row_height), menu.right - scale(1), menu.top + scale(1) + ((index + 1) * row_height)};
        const bool is_selected = index == selected;
        fill_rect(dc, row, is_selected ? RGB(126, 24, 31) : ((index % 2 == 0) ? kPanel2 : kPanel));
        if (index > 0) {
            draw_line(dc, menu.left + scale(8), row.top, menu.right - scale(8), row.top, kBorder);
        }
        if (is_selected) {
            const int check_mid = (row.top + row.bottom) / 2;
            draw_line(dc, row.left + scale(12), check_mid, row.left + scale(17), check_mid + scale(5), kText);
            draw_line(dc, row.left + scale(17), check_mid + scale(5), row.left + scale(27), check_mid - scale(6), kText);
        }
        draw_text(dc, RECT{row.left + scale(36), row.top, row.right - scale(12), row.bottom}, options[static_cast<std::size_t>(index)], is_selected ? kText : kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

RECT MainWindow::dropdown_anchor_rect(DropdownId id, const RECT& content) const {
    switch (id) {
    case DropdownId::CompressProfile:
        return compress_layout(content).profile;
    case DropdownId::CompressMethod:
        return compress_layout(content).method;
    case DropdownId::CompressBlockSize:
        return compress_layout(content).block_size;
    case DropdownId::ExtractOverwrite:
        return extract_layout(content).overwrite_policy;
    case DropdownId::HistoryOperation: {
        const RECT area = inset_rect(content, scale(kPageInsetX), scale(kPageInsetY));
        return RECT{area.left, area.top + scale(48), area.left + scale(220), area.top + scale(92)};
    }
    case DropdownId::HistoryStatus: {
        const RECT area = inset_rect(content, scale(kPageInsetX), scale(kPageInsetY));
        return RECT{area.left + scale(238), area.top + scale(48), area.left + scale(458), area.top + scale(92)};
    }
    case DropdownId::PreferencesMemoryPolicy:
        return preferences_layout(content).memory_policy;
    case DropdownId::PreferencesLogLevel:
        return preferences_layout(content).log_level;
    case DropdownId::PreferencesLogRetention:
        return preferences_layout(content).log_retention;
    case DropdownId::None:
        return RECT{};
    }
    return RECT{};
}

RECT MainWindow::dropdown_menu_rect(DropdownId id, const RECT& content) const {
    const auto options = dropdown_options(id);
    if (options.empty()) {
        return RECT{};
    }
    RECT anchor = dropdown_anchor_rect(id, content);
    if (anchor.right <= anchor.left || anchor.bottom <= anchor.top) {
        return RECT{};
    }
    const int gap = scale(4);
    const int row_height = scale(32);
    const int menu_height = row_height * static_cast<int>(options.size()) + scale(2);
    int top = anchor.bottom + gap;
    int bottom = top + menu_height;
    if (bottom > content.bottom - scale(8)) {
        bottom = anchor.top - gap;
        top = bottom - menu_height;
    }
    if (top < content.top + scale(8)) {
        top = content.top + scale(8);
        bottom = top + menu_height;
    }
    return RECT{anchor.left, top, anchor.right, bottom};
}

void MainWindow::draw_tab_transition(HDC dc, const RECT& rect) {
    const double progress = page_transition_progress();
    if (progress >= 1.0) {
        return;
    }
    const int width = rect.right - rect.left;
    const int line_width = static_cast<int>(std::round(static_cast<double>(width) * ease_out(progress)));
    fill_rect(dc, RECT{rect.left, rect.top, rect.left + std::max(scale(12), line_width), rect.top + scale(2)}, kAccent);
}

RECT MainWindow::content_rect() const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int top_bar = scale(kTopBar);
    const int rail_width = scale(kRailWidth);
    const int status_bar = scale(kStatusBar);
    return RECT{rail_width, top_bar, client.right, client.bottom - status_bar};
}

bool MainWindow::handle_content_click(int x, int y) {
    Page page;
    {
        std::lock_guard lock(mutex_);
        page = state_.page;
    }
    const RECT content = content_rect();

    // Local mutators centralize repaint and animation behavior for repeated control rows.
    const auto toggle_bool = [this](bool UiState::*member, ToggleId id) {
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
    };
    const auto checkbox_bool = [this](bool UiState::*member, const char* status) {
        {
            std::lock_guard lock(mutex_);
            state_.*member = !(state_.*member);
            state_.status = status;
        }
        request_repaint();
        return true;
    };

    // Queue clicks cover only file/folder selection and row selection; job controls live on later pages.
    if (page == Page::Queue) {
        const auto layout = queue_layout(content);
        if (handle_active_dropdown_click(x, y)) {
            return true;
        }
        if (contains_point(layout.add_files, x, y)) {
            add_files();
            return true;
        }
        if (contains_point(layout.add_folder, x, y)) {
            add_folder();
            return true;
        }
        if (contains_point(layout.clear, x, y)) {
            clear_queue();
            return true;
        }
        const int header_bottom = layout.table.top + scale(36);
        const int row_height = scale(34);
        if (y >= header_bottom && y < layout.table.bottom && row_height > 0) {
            const int index = (y - header_bottom) / row_height;
            std::lock_guard lock(mutex_);
            if (index >= 0 && index < static_cast<int>(state_.queued_paths.size())) {
                state_.selected_queue_index = index;
                request_repaint();
                return true;
            }
        }
        return false;
    }

    // Compress keeps dropdowns ahead of toggles so expanded menus consume their own option clicks.
    if (page == Page::Compress) {
        const auto layout = compress_layout(content);
        if (handle_active_dropdown_click(x, y)) {
            return true;
        }
        if (contains_point(layout.start, x, y)) {
            start_compress();
            return true;
        }
        if (contains_point(layout.destination, x, y)) {
            choose_destination();
            return true;
        }
        if (contains_point(layout.profile, x, y)) {
            open_dropdown(DropdownId::CompressProfile);
            return true;
        }
        if (contains_point(layout.method, x, y)) {
            open_dropdown(DropdownId::CompressMethod);
            return true;
        }
        if (contains_point(layout.block_size, x, y)) {
            open_dropdown(DropdownId::CompressBlockSize);
            return true;
        }
        if (contains_point(layout.solid_archive, x, y)) {
            return checkbox_bool(&UiState::solid_archive, "Solid archive setting changed");
        }
        if (contains_point(layout.store_timestamps, x, y)) {
            return checkbox_bool(&UiState::store_timestamps, "Timestamp setting changed");
        }
        if (contains_point(layout.delete_after_compression, x, y)) {
            return checkbox_bool(&UiState::delete_after_compression, "Delete-after-compression setting changed");
        }
        if (contains_point(layout.verify, x, y)) {
            return toggle_bool(&UiState::verify_after_write_opt_in, ToggleId::VerifyAfterWrite);
        }
        if (contains_point(layout.sha, x, y)) {
            return toggle_bool(&UiState::integrity_hash_opt_in, ToggleId::IntegrityHash);
        }
        if (contains_point(layout.defender, x, y)) {
            return toggle_bool(&UiState::defender_scan_opt_in, ToggleId::DefenderScan);
        }
        return false;
    }

    // Extract routes the overwrite policy through the dropdown model instead of hidden cycling.
    if (page == Page::Extract) {
        const auto layout = extract_layout(content);
        if (handle_active_dropdown_click(x, y)) {
            return true;
        }
        if (contains_point(layout.start, x, y)) {
            start_extract();
            return true;
        }
        if (contains_point(layout.destination, x, y)) {
            choose_destination();
            return true;
        }
        if (contains_point(layout.overwrite_policy, x, y)) {
            open_dropdown(DropdownId::ExtractOverwrite);
            return true;
        }
        if (contains_point(layout.verify_metadata, x, y)) {
            return checkbox_bool(&UiState::verify_metadata_before_extract, "Extraction metadata verification setting changed");
        }
        if (contains_point(layout.open_destination_after_extract, x, y)) {
            return checkbox_bool(&UiState::open_destination_after_extract, "Open destination after extraction setting changed");
        }
        if (contains_point(layout.sha, x, y)) {
            return toggle_bool(&UiState::integrity_hash_opt_in, ToggleId::IntegrityHash);
        }
        if (contains_point(layout.defender, x, y)) {
            return toggle_bool(&UiState::defender_scan_opt_in, ToggleId::DefenderScan);
        }
        return false;
    }

    // History filter controls use the same dropdown path as settings for consistent testing.
    if (page == Page::History) {
        RECT area = inset_rect(content, scale(kPageInsetX), scale(kPageInsetY));
        const RECT operation{area.left, area.top + scale(48), area.left + scale(220), area.top + scale(92)};
        const RECT status{area.left + scale(238), area.top + scale(48), area.left + scale(458), area.top + scale(92)};
        const RECT clear{area.right - scale(142), area.top, area.right, area.top + scale(34)};
        if (handle_active_dropdown_click(x, y)) {
            return true;
        }
        if (contains_point(operation, x, y)) {
            open_dropdown(DropdownId::HistoryOperation);
            return true;
        }
        if (contains_point(status, x, y)) {
            open_dropdown(DropdownId::HistoryStatus);
            return true;
        }
        if (contains_point(clear, x, y)) {
            clear_history();
            return true;
        }
        return false;
    }

    if (page == Page::Security) {
        RECT area = inset_rect(content, scale(kPageInsetX), scale(kPageInsetY));
        const RECT verify_button{area.right - scale(150), area.bottom - scale(58), area.right, area.bottom - scale(18)};
        if (contains_point(verify_button, x, y)) {
            append_history("Security review completed");
            {
                std::lock_guard lock(mutex_);
                state_.status = "Security review completed";
            }
            request_repaint();
            return true;
        }
        return false;
    }

    if (page == Page::Gpu) {
        RECT area = inset_rect(content, scale(kPageInsetX), scale(kPageInsetY));
        const RECT refresh{area.right - scale(110), area.top, area.right, area.top + scale(34)};
        if (contains_point(refresh, x, y)) {
            refresh_gpu_status();
            request_repaint();
            return true;
        }
        return false;
    }

    // Preferences mixes persistent defaults, opt-in security toggles, and dropdown policy fields.
    if (page == Page::Preferences) {
        const auto layout = preferences_layout(content);
        if (handle_active_dropdown_click(x, y)) {
            return true;
        }
        if (contains_point(layout.restore_defaults, x, y)) {
            restore_defaults();
            return true;
        }
        if (contains_point(layout.apply, x, y)) {
            append_history("Preferences applied for current session");
            {
                std::lock_guard lock(mutex_);
                state_.status = "Preferences applied";
            }
            request_repaint();
            return true;
        }
        if (contains_point(layout.sha, x, y)) {
            return toggle_bool(&UiState::integrity_hash_opt_in, ToggleId::IntegrityHash);
        }
        if (contains_point(layout.defender, x, y)) {
            return toggle_bool(&UiState::defender_scan_opt_in, ToggleId::DefenderScan);
        }
        if (contains_point(layout.gpu, x, y)) {
            return toggle_bool(&UiState::gpu_required, ToggleId::GpuRequired);
        }
        if (contains_point(layout.verify, x, y)) {
            return toggle_bool(&UiState::verify_after_write_opt_in, ToggleId::VerifyAfterWrite);
        }
        if (contains_point(layout.open_destination_after_operation, x, y)) {
            return checkbox_bool(&UiState::open_destination_after_operation, "Open destination setting changed");
        }
        if (contains_point(layout.confirm_before_deleting, x, y)) {
            return checkbox_bool(&UiState::confirm_before_deleting, "Delete confirmation setting changed");
        }
        if (contains_point(layout.show_operation_summary, x, y)) {
            return checkbox_bool(&UiState::show_operation_summary, "Operation summary setting changed");
        }
        if (contains_point(layout.memory_policy, x, y)) {
            open_dropdown(DropdownId::PreferencesMemoryPolicy);
            return true;
        }
        if (contains_point(layout.log_level, x, y)) {
            open_dropdown(DropdownId::PreferencesLogLevel);
            return true;
        }
        if (contains_point(layout.log_retention, x, y)) {
            open_dropdown(DropdownId::PreferencesLogRetention);
            return true;
        }
        return false;
    }
    return false;
}

bool MainWindow::handle_active_dropdown_click(int x, int y) {
    DropdownId active = DropdownId::None;
    {
        std::lock_guard lock(mutex_);
        active = state_.active_dropdown;
    }
    if (active == DropdownId::None) {
        return false;
    }

    const RECT content = content_rect();
    const RECT menu = dropdown_menu_rect(active, content);
    const RECT anchor = dropdown_anchor_rect(active, content);
    if (contains_point(menu, x, y)) {
        const int row_height = scale(32);
        const int option_index = row_height > 0 ? (y - menu.top - scale(1)) / row_height : -1;
        const auto options = dropdown_options(active);
        if (option_index >= 0 && option_index < static_cast<int>(options.size())) {
            select_dropdown_option(active, option_index);
            return true;
        }
    }
    if (contains_point(anchor, x, y)) {
        close_active_dropdown();
        return true;
    }
    close_active_dropdown();
    return true;
}

void MainWindow::open_dropdown(DropdownId id) {
    {
        std::lock_guard lock(mutex_);
        state_.active_dropdown = id;
        state_.status = "Dropdown opened";
    }
    request_repaint();
}

void MainWindow::close_active_dropdown() {
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        changed = state_.active_dropdown != DropdownId::None;
        state_.active_dropdown = DropdownId::None;
    }
    if (changed) {
        request_repaint();
    }
}

void MainWindow::select_dropdown_option(DropdownId id, int option_index) {
    {
        std::lock_guard lock(mutex_);
        switch (id) {
        case DropdownId::CompressProfile:
            state_.compression_profile_index = std::clamp(option_index, 0, 2);
            state_.status = "Compression profile changed";
            break;
        case DropdownId::CompressMethod:
            state_.gpu_required = option_index == 0;
            state_.status = "Compression method changed";
            break;
        case DropdownId::CompressBlockSize:
            state_.compression_block_size_index = std::clamp(option_index, 0, 3);
            state_.status = "Block size changed";
            break;
        case DropdownId::ExtractOverwrite:
            state_.overwrite = option_index == 1;
            state_.status = "Overwrite policy changed";
            break;
        case DropdownId::HistoryOperation:
            state_.history_operation_filter_index = std::clamp(option_index, 0, 3);
            state_.status = "History operation filter changed";
            break;
        case DropdownId::HistoryStatus:
            state_.history_status_filter_index = std::clamp(option_index, 0, 2);
            state_.status = "History status filter changed";
            break;
        case DropdownId::PreferencesMemoryPolicy:
            state_.memory_policy_index = std::clamp(option_index, 0, 2);
            state_.status = "Memory policy changed";
            break;
        case DropdownId::PreferencesLogLevel:
            state_.log_level_index = std::clamp(option_index, 0, 2);
            state_.status = "Log level changed";
            break;
        case DropdownId::PreferencesLogRetention:
            state_.log_retention_index = std::clamp(option_index, 0, 2);
            state_.status = "Log retention changed";
            break;
        case DropdownId::None:
            break;
        }
        state_.active_dropdown = DropdownId::None;
    }
    request_repaint();
}

void MainWindow::set_page(Page page) {
    Page previous;
    {
        std::lock_guard lock(mutex_);
        previous = state_.page;
        if (previous == page) {
            return;
        }
        state_.page = page;
        state_.active_dropdown = DropdownId::None;
    }
    start_page_transition(previous, page);
    request_repaint();
}

void MainWindow::add_files() {
    wchar_t smoke_paths[32768]{};
    constexpr DWORD smoke_paths_capacity = static_cast<DWORD>(sizeof(smoke_paths) / sizeof(smoke_paths[0]));
    const DWORD smoke_length = GetEnvironmentVariableW(L"SUPERZIP_GUI_SMOKE_FILE_SELECTION", smoke_paths, smoke_paths_capacity);
    if (smoke_length > 0 && smoke_length < smoke_paths_capacity) {
        std::lock_guard lock(mutex_);
        const bool was_empty = state_.queued_paths.empty();
        std::wstring_view paths(smoke_paths, smoke_length);
        std::size_t start = 0;
        while (start <= paths.size()) {
            const std::size_t end = paths.find(L';', start);
            const auto part = paths.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start);
            if (!part.empty()) {
                state_.queued_paths.emplace_back(std::wstring(part));
            }
            if (end == std::wstring_view::npos) {
                break;
            }
            start = end + 1;
        }
        if (was_empty && !state_.queued_paths.empty()) {
            state_.selected_queue_index = 0;
        }
        state_.status = "Smoke files added";
        request_repaint();
        return;
    }

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
    const bool was_empty = state_.queued_paths.empty();
    if (*cursor == L'\0') {
        state_.queued_paths.push_back(dir);
    } else {
        while (*cursor != L'\0') {
            std::filesystem::path name(cursor);
            state_.queued_paths.push_back(dir / name);
            cursor += name.wstring().size() + 1;
        }
    }
    if (was_empty && !state_.queued_paths.empty()) {
        state_.selected_queue_index = 0;
    }
    request_repaint();
}

void MainWindow::add_folder() {
    wchar_t smoke_path[32768]{};
    constexpr DWORD smoke_path_capacity = static_cast<DWORD>(sizeof(smoke_path) / sizeof(smoke_path[0]));
    const DWORD smoke_length = GetEnvironmentVariableW(L"SUPERZIP_GUI_SMOKE_FOLDER_SELECTION", smoke_path, smoke_path_capacity);
    if (smoke_length > 0 && smoke_length < smoke_path_capacity) {
        std::lock_guard lock(mutex_);
        const bool was_empty = state_.queued_paths.empty();
        state_.queued_paths.emplace_back(smoke_path);
        if (was_empty) {
            state_.selected_queue_index = 0;
        }
        state_.status = "Smoke folder added";
        request_repaint();
        return;
    }

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
        const bool was_empty = state_.queued_paths.empty();
        state_.queued_paths.emplace_back(path);
        if (was_empty) {
            state_.selected_queue_index = 0;
        }
    }
    request_repaint();
}

void MainWindow::clear_queue() {
    {
        std::lock_guard lock(mutex_);
        state_.queued_paths.clear();
        state_.selected_queue_index = -1;
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

void MainWindow::choose_destination() {
    wchar_t smoke_path[32768]{};
    constexpr DWORD smoke_path_capacity = static_cast<DWORD>(sizeof(smoke_path) / sizeof(smoke_path[0]));
    const DWORD smoke_length = GetEnvironmentVariableW(L"SUPERZIP_GUI_SMOKE_DESTINATION", smoke_path, smoke_path_capacity);
    if (smoke_length > 0 && smoke_length < smoke_path_capacity) {
        std::lock_guard lock(mutex_);
        state_.destination_directory = smoke_path;
        state_.status = "Destination selected";
        request_repaint();
        return;
    }

    BROWSEINFOW browse{};
    browse.hwndOwner = hwnd_;
    browse.lpszTitle = L"Choose SuperZip destination";
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
        state_.destination_directory = path;
        state_.status = "Destination selected";
    }
    request_repaint();
}

void MainWindow::cycle_compression_profile() {
    {
        std::lock_guard lock(mutex_);
        state_.compression_profile_index = (state_.compression_profile_index + 1) % 3;
        state_.status = "Compression profile changed";
    }
    request_repaint();
}

void MainWindow::restore_defaults() {
    {
        std::lock_guard lock(mutex_);
        state_.destination_directory.clear();
        state_.selected_queue_index = state_.queued_paths.empty() ? -1 : 0;
        state_.compression_profile_index = 0;
        state_.compression_block_size_index = 1;
        state_.memory_policy_index = 0;
        state_.log_level_index = 0;
        state_.log_retention_index = 0;
        state_.history_operation_filter_index = 0;
        state_.history_status_filter_index = 0;
        state_.open_destination_after_operation = false;
        state_.confirm_before_deleting = true;
        state_.show_operation_summary = true;
        state_.solid_archive = true;
        state_.store_timestamps = true;
        state_.delete_after_compression = false;
        state_.verify_metadata_before_extract = true;
        state_.open_destination_after_extract = false;
        state_.gpu_required = true;
        state_.overwrite = false;
        state_.integrity_hash_opt_in = false;
        state_.defender_scan_opt_in = false;
        state_.verify_after_write_opt_in = false;
        state_.status = "Defaults restored";
    }
    request_repaint();
}

void MainWindow::start_compress() {
    std::vector<std::filesystem::path> sources;
    bool gpu_required = true;
    bool integrity = false;
    bool defender = false;
    bool verify_after_write = false;
    std::uint32_t block_size = superzip::kDefaultArchiveBlockBytes;
    std::filesystem::path output;
    {
        std::lock_guard lock(mutex_);
        sources = state_.queued_paths;
        gpu_required = state_.gpu_required;
        integrity = state_.integrity_hash_opt_in;
        defender = state_.defender_scan_opt_in;
        verify_after_write = state_.verify_after_write_opt_in;
        block_size = compression_block_size_bytes(state_.compression_block_size_index);
        output = compression_output_path_for(state_);
    }
    if (sources.empty()) {
        {
            std::lock_guard lock(mutex_);
            state_.status = "Add files or folders before starting compression";
        }
        request_repaint();
        return;
    }
    run_job([this, sources, output, gpu_required, integrity, defender, verify_after_write, block_size] {
        CompressOptions options;
        options.gpu_required = gpu_required;
        options.block_size = block_size;
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
    std::filesystem::path output;
    {
        std::lock_guard lock(mutex_);
        sources = state_.queued_paths;
        gpu_required = state_.gpu_required;
        overwrite = state_.overwrite;
        integrity = state_.integrity_hash_opt_in;
        defender = state_.defender_scan_opt_in;
        output = extraction_output_path_for(state_);
    }
    if (sources.empty()) {
        {
            std::lock_guard lock(mutex_);
            state_.status = "Select an archive before starting extraction";
        }
        request_repaint();
        return;
    }
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
    state_.gpu_runtime_name = info.runtime_name;
    state_.gpu_device_name = info.device_name;
    state_.gpu_arch = info.gcn_arch;
}

void MainWindow::initialize_performance_monitor() {
    if (gpu_query_ != nullptr) {
        return;
    }
    if (PdhOpenQueryW(nullptr, 0, &gpu_query_) != ERROR_SUCCESS) {
        gpu_query_ = nullptr;
        gpu_counter_ = nullptr;
        return;
    }
    const PDH_STATUS status = PdhAddEnglishCounterW(gpu_query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpu_counter_);
    if (status != ERROR_SUCCESS) {
        PdhCloseQuery(gpu_query_);
        gpu_query_ = nullptr;
        gpu_counter_ = nullptr;
        return;
    }
    PdhCollectQueryData(gpu_query_);
}

void MainWindow::shutdown_performance_monitor() {
    if (gpu_query_ != nullptr) {
        PdhCloseQuery(gpu_query_);
        gpu_query_ = nullptr;
        gpu_counter_ = nullptr;
    }
}

double MainWindow::sample_gpu_utilization() {
    if (gpu_query_ == nullptr || gpu_counter_ == nullptr) {
        return -1.0;
    }
    if (PdhCollectQueryData(gpu_query_) != ERROR_SUCCESS) {
        return -1.0;
    }

    DWORD buffer_size = 0;
    DWORD item_count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(gpu_counter_, PDH_FMT_DOUBLE, &buffer_size, &item_count, nullptr);
    if (status != PDH_MORE_DATA || buffer_size == 0 || item_count == 0) {
        return -1.0;
    }
    std::vector<std::byte> buffer(buffer_size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    status = PdhGetFormattedCounterArrayW(gpu_counter_, PDH_FMT_DOUBLE, &buffer_size, &item_count, items);
    if (status != ERROR_SUCCESS) {
        return -1.0;
    }

    const std::wstring pid_marker = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    double process_total = 0.0;
    double system_total = 0.0;
    bool found_process_sample = false;
    for (DWORD index = 0; index < item_count; ++index) {
        const auto& item = items[index];
        if (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA || item.szName == nullptr) {
            continue;
        }
        const double value = std::max(0.0, item.FmtValue.doubleValue);
        system_total += value;
        if (std::wstring_view(item.szName).find(pid_marker) != std::wstring_view::npos) {
            process_total += value;
            found_process_sample = true;
        }
    }
    const double value = found_process_sample ? process_total : system_total;
    return std::clamp(value, 0.0, 100.0);
}

void MainWindow::update_performance_sample() {
    const auto now = std::chrono::steady_clock::now();
    double elapsed_seconds = 0.0;
    if (last_performance_sample_time_ != std::chrono::steady_clock::time_point{}) {
        elapsed_seconds = std::chrono::duration<double>(now - last_performance_sample_time_).count();
    }

    double cpu_percent = 0.0;
    FILETIME creation_time{};
    FILETIME exit_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time)) {
        if (elapsed_seconds > 0.0) {
            const std::uint64_t previous_ticks = filetime_ticks(last_process_kernel_time_) + filetime_ticks(last_process_user_time_);
            const std::uint64_t current_ticks = filetime_ticks(kernel_time) + filetime_ticks(user_time);
            const auto logical_processors = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
            if (current_ticks >= previous_ticks) {
                cpu_percent = (static_cast<double>(current_ticks - previous_ticks) / (elapsed_seconds * 10000000.0 * static_cast<double>(logical_processors))) * 100.0;
                cpu_percent = std::clamp(cpu_percent, 0.0, 100.0);
            }
        }
        last_process_kernel_time_ = kernel_time;
        last_process_user_time_ = user_time;
    }

    double io_read_rate = 0.0;
    double io_write_rate = 0.0;
    IO_COUNTERS io_counters{};
    if (GetProcessIoCounters(GetCurrentProcess(), &io_counters)) {
        if (elapsed_seconds > 0.0) {
            if (io_counters.ReadTransferCount >= last_io_read_bytes_) {
                io_read_rate = static_cast<double>(io_counters.ReadTransferCount - last_io_read_bytes_) / elapsed_seconds;
            }
            if (io_counters.WriteTransferCount >= last_io_write_bytes_) {
                io_write_rate = static_cast<double>(io_counters.WriteTransferCount - last_io_write_bytes_) / elapsed_seconds;
            }
        }
        last_io_read_bytes_ = io_counters.ReadTransferCount;
        last_io_write_bytes_ = io_counters.WriteTransferCount;
    }

    std::uint64_t private_bytes = 0;
    PROCESS_MEMORY_COUNTERS_EX memory_counters{};
    memory_counters.cb = sizeof(memory_counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory_counters), sizeof(memory_counters))) {
        private_bytes = static_cast<std::uint64_t>(memory_counters.PrivateUsage);
    }

    double system_memory_percent = 0.0;
    MEMORYSTATUSEX memory_status{};
    memory_status.dwLength = sizeof(memory_status);
    if (GlobalMemoryStatusEx(&memory_status)) {
        system_memory_percent = static_cast<double>(memory_status.dwMemoryLoad);
    }

    if (last_gpu_memory_sample_time_ == std::chrono::steady_clock::time_point{} ||
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_gpu_memory_sample_time_).count() >= kGpuMemorySampleMs) {
        const auto info = query_gpu_info();
        cached_vram_total_bytes_ = info.vram_total_bytes;
        cached_vram_free_bytes_ = info.vram_free_bytes;
        last_gpu_memory_sample_time_ = now;
        std::lock_guard lock(mutex_);
        state_.gpu_status = info.status;
        state_.gpu_runtime_name = info.runtime_name;
        state_.gpu_device_name = info.device_name;
        state_.gpu_arch = info.gcn_arch;
    }

    const double gpu_percent = sample_gpu_utilization();
    PerformanceMonitorSample sample;
    sample.live = true;
    sample.gpu_utilization_available = gpu_percent >= 0.0;
    sample.cpu_percent = cpu_percent;
    sample.gpu_utilization_percent = sample.gpu_utilization_available ? gpu_percent : 0.0;
    sample.system_memory_percent = system_memory_percent;
    sample.io_read_bytes_per_second = std::max(0.0, io_read_rate);
    sample.io_write_bytes_per_second = std::max(0.0, io_write_rate);
    sample.private_bytes = private_bytes;
    sample.vram_total_bytes = cached_vram_total_bytes_;
    sample.vram_free_bytes = cached_vram_free_bytes_;
    {
        std::lock_guard lock(mutex_);
        state_.performance = sample;
    }
    last_performance_sample_time_ = now;
}

void MainWindow::start_page_transition(Page from, Page to) {
    transition_from_page_ = from;
    transition_to_page_ = to;
    page_transition_start_ = std::chrono::steady_clock::now();
    SetTimer(hwnd_, kAnimationTimer, 8, nullptr);
}

void MainWindow::start_toggle_animation(ToggleId id, bool from, bool to) {
    transition_toggle_ = id;
    transition_toggle_from_ = from;
    transition_toggle_to_ = to;
    toggle_transition_start_ = std::chrono::steady_clock::now();
    SetTimer(hwnd_, kAnimationTimer, 8, nullptr);
}

double MainWindow::page_transition_progress() const {
    if (page_transition_start_ == std::chrono::steady_clock::time_point{}) {
        return 1.0;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - page_transition_start_).count();
    return std::clamp(static_cast<double>(elapsed) / static_cast<double>(kPageTransitionMs), 0.0, 1.0);
}

double MainWindow::toggle_visual_position(ToggleId id, bool enabled) const {
    const double final_position = enabled ? 1.0 : 0.0;
    if (id == ToggleId::None || id != transition_toggle_ || toggle_transition_start_ == std::chrono::steady_clock::time_point{}) {
        return final_position;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - toggle_transition_start_).count();
    const double progress = std::clamp(static_cast<double>(elapsed) / static_cast<double>(kToggleTransitionMs), 0.0, 1.0);
    if (progress >= 1.0) {
        return final_position;
    }
    const double start = transition_toggle_from_ ? 1.0 : 0.0;
    const double end = transition_toggle_to_ ? 1.0 : 0.0;
    return start + ((end - start) * ease_out(progress));
}

void MainWindow::tick_animation() {
    const bool page_active = page_transition_progress() < 1.0;
    const bool toggle_active =
        transition_toggle_ != ToggleId::None &&
        toggle_transition_start_ != std::chrono::steady_clock::time_point{} &&
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - toggle_transition_start_).count() < kToggleTransitionMs;
    if (!page_active) {
        page_transition_start_ = {};
    }
    if (!toggle_active) {
        transition_toggle_ = ToggleId::None;
        toggle_transition_start_ = {};
    }
    if (page_active || toggle_active) {
        request_repaint();
        return;
    }
    KillTimer(hwnd_, kAnimationTimer);
    request_repaint();
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
