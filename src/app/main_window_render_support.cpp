#include "app/main_window_support.hpp"

#include "core/archive_format.hpp"
#include "superzip_brand_logo.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <psapi.h>
#include <sstream>
#include <string>
#include <vector>

namespace superzip::app {

// Purpose: Blend two Win32 colors linearly.
// Inputs: `from` and `to` are colors and `t` is clamped interpolation progress.
// Outputs: Returns the blended color.
COLORREF blend_color(COLORREF from, COLORREF to, double t) {
    const double clamped = std::clamp(t, 0.0, 1.0);
    const auto blend = [clamped](int a, int b) {
        return static_cast<int>(std::round(static_cast<double>(a) + (static_cast<double>(b - a) * clamped)));
    };
    return RGB(blend(GetRValue(from), GetRValue(to)), blend(GetGValue(from), GetGValue(to)),
               blend(GetBValue(from), GetBValue(to)));
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

// Purpose: Sample the current process private memory counter.
// Inputs: None; reads the current process through PSAPI.
// Outputs: Returns private bytes, or zero when Windows does not provide the counter.
std::uint64_t sample_private_memory_bytes() {
    PROCESS_MEMORY_COUNTERS_EX memory_counters{};
    memory_counters.cb = sizeof(memory_counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory_counters),
                              sizeof(memory_counters))) {
        return 0;
    }
    return static_cast<std::uint64_t>(memory_counters.PrivateUsage);
}

// Purpose: Sample physical system memory usage for the live monitor.
// Inputs: None; reads `GlobalMemoryStatusEx`.
// Outputs: Returns percent, total bytes, and used bytes, or zeros when unavailable.
SystemMemoryUsage sample_system_memory_usage() {
    MEMORYSTATUSEX memory_status{};
    memory_status.dwLength = sizeof(memory_status);
    if (!GlobalMemoryStatusEx(&memory_status)) {
        return {};
    }
    const auto total = static_cast<std::uint64_t>(memory_status.ullTotalPhys);
    const auto available = static_cast<std::uint64_t>(memory_status.ullAvailPhys);
    return SystemMemoryUsage{
        .percent = static_cast<double>(memory_status.dwMemoryLoad),
        .total_bytes = total,
        .used_bytes = available <= total ? total - available : 0U,
    };
}

// Purpose: Format a percentage for compact monitor cards.
// Inputs: `value` is a percent value already clamped by the caller when needed.
// Outputs: Returns a one-decimal UTF-16 percentage string.
std::wstring percentage_text(double value) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(1) << value << L"%";
    return out.str();
}

// Purpose: Compute a bounded progress ratio from byte or entry counters.
// Inputs: `snapshot` is a worker progress sample with optional totals.
// Outputs: Returns 0.0-1.0; returns 0.0 when no total is available.
double progress_ratio(const ProgressSnapshot& snapshot) {
    if (snapshot.total_bytes > 0U) {
        return std::clamp(static_cast<double>(snapshot.processed_bytes) / static_cast<double>(snapshot.total_bytes),
                          0.0, 1.0);
    }
    if (snapshot.total_entries > 0U) {
        return std::clamp(static_cast<double>(snapshot.completed_entries) / static_cast<double>(snapshot.total_entries),
                          0.0, 1.0);
    }
    return 0.0;
}

// Purpose: Decide whether the copied progress sample should still be drawn.
// Inputs: `state` is the immutable UI snapshot for one frame.
// Outputs: Returns true for active progress and for completed progress still inside its hold window.
bool progress_visible(const UiState& state) {
    if (state.progress.operation == OperationKind::Idle) {
        return false;
    }
    if (state.progress_visible_until == std::chrono::steady_clock::time_point{}) {
        return true;
    }
    return std::chrono::steady_clock::now() <= state.progress_visible_until;
}

// Purpose: Decide whether a tab-local progress bar should be visible.
// Inputs: `state` is the immutable UI snapshot for one frame.
// Outputs: Returns true only while a worker is actively publishing progress, not during the retained status hold.
bool progress_bar_active(const UiState& state) {
    return state.progress.operation != OperationKind::Idle &&
           state.progress_visible_until == std::chrono::steady_clock::time_point{};
}

// Purpose: Format operation progress as a stable one-decimal percentage.
// Inputs: `snapshot` is a worker progress sample.
// Outputs: Returns text such as `42.7%`.
std::wstring progress_percent_text(const ProgressSnapshot& snapshot) {
    return percentage_text(progress_ratio(snapshot) * 100.0);
}

