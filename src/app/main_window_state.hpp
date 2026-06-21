#pragma once

#include "core/progress.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
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
    Settings,
    About,
};

enum class ToggleId {
    None,
    VerifyAfterWrite,
    IntegrityHash,
    DefenderScan,
    GpuRequired,
    SolidArchive,
    StoreTimestamps,
    DeleteAfterCompression,
    VerifyMetadata,
    OpenDestinationAfterExtract,
    OpenDestinationAfterOperation,
    ConfirmBeforeDeleting,
    ShowOperationSummary,
};

enum class DropdownId {
    None,
    CompressFormat,
    CompressLevel,
    CompressMethod,
    CompressBlockSize,
    ExtractOverwrite,
    HistoryOperation,
    HistoryStatus,
    GpuUpdateSpeed,
    SystemIoDrive,
    SettingsMemoryPolicy,
    SettingsLogLevel,
    SettingsLogRetention,
};

enum class LogSeverity {
    Information,
    Warning,
    Debug,
};

enum class FocusTargetKind {
    Navigation,
    QueueAddFiles,
    QueueAddFolder,
    QueueRemoveSelected,
    QueueClear,
    QueueHeaderCheckbox,
    QueueRow,
    CompressStart,
    CompressDestination,
    CompressFormat,
    CompressLevel,
    CompressMethod,
    CompressBlockSize,
    CompressSolidArchive,
    CompressStoreTimestamps,
    CompressDeleteAfterCompression,
    CompressVerifyAfterWrite,
    CompressIntegrityHash,
    CompressDefenderScan,
    ExtractStart,
    ExtractDestination,
    ExtractOverwrite,
    ExtractVerifyMetadata,
    ExtractOpenDestination,
    ExtractIntegrityHash,
    ExtractDefenderScan,
    SecurityVerify,
    HistoryOperation,
    HistoryStatus,
    HistoryClear,
    HistoryRow,
    SystemUpdateSpeed,
    SystemIoDrive,
    SettingsOpenDestination,
    SettingsConfirmDelete,
    SettingsShowSummary,
    SettingsIntegrityHash,
    SettingsDefenderScan,
    SettingsGpuRequired,
    SettingsVerifyAfterWrite,
    SettingsMemoryPolicy,
    SettingsLogLevel,
    SettingsLogRetention,
    SettingsOpenLogFile,
    SettingsRestoreDefaults,
    SettingsApply,
    AboutLicenses,
};

struct FocusTarget {
    FocusTargetKind kind = FocusTargetKind::Navigation;
    RECT rect{};
    int index = 0;
};

struct PerformanceMonitorSample {
    bool live = false;
    bool gpu_utilization_available = false;
    double cpu_percent = 0.0;
    double process_cpu_percent = 0.0;
    double gpu_utilization_percent = 0.0;
    double system_memory_percent = 0.0;
    double io_busy_percent = 0.0;
    double io_read_bytes_per_second = 0.0;
    double io_write_bytes_per_second = 0.0;
    std::uint64_t private_bytes = 0;
    std::uint64_t system_memory_total_bytes = 0;
    std::uint64_t system_memory_used_bytes = 0;
    std::uint64_t vram_total_bytes = 0;
    std::uint64_t vram_free_bytes = 0;
    std::uint64_t process_dedicated_vram_bytes = 0;
};

struct HistoryEntry {
    std::string operation;
    std::string archive_name;
    std::string archive_path;
    std::string detail;
    bool success = true;
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
};

struct LogEntry {
    LogSeverity severity = LogSeverity::Information;
    std::string message;
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
};

struct AppSettings {
    int compression_format_index = 0;
    int compression_level_index = 2;
    int compression_block_size_index = 2;
    int memory_policy_index = 0;
    int log_level_index = 0;
    int log_retention_index = 0;
    int performance_update_seconds = 3;
    bool open_destination_after_operation = false;
    bool confirm_before_deleting = true;
    bool show_operation_summary = true;
    bool solid_archive = true;
    bool store_timestamps = true;
    bool delete_after_compression = false;
    bool verify_metadata_before_extract = true;
    bool open_destination_after_extract = false;
    bool gpu_required = true;
    bool overwrite = false;
    bool integrity_hash_opt_in = false;
    bool defender_scan_opt_in = false;
    bool verify_after_write_opt_in = false;
};

struct UiState {
    Page page = Page::Queue;
    std::vector<std::filesystem::path> queued_paths;
    std::vector<bool> queued_enabled;
    std::vector<HistoryEntry> history;
    std::vector<LogEntry> logs;
    std::string status = "Ready";
    std::string gpu_status;
    std::string gpu_runtime_name;
    std::string gpu_device_name;
    std::string gpu_arch;
    ProgressSnapshot progress;
    std::chrono::steady_clock::time_point progress_visible_until{};
    PerformanceMonitorSample performance;
    std::filesystem::path destination_directory;
    int selected_queue_index = -1;
    int compression_format_index = 0;
    int compression_level_index = 2;
    int compression_block_size_index = 2;
    int memory_policy_index = 0;
    int log_level_index = 0;
    int log_retention_index = 0;
    int selected_history_index = -1;
    int history_operation_filter_index = 0;
    int history_status_filter_index = 0;
    int io_drive_index = 0;
    int performance_update_seconds = 3;
    bool open_destination_after_operation = false;
    bool confirm_before_deleting = true;
    bool show_operation_summary = true;
    bool solid_archive = true;
    bool store_timestamps = true;
    bool delete_after_compression = false;
    bool verify_metadata_before_extract = true;
    bool open_destination_after_extract = false;
    bool prefer_suzip = true;
    bool gpu_required = true;
    bool overwrite = false;
    bool integrity_hash_opt_in = false;
    bool defender_scan_opt_in = false;
    bool verify_after_write_opt_in = false;
    bool extract_overwrite_prompt_visible = false;
    bool license_notices_dialog_visible = false;
    std::wstring extract_overwrite_prompt_destination;
    DropdownId active_dropdown = DropdownId::None;
};

}  // namespace superzip::app
