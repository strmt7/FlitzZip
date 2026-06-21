#pragma once

#include "app/main_window_state.hpp"
#include "core/archive.hpp"
#include "core/archive_format.hpp"
#include "core/defender_scan.hpp"
#include "core/integrity.hpp"
#include "core/progress.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>
#include <shellapi.h>
#include <ole2.h>
#include <objidl.h>
#include <gdiplus.h>

namespace superzip::app {

constexpr int kTopBar = 52;
constexpr int kRailWidth = 86;
constexpr int kStatusBar = 34;
constexpr int kDesignClientWidth = 1200;
constexpr int kDesignClientHeight = 760;
constexpr int kPageInsetX = 30;
constexpr int kPageInsetY = 22;
constexpr int kPageHeaderHeight = 34;
constexpr int kPageTitleTextHeight = 46;
constexpr int kQueueCheckboxColumnWidth = 44;
constexpr int kQueueResizeGripHalfWidth = 4;
constexpr int kQueueHeaderHeight = 36;
constexpr int kQueueRowHeight = 34;
constexpr int kQueueScrollbarWidth = 12;
constexpr int kOperationProgressHeight = 5;
constexpr int kPerformanceUpdateFieldWidth = 78;
constexpr int kClockSegmentWidth = 106;
constexpr UINT_PTR kAnimationTimer = 7;
constexpr UINT_PTR kSmokeAutoCloseTimer = 9;
constexpr UINT_PTR kSmokeClosePollTimer = 10;
constexpr UINT_PTR kProgressHoldTimer = 11;
constexpr UINT_PTR kClockTimer = 12;
constexpr UINT_PTR kTextTooltipTimer = 13;
constexpr UINT_PTR kDeferredCommandTimer = 14;
constexpr UINT kProgressHoldMs = 15000;
constexpr UINT kClockPollMs = 50;
constexpr UINT kTextTooltipDelayMs = 500;
constexpr int kPageTransitionMs = 120;
constexpr int kToggleTransitionMs = 105;
constexpr int kButtonReleaseTransitionMs = 130;
constexpr int kSmoothScrollTransitionMs = 180;
constexpr UINT kDragQueryMessage = 0x0049;
constexpr std::size_t kMaxLogEntries = 128;
constexpr std::wstring_view kProductTagline = L"ULTRAFAST GPU-ACCELERATED ARCHIVAL SOFTWARE";

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
constexpr UINT kGpuMemorySampleMs = 1000;

constexpr std::array<std::string_view, 19> kCompressionCreateFormatExtensions{
    ".suzip",   ".zip",  ".tar.gz", ".tgz", ".tar",  ".gz",      ".zst",  ".zstd", ".tar.zst", ".tzst",
    ".tar.bz2", ".tbz2", ".tbz",    ".bz2", ".cpio", ".cpio.gz", ".cpgz", ".ar",   ".Z",
};
constexpr int kDefaultCompressionFormatIndex = 0;
constexpr int kCompressionFormatMaxIndex = static_cast<int>(kCompressionCreateFormatExtensions.size()) - 1;
constexpr std::array<int, 4> kPerformanceUpdateSecondsOptions{1, 3, 5, 10};
constexpr std::array<std::uint32_t, 7> kCompressionBlockSizeOptions{
    256U * 1024U,       512U * 1024U,       superzip::kDefaultArchiveBlockBytes, 2U * 1024U * 1024U,
    4U * 1024U * 1024U, 8U * 1024U * 1024U, superzip::kMaxArchiveBlockBytes,
};
constexpr int kCompressionBlockSizeMaxIndex = static_cast<int>(kCompressionBlockSizeOptions.size()) - 1;

struct SystemMemoryUsage {
    double percent = 0.0;
    std::uint64_t total_bytes = 0;
    std::uint64_t used_bytes = 0;
};

// Purpose: Return the fixed release window style.
// Inputs: None.
// Outputs: Returns a non-resizable Win32 overlapped-window style.
DWORD window_style();

// Purpose: Read the optional GUI-smoke auto-close timeout.
// Inputs: Uses SUPERZIP_GUI_SMOKE_AUTO_CLOSE_MS from the process environment.
// Outputs: Returns a bounded timer interval in milliseconds, or zero when disabled.
UINT smoke_auto_close_ms();

// Purpose: Read the optional GUI-smoke close-marker file path.
// Inputs: Uses SUPERZIP_GUI_SMOKE_CLOSE_FILE from the process environment.
// Outputs: Returns the marker path, or an empty path when smoke close polling is disabled.
std::filesystem::path smoke_close_marker_path();

// Purpose: Check whether the GUI smoke harness requested in-process shutdown.
// Inputs: Uses the smoke close-marker path from the process environment.
// Outputs: Returns true only when the configured marker file exists.
bool smoke_close_requested();

// Purpose: Resolve the current user's Downloads known folder for archive output defaults.
// Inputs: None; asks Windows for the interactive user's Downloads folder.
// Outputs: Returns a Downloads-like known folder path; never falls back to the process current directory.
std::filesystem::path current_user_downloads_directory();

// Purpose: Convert a shell HDROP payload into filesystem paths.
// Inputs: drop is a valid HDROP handle owned by the caller.
// Outputs: Returns every nonempty path advertised by the shell payload.
std::vector<std::filesystem::path> paths_from_hdrop(HDROP drop);

// Purpose: Create a concise history message for an optional SHA-256 integrity check.
// Inputs: `prefix` names the hashed target and `result` is the completed integrity result.
// Outputs: Returns a user-visible status string with digest and directory-tree counters when applicable.
std::string integrity_history_status(std::string_view prefix, const IntegrityResult& result);

// Purpose: Create a concise history message for an optional Defender scan.
// Inputs: prefix names the scanned item and scan is the Defender result.
// Outputs: Returns a user-visible status string that distinguishes scan states.
std::string defender_history_status(std::string_view prefix, const DefenderScanResult& scan);

// Purpose: Map a visible busy status into the operation category used by the History tab.
// Inputs: label is the status text passed to the background worker.
// Outputs: Returns the stable History operation filter name for success and failure rows.
std::string operation_for_job_label(std::string_view label);

std::filesystem::path destination_directory_or_default(const UiState& state);
ArchiveFormat compression_format_value(int index);
std::wstring compression_format_text(int index);
std::wstring compression_format_extension(int index);
bool compression_format_uses_suzip_tuning(ArchiveFormat format);
bool compression_format_uses_level(ArchiveFormat format);
std::wstring compression_output_filename_for(const UiState& state);
std::filesystem::path compression_output_path_for(const UiState& state);
std::filesystem::path extraction_output_path_for(const UiState& state);
OperationStats extract_detected_archive(ArchiveFormat archive_format, const std::filesystem::path& archive,
                                        const std::filesystem::path& output, bool gpu_required, bool overwrite,
                                        const ProgressCallback& progress_callback);
std::wstring compression_level_text(int index);
int normalize_performance_update_seconds(int seconds);
int performance_update_index_for_seconds(int seconds);
std::wstring performance_update_speed_text(int seconds);
std::wstring performance_update_option_text(int index);
std::vector<std::wstring> fixed_io_drive_options();
int normalize_io_drive_index(int index);
std::wstring io_drive_option_text(int index);
int compression_level_value(int index);
OperationStats compress_gui_archive(const std::vector<std::filesystem::path>& sources,
                                    const std::filesystem::path& output, ArchiveFormat archive_format,
                                    bool gpu_required, bool verify_after_write, std::uint32_t block_size,
                                    int compression_level, const ProgressCallback& progress_callback);
std::wstring compression_block_size_text(int index);
std::uint32_t compression_block_size_bytes(int index);
std::wstring memory_policy_text(int index);
std::wstring log_level_text(int index);
std::wstring log_retention_text(int index);
void apply_settings_to_state(const AppSettings& settings, UiState& state);
AppSettings settings_from_state(const UiState& state);
bool settings_equal(const AppSettings& left, const AppSettings& right);
AppSettings parse_settings_json(std::string_view json);
std::string settings_to_json(const AppSettings& settings);
std::filesystem::path app_storage_directory();
std::filesystem::path settings_file_path();
std::filesystem::path log_file_path();
void ensure_app_storage();
AppSettings read_settings_file(const std::filesystem::path& path);
void write_settings_file(const std::filesystem::path& path, const AppSettings& settings);
void append_log_file_entry(const std::filesystem::path& path, LogSeverity severity, const std::string& message,
                           std::chrono::system_clock::time_point timestamp);
std::wstring history_operation_filter_text(int index);
std::wstring history_status_filter_text(int index);
std::vector<std::wstring> dropdown_options(DropdownId id);
int dropdown_selected_index(const UiState& state, DropdownId id);
COLORREF blend_color(COLORREF from, COLORREF to, double t);
double ease_out(double t);
std::wstring widen(const std::string& value);
std::string human_bytes(double value);
std::uint64_t filetime_ticks(const FILETIME& value);
std::uint64_t sample_private_memory_bytes();
SystemMemoryUsage sample_system_memory_usage();
std::wstring percentage_text(double value);
double progress_ratio(const ProgressSnapshot& snapshot);
bool progress_visible(const UiState& state);
bool progress_bar_active(const UiState& state);
std::wstring progress_percent_text(const ProgressSnapshot& snapshot);
std::wstring rate_text(double bytes_per_second);
std::wstring duration_remaining_text(double seconds);
std::wstring progress_time_remaining_text(const UiState& state);
std::wstring local_time_text(std::chrono::system_clock::time_point time_point);
std::wstring current_user_time_text();
std::wstring entry_size_text(const std::filesystem::path& path);
std::wstring entry_type_text(const std::filesystem::path& path);
std::wstring detected_archive_format_text(const std::vector<std::filesystem::path>& paths);
std::vector<std::filesystem::path> selected_extract_archive_paths(const UiState& state);
std::wstring selected_extract_archive_text(const std::vector<std::filesystem::path>& archives);
std::wstring selected_extract_archive_format_text(const std::vector<std::filesystem::path>& archives);
void fill_rect(HDC dc, const RECT& rect, COLORREF color);
void fill_round_rect(HDC dc, const RECT& rect, COLORREF color, int radius);
void stroke_rect(HDC dc, const RECT& rect, COLORREF color);
void draw_line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color);
void draw_graph_grid(HDC dc, const RECT& rect);
void draw_graph_series(HDC dc, const RECT& rect, std::span<const double> values, COLORREF color);
void draw_text(HDC dc, const RECT& rect, std::wstring_view text, COLORREF color, UINT format);
void draw_graph_axis_label(HDC dc, const RECT& rect, const std::wstring& text, COLORREF color, UINT format);
void draw_graph_axis_labels(HDC dc, const RECT& rect, const std::wstring& top_label, const std::wstring& bottom_label);
RECT inset_rect(RECT rect, int dx, int dy);
bool contains_point(const RECT& rect, int x, int y);
void append_focus_target(std::vector<FocusTarget>& targets, FocusTargetKind kind, RECT rect, int index = 0);
void draw_logo(HDC dc, const RECT& rect, COLORREF color);
Gdiplus::Color gp_color(COLORREF color, BYTE alpha = 255);
void configure_icon_pen(Gdiplus::Pen& pen);
void add_rounded_rect_path(Gdiplus::GraphicsPath& path, float x, float y, float width, float height, float radius);
void draw_settings_nav_icon(Gdiplus::Graphics& graphics, float origin_x, float origin_y, float size, Gdiplus::Pen& pen,
                            Gdiplus::SolidBrush& soft_fill);
void draw_nav_icon(HDC dc, Page page, const RECT& rect, COLORREF color);
bool gpu_ready(const UiState& state);

}  // namespace superzip::app