// Purpose: Format a throughput value for compact monitor cards.
// Inputs: `bytes_per_second` is a nonnegative byte rate.
// Outputs: Returns a UTF-16 string with `/s` suffix.
std::wstring rate_text(double bytes_per_second) {
    return widen(human_bytes(bytes_per_second) + "/s");
}

// Purpose: Format a remaining-duration estimate for the compact status strip.
// Inputs: `seconds` is a nonnegative floating-point estimate.
// Outputs: Returns a compact value using sec, min, h, and d units, or `--` when invalid.
std::wstring duration_remaining_text(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return L"--";
    }
    auto total = static_cast<unsigned long long>(std::ceil(seconds));
    if (total < 60ULL) {
        return std::to_wstring(total) + L" sec";
    }
    if (total < 3600ULL) {
        const auto minutes = total / 60ULL;
        const auto remainder = total % 60ULL;
        return remainder == 0ULL ? std::to_wstring(minutes) + L" min"
                                 : std::to_wstring(minutes) + L" min " + std::to_wstring(remainder) + L" sec";
    }
    if (total < 86400ULL) {
        const auto hours = total / 3600ULL;
        const auto minutes = (total % 3600ULL) / 60ULL;
        return minutes == 0ULL ? std::to_wstring(hours) + L" h"
                               : std::to_wstring(hours) + L" h " + std::to_wstring(minutes) + L" min";
    }
    const auto days = total / 86400ULL;
    const auto hours = (total % 86400ULL) / 3600ULL;
    return hours == 0ULL ? std::to_wstring(days) + L" d"
                         : std::to_wstring(days) + L" d " + std::to_wstring(hours) + L" h";
}

// Purpose: Format the smoothed remaining operation time.
// Inputs: `state` contains the latest progress sample and exponentially smoothed ETA.
// Outputs: Returns formatted remaining time or `--` when the estimate is unavailable.
std::wstring progress_time_remaining_text(const UiState& state) {
    if (progress_ratio(state.progress) >= 1.0) {
        return L"0 sec";
    }
    if (state.smoothed_time_remaining_seconds < 0.0) {
        return L"--";
    }
    return duration_remaining_text(state.smoothed_time_remaining_seconds);
}

// Purpose: Format one local wall-clock time for visible UI tables and status text.
// Inputs: `time_point` is a system-clock value to present in the user's local timezone.
// Outputs: Returns fixed 12-hour text as `H:MM:SS AM/PM` without a leading hour zero.
std::wstring local_time_text(std::chrono::system_clock::time_point time_point) {
    constexpr ULONGLONG kUnixEpochFiletimeTicks = 116444736000000000ULL;
    const auto ticks_since_unix_epoch =
        std::chrono::duration_cast<std::chrono::duration<long long, std::ratio<1, 10000000>>>(
            time_point.time_since_epoch())
            .count();
    ULARGE_INTEGER utc_ticks{};
    utc_ticks.QuadPart = static_cast<ULONGLONG>(ticks_since_unix_epoch) + kUnixEpochFiletimeTicks;
    FILETIME utc_file_time{utc_ticks.LowPart, utc_ticks.HighPart};
    FILETIME local_file_time{};
    SYSTEMTIME local_time{};
    if (FileTimeToLocalFileTime(&utc_file_time, &local_file_time) == FALSE ||
        FileTimeToSystemTime(&local_file_time, &local_time) == FALSE) {
        return L"-";
    }
    const int hour24 = std::clamp(static_cast<int>(local_time.wHour), 0, 23);
    const bool is_pm = hour24 >= 12;
    const int hour12_raw = hour24 % 12;
    const int hour12 = hour12_raw == 0 ? 12 : hour12_raw;
    std::wostringstream out;
    out << hour12 << L":" << std::setfill(L'0') << std::setw(2)
        << std::clamp(static_cast<int>(local_time.wMinute), 0, 59) << L":" << std::setw(2)
        << std::clamp(static_cast<int>(local_time.wSecond), 0, 59) << (is_pm ? L" PM" : L" AM");
    return out.str();
}

