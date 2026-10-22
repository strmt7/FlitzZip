#pragma once

#include "core/archive_name_encoding.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

namespace flitzzip::app {

struct QueueLayout {
    RECT area{};
    RECT remove_selected{};
    RECT add_files{};
    RECT add_folder{};
    RECT clear{};
    RECT table{};
};

struct QueueColumnLayout {
    RECT header_checkbox{};
    RECT checkbox{};
    RECT name{};
    RECT size{};
    RECT type{};
    RECT path{};
    std::array<RECT, 3> resize_grips{};
};

struct HistoryColumnLayout {
    RECT time{};
    RECT operation{};
    RECT archive_name{};
    RECT archive_path{};
    RECT status{};
    std::array<RECT, 4> resize_grips{};
};

struct ExtractJobRequest {
    std::vector<std::filesystem::path> archives;
    std::filesystem::path output;
    bool gpu_required = true;
    bool overwrite = false;
    bool integrity = false;
    bool defender = false;
    bool validate_before_publish = true;
    bool open_destination = false;
    ArchivePathEncoding name_encoding = ArchivePathEncoding::Utf8;
};

// Independent selected-drive measurements; an absent counter is never an observed idle rate.
struct ProcessIoRates {
    std::wstring drive;
    std::optional<double> busy_percent;
    std::optional<double> read_bytes_per_second;
    std::optional<double> write_bytes_per_second;
};

struct CompressLayout {
    RECT area{};
    RECT archive_name{};
    RECT destination{};
    RECT format{};
    RECT compression_level{};
    RECT method{};
    RECT block_size{};
    RECT advanced{};
    RECT verify{};
    RECT security{};
    RECT sha{};
    RECT defender{};
    RECT stop{};
    RECT start{};
};

struct HistoryLayout {
    RECT area{};
    RECT operation_filter{};
    RECT status_filter{};
    RECT clear{};
    RECT table{};
    RECT details{};
};

struct ExtractLayout {
    RECT area{};
    RECT archive{};
    RECT destination{};
    RECT path_mode{};
    RECT overwrite_policy{};
    RECT name_encoding{};
    RECT checks{};
    RECT validate_before_publish{};
    RECT open_destination_after_extract{};
    RECT sha{};
    RECT defender{};
    RECT stop{};
    RECT start{};
};

struct SettingsLayout {
    RECT area{};
    RECT general{};
    RECT security{};
    RECT performance{};
    RECT logging{};
    RECT restore_defaults{};
    RECT apply{};
    RECT sha{};
    RECT defender{};
    RECT gpu{};
    RECT verify{};
    RECT log_level{};
    RECT log_retention{};
    RECT open_log_file{};
    RECT open_destination_after_operation{};
    RECT show_operation_summary{};
};

}  // namespace flitzzip::app
