#pragma once

#include <array>
#include <filesystem>
#include <vector>

#include <windows.h>

namespace superzip::app {

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
    RECT archive{};
    RECT status{};
    std::array<RECT, 3> resize_grips{};
};

struct ExtractJobRequest {
    std::vector<std::filesystem::path> archives;
    std::filesystem::path output;
    bool gpu_required = true;
    bool overwrite = false;
    bool integrity = false;
    bool defender = false;
};

struct ProcessIoRates {
    double read_bytes_per_second = 0.0;
    double write_bytes_per_second = 0.0;
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
    RECT solid_archive{};
    RECT store_timestamps{};
    RECT delete_after_compression{};
    RECT verify{};
    RECT security{};
    RECT sha{};
    RECT defender{};
    RECT start{};
};

struct ExtractLayout {
    RECT area{};
    RECT archive{};
    RECT destination{};
    RECT path_mode{};
    RECT overwrite_policy{};
    RECT checks{};
    RECT verify_metadata{};
    RECT open_destination_after_extract{};
    RECT sha{};
    RECT defender{};
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
    RECT memory_policy{};
    RECT log_level{};
    RECT log_retention{};
    RECT open_destination_after_operation{};
    RECT confirm_before_deleting{};
    RECT show_operation_summary{};
};

}  // namespace superzip::app