// Purpose: Format the current local time for the status strip.
// Inputs: None; reads the local system clock.
// Outputs: Returns fixed 12-hour text as `H:MM:SS AM/PM` without a leading hour zero.
std::wstring current_user_time_text() {
    return local_time_text(std::chrono::system_clock::now());
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
// Outputs: Returns `Folder`, `Archive`, `File`, or `Missing`; archive detection is extension-only and non-blocking.
std::wstring entry_type_text(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return L"Folder";
    }
    if (std::filesystem::is_regular_file(path, ec)) {
        const auto extension_format = detect_archive_format_by_extension(path);
        if (archive_format_info(extension_format).can_extract) {
            return L"Archive";
        }
        return L"File";
    }
    return L"Missing";
}

// Purpose: Return the detected archive format label for the extraction page.
// Inputs: `paths` is the current queue; only the first path is the extraction archive.
// Outputs: Returns a display label without throwing for empty queues or unreadable probes.
std::wstring detected_archive_format_text(const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) {
        return L"-";
    }
    const auto format = detect_archive_format(paths.front());
    if (format == ArchiveFormat::Unknown) {
        return L"-";
    }
    return widen(archive_format_extension_info_for_path(format, paths.front()).display_name);
}

// Purpose: Collect selected queue entries that can be extracted as archives by extension.
// Inputs: `state` is a stable UI snapshot; only regular-file queue rows with selected ticks are considered.
// Outputs: Returns selected archive paths in queue order without reading file content.
std::vector<std::filesystem::path> selected_extract_archive_paths(const UiState& state) {
    std::vector<std::filesystem::path> archives;
    archives.reserve(state.queued_paths.size());
    for (std::size_t index = 0; index < state.queued_paths.size(); ++index) {
        const bool selected = index >= state.queued_enabled.size() || state.queued_enabled[index];
        if (!selected) {
            continue;
        }
        std::error_code ec;
        const auto& path = state.queued_paths[index];
        if (!std::filesystem::is_regular_file(path, ec)) {
            continue;
        }
        const auto extension_format = detect_archive_format_by_extension(path);
        if (archive_format_info(extension_format).can_extract) {
            archives.push_back(path);
        }
    }
    return archives;
}

// Purpose: Format the Extract page archive-path field for zero, one, or multiple selected archives.
// Inputs: `archives` is the selected archive list returned by `selected_extract_archive_paths`.
// Outputs: Returns muted placeholder text, the single path, or the fixed multiple-selection text.
std::wstring selected_extract_archive_text(const std::vector<std::filesystem::path>& archives) {
    if (archives.empty()) {
        return L"Select one or more archives from the queue";
    }
    if (archives.size() == 1U) {
        return archives.front().wstring();
    }
    return L"Multiple selected archives";
}

// Purpose: Format the Extract page format field from selected archive extensions.
// Inputs: `archives` is the selected archive list; every path must be classified cheaply by extension.
// Outputs: Returns one extension display name when all selected archives share a format, otherwise `-`.
std::wstring selected_extract_archive_format_text(const std::vector<std::filesystem::path>& archives) {
    if (archives.empty()) {
        return L"-";
    }
    const auto first_format = detect_archive_format_by_extension(archives.front());
    if (!archive_format_info(first_format).can_extract) {
        return L"-";
    }
    for (const auto& archive : archives) {
        if (detect_archive_format_by_extension(archive) != first_format) {
            return L"-";
        }
    }
    return widen(archive_format_extension_info_for_path(first_format, archives.front()).display_name);
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

// Purpose: Draw a subtle Task Manager-style graph grid.
// Inputs: `dc` is the target and `rect` is the graph plotting rectangle.
// Outputs: Renders fixed horizontal and vertical guide lines.
void draw_graph_grid(HDC dc, const RECT& rect) {
    fill_rect(dc, rect, RGB(17, 27, 31));
    for (int i = 1; i < 4; ++i) {
        const int y = rect.top + ((rect.bottom - rect.top) * i) / 4;
        draw_line(dc, rect.left, y, rect.right, y, RGB(34, 50, 56));
    }
    for (int i = 1; i < 6; ++i) {
        const int x = rect.left + ((rect.right - rect.left) * i) / 6;
        draw_line(dc, x, rect.top, x, rect.bottom, RGB(28, 42, 48));
    }
    stroke_rect(dc, rect, RGB(46, 67, 74));
}

// Purpose: Draw one normalized history series inside a graph rectangle.
// Inputs: `dc` is the target, `rect` is the plot area, `values` are clamped 0-1 samples, and `color` is the line.
// Outputs: Renders a filled Task Manager-style trend area and crisp polyline; empty input produces no line.
void draw_graph_series(HDC dc, const RECT& rect, std::span<const double> values, COLORREF color) {
    if (values.empty()) {
        return;
    }
    const int width = std::max(1, static_cast<int>(rect.right - rect.left - 1));
    const int height = std::max(1, static_cast<int>(rect.bottom - rect.top - 1));
    const int left = static_cast<int>(rect.left);
    const int right = static_cast<int>(rect.right);
    constexpr std::size_t kFullGraphSampleCapacity = 96U;
    const std::size_t capacity = std::max<std::size_t>(values.size(), kFullGraphSampleCapacity);
    const double sample_step =
        capacity <= 1U ? static_cast<double>(width) : static_cast<double>(width) / static_cast<double>(capacity - 1U);
    const double first_x =
        static_cast<double>(rect.right - 1) - (static_cast<double>(values.size() - 1U) * sample_step);
    std::vector<POINT> points;
    points.reserve(values.size() + 2U);
    points.push_back(POINT{std::clamp(static_cast<int>(std::round(first_x)), left, right), rect.bottom});
    for (std::size_t index = 0; index < values.size(); ++index) {
        const double y_ratio = 1.0 - std::clamp(values[index], 0.0, 1.0);
        const int x =
            std::clamp(static_cast<int>(std::round(first_x + (static_cast<double>(index) * sample_step))), left, right);
        const int y = rect.top + static_cast<int>(std::round(y_ratio * static_cast<double>(height)));
        points.push_back(POINT{x, y});
    }
    points.push_back(POINT{points[points.size() - 1U].x, rect.bottom});

    const COLORREF fill_color = blend_color(color, RGB(17, 27, 31), 0.72);
    HBRUSH brush = CreateSolidBrush(fill_color);
    HGDIOBJ previous_brush = SelectObject(dc, brush);
    HGDIOBJ previous_pen_for_fill = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, points.data(), static_cast<int>(points.size()));
    SelectObject(dc, previous_pen_for_fill);
    SelectObject(dc, previous_brush);
    DeleteObject(brush);

    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ previous_pen = SelectObject(dc, pen);
    if (values.size() == 1U) {
        MoveToEx(dc, points[1].x, points[1].y, nullptr);
        LineTo(dc, std::min(points[1].x + 1, rect.right), points[1].y);
    } else {
        for (std::size_t index = 1; index + 1U < points.size(); ++index) {
            if (index == 1U) {
                MoveToEx(dc, points[index].x, points[index].y, nullptr);
            } else {
                LineTo(dc, points[index].x, points[index].y);
            }
        }
    }
    SelectObject(dc, previous_pen);
    DeleteObject(pen);
}

// Purpose: Draw UTF-16 text with the currently selected font.
// Inputs: `dc` is the target, `rect` is the layout box, `text` is display text, `color` is text color, and `format` is
// DrawTextW flags. Outputs: Writes text into `dc` without mutating the caller's rectangle.
void draw_text(HDC dc, const RECT& rect, std::wstring_view text, COLORREF color, UINT format) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT copy = rect;
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &copy, format);
}

// Purpose: Draw one graph-axis text label as a top-layer overlay.
// Inputs: `dc` is the target, `rect` is the label box, `text` is preformatted, `color` is the text color, and
// `format` controls alignment.
// Outputs: Draws plain clipped text after graph lines without adding a label box or changing graph geometry.
void draw_graph_axis_label(HDC dc, const RECT& rect, const std::wstring& text, COLORREF color, UINT format) {
    if (text.empty() || rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    const UINT flags = format | DT_SINGLELINE | DT_NOPREFIX;
    for (POINT offset : std::array<POINT, 4>{{POINT{-1, 0}, POINT{1, 0}, POINT{0, -1}, POINT{0, 1}}}) {
        RECT shadow = rect;
        OffsetRect(&shadow, offset.x, offset.y);
        draw_text(dc, shadow, text, RGB(17, 27, 31), flags);
    }
    draw_text(dc, rect, text, color, flags);
}

// Purpose: Draw compact scale labels inside a live graph.
// Inputs: `dc` is the target, `rect` is the plot area, and labels are already formatted for display.
// Outputs: Renders unobtrusive axis text above grid and graph lines without changing the graph geometry.
void draw_graph_axis_labels(HDC dc, const RECT& rect, const std::wstring& top_label, const std::wstring& bottom_label) {
    const COLORREF axis_text = RGB(103, 127, 135);
    SIZE top_size{};
    SIZE bottom_size{};
    if (!top_label.empty()) {
        GetTextExtentPoint32W(dc, top_label.data(), static_cast<int>(top_label.size()), &top_size);
    }
    if (!bottom_label.empty()) {
        GetTextExtentPoint32W(dc, bottom_label.data(), static_cast<int>(bottom_label.size()), &bottom_size);
    }
    const int right_padding = 12;
    const int requested_width = std::max<int>(static_cast<int>(top_size.cx), static_cast<int>(bottom_size.cx)) + 18;
    const int available_width = std::max<int>(48, rect.right - rect.left - right_padding - 2);
    const int label_width = std::min(std::max(requested_width, 48), available_width);
    const int label_height =
        std::max<int>(18, std::max<int>(static_cast<int>(top_size.cy), static_cast<int>(bottom_size.cy)) + 6);
    draw_graph_axis_label(dc,
                          RECT{rect.right - label_width - right_padding, rect.top + 5, rect.right - right_padding,
                               rect.top + 5 + label_height},
                          top_label, axis_text, DT_RIGHT | DT_TOP);
    draw_graph_axis_label(dc,
                          RECT{rect.right - label_width - right_padding, rect.bottom - label_height - 5,
                               rect.right - right_padding, rect.bottom - 5},
                          bottom_label, axis_text, DT_RIGHT | DT_TOP);
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

// Purpose: Append one valid keyboard-focus target.
// Inputs: `targets` receives the target, `kind` identifies behavior, `rect` is hit geometry, and `index` is optional.
// Outputs: Adds a target only when its rectangle has positive size.
void append_focus_target(std::vector<FocusTarget>& targets, FocusTargetKind kind, RECT rect, int index) {
    if (rect.right > rect.left && rect.bottom > rect.top) {
        targets.push_back(FocusTarget{kind, rect, index});
    }
}

// Purpose: Draw the SuperZip stacked archive mark.
// Inputs: `dc` is the target, `rect` is the logo bounds, and `color` is the stroke color.
// Outputs: Draws the canonical vector mark generated from `resources/brand/superzip-logo.svg`.
void draw_logo(HDC dc, const RECT& rect, COLORREF color) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    const float width = static_cast<float>(rect.right - rect.left);
    const float height = static_cast<float>(rect.bottom - rect.top);
    const float scale_x = width / brand::kSuperZipLogoMarkWidth;
    const float scale_y = height / brand::kSuperZipLogoMarkHeight;
    const float scale_factor = std::max(0.01f, std::min(scale_x, scale_y));
    const float rendered_width = brand::kSuperZipLogoMarkWidth * scale_factor;
    const float rendered_height = brand::kSuperZipLogoMarkHeight * scale_factor;
    const float origin_x = static_cast<float>(rect.left) + (width - rendered_width) / 2.0f;
    const float origin_y = static_cast<float>(rect.top) + (height - rendered_height) / 2.0f;
    auto map_point = [&](brand::LogoPoint point) {
        return Gdiplus::PointF{origin_x + point.x * scale_factor, origin_y + point.y * scale_factor};
    };

    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)),
                     std::max(1.35f, brand::kSuperZipLogoMarkStrokeWidth * scale_factor));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    for (const auto& layer : brand::kSuperZipLogoMarkLayers) {
        std::array<Gdiplus::PointF, 4> points{};
        std::ranges::transform(layer.points, points.begin(), map_point);
        graphics.DrawPolygon(&pen, points.data(), static_cast<INT>(points.size()));
    }
}

// Purpose: Convert a Win32 COLORREF into a GDI+ color.
// Inputs: `color` is a Win32 RGB value and `alpha` is the target opacity.
// Outputs: Returns an ARGB GDI+ color with channel order corrected.
Gdiplus::Color gp_color(COLORREF color, BYTE alpha) {
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

// Purpose: Draw the settings gear navigation glyph.
// Inputs: `graphics` is the anti-aliased target, `origin_x`/`origin_y`/`size` define normalized icon space, and drawing
// objects provide styling. Outputs: Writes the gear glyph without changing product state.
void draw_settings_nav_icon(Gdiplus::Graphics& graphics, float origin_x, float origin_y, float size, Gdiplus::Pen& pen,
                            Gdiplus::SolidBrush& soft_fill) {
    auto px = [origin_x, size](float value) { return origin_x + (value * size / 30.0f); };
    auto py = [origin_y, size](float value) { return origin_y + (value * size / 30.0f); };
    auto rectf = [&](float left, float top, float right, float bottom) {
        return Gdiplus::RectF(px(left), py(top), px(right) - px(left), py(bottom) - py(top));
    };

    std::array<Gdiplus::PointF, 16> teeth{};
    for (int i = 0; i < static_cast<int>(teeth.size()); ++i) {
        const double angle = (-3.14159265358979323846 / 2.0) + (3.14159265358979323846 * 2.0 * static_cast<double>(i) /
                                                                static_cast<double>(teeth.size()));
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
    case Page::Compress: {
        Gdiplus::GraphicsPath tray;
        add_rounded_rect_path(tray, px(6.2f), py(16.2f), px(23.8f) - px(6.2f), py(24.2f) - py(16.2f), size / 12.0f);
        graphics.FillPath(&soft_fill, &tray);
        graphics.DrawPath(&pen, &tray);
        graphics.DrawLine(&strong_pen, px(15.0f), py(5.2f), px(15.0f), py(15.0f));
        Gdiplus::PointF arrow[] = {{px(10.4f), py(11.2f)}, {px(15.0f), py(15.8f)}, {px(19.6f), py(11.2f)}};
        graphics.DrawLines(&strong_pen, arrow, 3);
        graphics.DrawLine(&fine_pen, px(10.0f), py(20.0f), px(20.0f), py(20.0f));
    } break;
    case Page::Extract: {
        Gdiplus::GraphicsPath tray;
        add_rounded_rect_path(tray, px(6.2f), py(16.2f), px(23.8f) - px(6.2f), py(24.2f) - py(16.2f), size / 12.0f);
        graphics.FillPath(&soft_fill, &tray);
        graphics.DrawPath(&pen, &tray);
        graphics.DrawLine(&strong_pen, px(15.0f), py(15.8f), px(15.0f), py(5.2f));
        Gdiplus::PointF arrow[] = {{px(10.4f), py(9.8f)}, {px(15.0f), py(5.2f)}, {px(19.6f), py(9.8f)}};
        graphics.DrawLines(&strong_pen, arrow, 3);
        graphics.DrawLine(&fine_pen, px(10.0f), py(20.0f), px(20.0f), py(20.0f));
    } break;
    case Page::Security: {
        Gdiplus::PointF shield[] = {
            {px(15.0f), py(4.3f)},  {px(23.3f), py(7.3f)}, {px(22.2f), py(17.3f)},
            {px(15.0f), py(25.2f)}, {px(7.8f), py(17.3f)}, {px(6.7f), py(7.3f)},
        };
        Gdiplus::GraphicsPath path;
        path.AddPolygon(shield, 6);
        path.CloseFigure();
        graphics.FillPath(&soft_fill, &path);
        graphics.DrawPath(&pen, &path);
        Gdiplus::PointF check[] = {{px(10.7f), py(15.2f)}, {px(14.0f), py(18.5f)}, {px(20.2f), py(11.2f)}};
        graphics.DrawLines(&strong_pen, check, 3);
    } break;
    case Page::History: {
        graphics.DrawEllipse(&pen, rectf(5.0f, 5.0f, 25.0f, 25.0f));
        graphics.DrawLine(&fine_pen, px(15.0f), py(8.0f), px(15.0f), py(10.6f));
        graphics.DrawLine(&fine_pen, px(15.0f), py(19.4f), px(15.0f), py(22.0f));
        graphics.DrawLine(&fine_pen, px(8.0f), py(15.0f), px(10.6f), py(15.0f));
        graphics.DrawLine(&fine_pen, px(19.4f), py(15.0f), px(22.0f), py(15.0f));
        graphics.DrawLine(&pen, px(15.0f), py(15.0f), px(15.0f), py(10.0f));
        graphics.DrawLine(&pen, px(15.0f), py(15.0f), px(19.2f), py(17.8f));
        graphics.FillEllipse(&firm_fill, rectf(13.7f, 13.7f, 16.3f, 16.3f));
    } break;
    case Page::Gpu: {
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
    } break;
    case Page::Settings:
        draw_settings_nav_icon(graphics, origin_x, origin_y, size, pen, soft_fill);
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

}  // namespace superzip::app
