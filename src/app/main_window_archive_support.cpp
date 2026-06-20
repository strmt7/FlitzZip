#include "app/main_window_support.hpp"

#include "app/log_retention.hpp"
#include "ar/ar_adapter.hpp"
#include "arc/arc_adapter.hpp"
#include "arj/arj_adapter.hpp"
#include "base64/base64_adapter.hpp"
#include "bzip2/bzip2_adapter.hpp"
#include "cab/cab_adapter.hpp"
#include "core/archive.hpp"
#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "cpio/cpio_adapter.hpp"
#include "gzip/gzip_adapter.hpp"
#include "hqx/hqx_adapter.hpp"
#include "iso/iso_adapter.hpp"
#include "lha/lha_adapter.hpp"
#include "lzip/lzip_adapter.hpp"
#include "lzma/lzma_adapter.hpp"
#include "macbinary/macbinary_adapter.hpp"
#include "rpm/rpm_adapter.hpp"
#include "sevenzip/sevenzip_adapter.hpp"
#include "tar/tar_adapter.hpp"
#include "unix_compress/unix_compress_adapter.hpp"
#include "uue/uue_adapter.hpp"
#include "wim/wim_adapter.hpp"
#include "xar/xar_adapter.hpp"
#include "xxe/xxe_adapter.hpp"
#include "xz/xz_adapter.hpp"
#include "zip/zip_adapter.hpp"
#include "zstd/zstd_adapter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>

#include <knownfolders.h>
#include <shlobj.h>

namespace superzip::app {
namespace {

constexpr std::array<std::string_view, 13> kSettingsV1CompressionFormatExtensions{
    ".suzip", ".zip", ".tar", ".tar.gz", ".tar.bz2", ".tar.zst", ".gz", ".bz2", ".zst", ".Z", ".cpio", ".cpgz", ".ar",
};

// Purpose: Normalize a mutable compression-format index into the current extension selector.
// Inputs: `index` is a user, settings, or keyboard-provided row index.
// Outputs: Returns a valid row index inside `kCompressionCreateFormatExtensions`.
int normalize_compression_format_index(int index) {
    const int count = static_cast<int>(kCompressionCreateFormatExtensions.size());
    return (index % count + count) % count;
}

// Purpose: Resolve the extension metadata for a current compression selector row.
// Inputs: `index` is a mutable UI/settings row.
// Outputs: Returns the matching create-capable extension metadata, or the default `.suzip` row if corrupted.
const ArchiveFormatExtensionInfo& compression_format_entry(int index) {
    const auto normalized = static_cast<std::size_t>(normalize_compression_format_index(index));
    const auto& entry = archive_format_extension_info_for_extension(kCompressionCreateFormatExtensions[normalized]);
    if (entry.format == ArchiveFormat::Unknown || !entry.can_create) {
        return archive_format_extension_info_for_extension(
            kCompressionCreateFormatExtensions[kDefaultCompressionFormatIndex]);
    }
    return entry;
}

// Purpose: Find the current selector row for one exact extension.
// Inputs: `extension` is a supported create extension including its leading dot.
// Outputs: Returns the matching current row or the default `.suzip` row.
int compression_format_index_for_extension(std::string_view extension) {
    const auto requested = std::string_view(archive_format_extension_info_for_extension(extension).extension);
    for (int index = 0; index <= kCompressionFormatMaxIndex; ++index) {
        const auto& entry = archive_format_extension_info_for_extension(kCompressionCreateFormatExtensions[index]);
        if (std::string_view(entry.extension) == requested) {
            return index;
        }
    }
    return kDefaultCompressionFormatIndex;
}

// Purpose: Migrate a v1 persisted compression-format row to the current extension-specific selector.
// Inputs: `old_index` is the v1 row where aliases were grouped by archive format.
// Outputs: Returns the current row that preserves the old default extension choice.
int migrate_settings_v1_compression_format_index(int old_index) {
    if (old_index < 0 || old_index >= static_cast<int>(kSettingsV1CompressionFormatExtensions.size())) {
        return kDefaultCompressionFormatIndex;
    }
    return compression_format_index_for_extension(
        kSettingsV1CompressionFormatExtensions[static_cast<std::size_t>(old_index)]);
}

// Purpose: Detect whether settings were written before extension-specific format rows existed.
// Inputs: `json` is the settings document being parsed.
// Outputs: Returns true for schema v1 or schema-less early settings.
bool settings_uses_v1_format_rows(std::string_view json) {
    if (json.find("\"superzip.settings.v1\"") != std::string_view::npos) {
        return true;
    }
    return json.find("\"schema\"") == std::string_view::npos;
}

}  // namespace

// Purpose: Create a concise history message for an optional Defender scan.
// Inputs: `prefix` names the scanned item and `scan` is the Defender result.
// Outputs: Returns a user-visible status string that distinguishes clean, timeout, failed, and unavailable states.
std::string defender_history_status(std::string_view prefix, const DefenderScanResult& scan) {
    std::string message(prefix);
    if (scan.attempted && scan.clean) {
        message += " scan clean";
    } else if (scan.timed_out) {
        message += " scan timed out";
    } else if (scan.attempted) {
        message += " scan not clean";
    } else {
        message += " scan unavailable";
    }
    return message;
}

// Purpose: Map a visible busy status into the operation category used by the History tab.
// Inputs: `label` is the status text passed to the background worker.
// Outputs: Returns the stable History operation filter name for success and failure rows.
std::string operation_for_job_label(std::string_view label) {
    if (label.starts_with("Compress")) {
        return "Compress";
    }
    if (label.starts_with("Extract")) {
        return "Extract";
    }
    if (label.starts_with("Verif")) {
        return "Security";
    }
    return "Failure";
}

// Purpose: Resolve the selected destination directory or the current user's Downloads folder.
// Inputs: `state` is the copied UI state.
// Outputs: Returns a usable destination folder path for display and jobs without depending on process cwd.
std::filesystem::path destination_directory_or_default(const UiState& state) {
    if (!state.destination_directory.empty()) {
        return state.destination_directory;
    }
    return current_user_downloads_directory();
}

// Purpose: Map the visible compression-format selection to an archive format.
// Inputs: `index` is the mutable compression-format selection in UI state.
// Outputs: Returns one implemented create-capable archive format.
ArchiveFormat compression_format_value(int index) {
    return compression_format_entry(index).format;
}

// Purpose: Return the user-facing compression-format label.
// Inputs: `index` is the mutable compression-format selection in UI state.
// Outputs: Returns a core-owned label with one exact extension for this row.
std::wstring compression_format_text(int index) {
    return widen(compression_format_entry(index).display_name);
}

// Purpose: Return the default output extension for a selected compression format.
// Inputs: `index` is the mutable compression-format selection in UI state.
// Outputs: Returns the extension used for the default GUI archive name.
std::wstring compression_format_extension(int index) {
    return widen(compression_format_entry(index).extension);
}

// Purpose: Return whether a compression format uses native SUZIP tuning controls.
// Inputs: `format` is a selected create format.
// Outputs: Returns true only for the GPU-first `.suzip` format.
bool compression_format_uses_suzip_tuning(ArchiveFormat format) {
    return format == ArchiveFormat::SuperZip;
}

// Purpose: Return whether the selected create format can honor the compression-level dropdown.
// Inputs: `format` is the current compression target format.
// Outputs: Returns true for level-aware encoders and false for stored/text/container-only formats.
bool compression_format_uses_level(ArchiveFormat format) {
    switch (format) {
    case ArchiveFormat::SuperZip:
    case ArchiveFormat::Zip:
    case ArchiveFormat::TarGzip:
    case ArchiveFormat::TarBzip2:
    case ArchiveFormat::TarZstd:
    case ArchiveFormat::Gzip:
    case ArchiveFormat::Bzip2:
    case ArchiveFormat::Zstd:
    case ArchiveFormat::CpioGzip:
        return true;
    default:
        return false;
    }
}

// Purpose: Resolve the visible compression output filename.
// Inputs: `state` is the copied UI state.
// Outputs: Returns the default archive filename for the selected compression format.
std::wstring compression_output_filename_for(const UiState& state) {
    return L"SuperZip-output" + compression_format_extension(state.compression_format_index);
}

// Purpose: Resolve the visible compression output path.
// Inputs: `state` is the copied UI state.
// Outputs: Returns the destination archive path using the selected archive extension.
std::filesystem::path compression_output_path_for(const UiState& state) {
    return destination_directory_or_default(state) / compression_output_filename_for(state);
}

// Purpose: Resolve the visible extraction output path.
// Inputs: `state` is the copied UI state.
// Outputs: Returns the selected destination, or a Downloads-based extraction folder when none is selected.
std::filesystem::path extraction_output_path_for(const UiState& state) {
    if (!state.destination_directory.empty()) {
        return state.destination_directory;
    }
    return current_user_downloads_directory() / L"SuperZip-extracted";
}

using CompatibilityExtractor = OperationStats (*)(const std::filesystem::path&, const std::filesystem::path&, bool,
                                                  const ProgressCallback&);

struct CompatibilityExtractionRoute {
    ArchiveFormat format;
    CompatibilityExtractor extractor;
};

// Purpose: Find the compatibility extraction adapter for a detected archive format.
// Inputs: `archive_format` is the detected non-SUZIP format to route.
// Outputs: Returns the matching adapter function, or null when the format has no extraction implementation.
CompatibilityExtractor compatibility_extractor_for(ArchiveFormat archive_format) {
    static constexpr std::array<CompatibilityExtractionRoute, 33> routes{{
        {ArchiveFormat::Zip, extract_zip},
        {ArchiveFormat::Zipx, extract_zip},
        {ArchiveFormat::SevenZip, extract_7z},
        {ArchiveFormat::Tar, extract_tar},
        {ArchiveFormat::TarGzip, extract_tar_gzip},
        {ArchiveFormat::TarBzip2, extract_tar_bzip2},
        {ArchiveFormat::TarXz, extract_tar_xz},
        {ArchiveFormat::TarLzip, extract_tar_lzip},
        {ArchiveFormat::TarZstd, extract_tar_zstd},
        {ArchiveFormat::Gzip, extract_gzip_file},
        {ArchiveFormat::Bzip2, extract_bzip2_file},
        {ArchiveFormat::Xz, extract_xz_file},
        {ArchiveFormat::Lzma, extract_lzma_file},
        {ArchiveFormat::Lzip, extract_lzip_file},
        {ArchiveFormat::Zstd, extract_zstd_file},
        {ArchiveFormat::UnixCompress, extract_unix_compress_file},
        {ArchiveFormat::Base64, extract_base64_file},
        {ArchiveFormat::Hqx, extract_hqx_file},
        {ArchiveFormat::MacBinary, extract_macbinary_file},
        {ArchiveFormat::Xxe, extract_xxe_file},
        {ArchiveFormat::Uue, extract_uue_file},
        {ArchiveFormat::Cab, extract_cab},
        {ArchiveFormat::Iso, extract_iso},
        {ArchiveFormat::Cpio, extract_cpio},
        {ArchiveFormat::CpioGzip, extract_cpio_gzip},
        {ArchiveFormat::Ar, extract_ar},
        {ArchiveFormat::Deb, extract_ar},
        {ArchiveFormat::Arj, extract_arj},
        {ArchiveFormat::Arc, extract_arc},
        {ArchiveFormat::Rpm, extract_rpm},
        {ArchiveFormat::Lha, extract_lha},
        {ArchiveFormat::Wim, extract_wim},
        {ArchiveFormat::Xar, extract_xar},
    }};

    const auto match =
        std::find_if(routes.begin(), routes.end(), [archive_format](const CompatibilityExtractionRoute& route) {
            return route.format == archive_format;
        });
    return match == routes.end() ? nullptr : match->extractor;
}

// Purpose: Dispatch one detected archive to the matching extraction adapter.
// Inputs: `archive_format` is the already detected format, `archive` is the input file, `output` is the extraction
// root, `gpu_required` controls only SUZIP, `overwrite` controls existing targets, and `progress_callback` receives
// progress updates. Outputs: Returns extraction statistics, or throws when the format is unknown, unsupported, unsafe,
// or corrupt.
OperationStats extract_detected_archive(ArchiveFormat archive_format, const std::filesystem::path& archive,
                                        const std::filesystem::path& output, bool gpu_required, bool overwrite,
                                        const ProgressCallback& progress_callback) {
    if (archive_format == ArchiveFormat::SuperZip) {
        ExtractOptions options;
        options.gpu_required = gpu_required;
        options.overwrite = overwrite;
        return extract_suzip(archive, output, options, progress_callback);
    }

    if (archive_format == ArchiveFormat::Unknown) {
        throw ArchiveError("unable to detect archive format: " + archive.string());
    }
    if (const auto extractor = compatibility_extractor_for(archive_format); extractor != nullptr) {
        return extractor(archive, output, overwrite, progress_callback);
    }
    throw ArchiveError(std::string("archive format recognized but not implemented for extraction: ") +
                       archive_format_info(archive_format).key);
}

// Purpose: Return the user-facing compression-level label.
// Inputs: `index` is the mutable compression-level selection in UI state.
// Outputs: Returns a stable label that maps to a zlib/miniz compression level.
std::wstring compression_level_text(int index) {
    constexpr std::array<std::wstring_view, 5> labels{
        L"Fastest", L"Fast", L"Balanced", L"Strong", L"Maximum",
    };
    const auto normalized = static_cast<std::size_t>(
        (index % static_cast<int>(labels.size()) + static_cast<int>(labels.size())) % static_cast<int>(labels.size()));
    return std::wstring(labels[normalized]);
}

// Purpose: Normalize a performance-sampling interval to the closest supported GUI option.
// Inputs: `seconds` is a persisted or in-memory sampling interval.
// Outputs: Returns one of 1, 3, 5, or 10 seconds.
int normalize_performance_update_seconds(int seconds) {
    return *std::min_element(
        kPerformanceUpdateSecondsOptions.begin(), kPerformanceUpdateSecondsOptions.end(),
        [seconds](int left, int right) { return std::abs(left - seconds) < std::abs(right - seconds); });
}

// Purpose: Resolve the dropdown row for a normalized performance-sampling interval.
// Inputs: `seconds` is a persisted or in-memory sampling interval.
// Outputs: Returns a zero-based index in `kPerformanceUpdateSecondsOptions`.
int performance_update_index_for_seconds(int seconds) {
    const int normalized = normalize_performance_update_seconds(seconds);
    const auto found =
        std::find(kPerformanceUpdateSecondsOptions.begin(), kPerformanceUpdateSecondsOptions.end(), normalized);
    return found == kPerformanceUpdateSecondsOptions.end()
               ? 0
               : static_cast<int>(std::distance(kPerformanceUpdateSecondsOptions.begin(), found));
}

// Purpose: Return a compact label for the live performance sampling interval.
// Inputs: `seconds` is a user-facing interval normalized to supported choices.
// Outputs: Returns a compact label such as `1 s`.
std::wstring performance_update_speed_text(int seconds) {
    const int clamped = normalize_performance_update_seconds(seconds);
    return std::to_wstring(clamped) + L" s";
}

// Purpose: Return the dropdown option text for one live performance update interval.
// Inputs: `index` is a zero-based dropdown row.
// Outputs: Returns the matching 1, 3, 5, or 10 second label.
std::wstring performance_update_option_text(int index) {
    const auto normalized =
        static_cast<std::size_t>((index % static_cast<int>(kPerformanceUpdateSecondsOptions.size()) +
                                  static_cast<int>(kPerformanceUpdateSecondsOptions.size())) %
                                 static_cast<int>(kPerformanceUpdateSecondsOptions.size()));
    return performance_update_speed_text(kPerformanceUpdateSecondsOptions[normalized]);
}

// Purpose: Map the visible compression-level selection to a miniz setting.
// Inputs: `index` is the mutable compression-level selection in UI state.
// Outputs: Returns one of the supported non-store compression settings: 1, 3, 5, 7, or 9.
int compression_level_value(int index) {
    constexpr std::array<int, 5> options{1, 3, superzip::kDefaultCompressionLevel, 7, 9};
    const auto normalized =
        static_cast<std::size_t>((index % static_cast<int>(options.size()) + static_cast<int>(options.size())) %
                                 static_cast<int>(options.size()));
    return options[normalized];
}

// Purpose: Run the selected create backend for a GUI compression job.
// Inputs: `sources`, `output`, `archive_format`, GPU options, `block_size`, `compression_level`, and progress callback.
// Outputs: Returns backend telemetry or throws when the selected format cannot be created.
OperationStats compress_gui_archive(const std::vector<std::filesystem::path>& sources,
                                    const std::filesystem::path& output, ArchiveFormat archive_format,
                                    bool gpu_required, bool verify_after_write, std::uint32_t block_size,
                                    int compression_level, const ProgressCallback& progress_callback) {
    switch (archive_format) {
    case ArchiveFormat::SuperZip: {
        CompressOptions options;
        options.gpu_required = gpu_required;
        options.block_size = block_size;
        options.compression_level = compression_level;
        options.verify_after_write = verify_after_write;
        return compress_suzip(sources, output, options, progress_callback);
    }
    case ArchiveFormat::Zip:
        return compress_zip(sources, output, compression_level, progress_callback);
    case ArchiveFormat::Tar:
        return compress_tar(sources, output, progress_callback);
    case ArchiveFormat::TarGzip:
        return compress_tar_gzip(sources, output, compression_level, progress_callback);
    case ArchiveFormat::TarBzip2:
        return compress_tar_bzip2(sources, output, compression_level, progress_callback);
    case ArchiveFormat::TarZstd:
        return compress_tar_zstd(sources, output, compression_level, progress_callback);
    case ArchiveFormat::Gzip:
        return compress_gzip(sources, output, compression_level, progress_callback);
    case ArchiveFormat::Bzip2:
        return compress_bzip2(sources, output, compression_level, progress_callback);
    case ArchiveFormat::Zstd:
        return compress_zstd(sources, output, compression_level, progress_callback);
    case ArchiveFormat::UnixCompress:
        return compress_unix_compress(sources, output, progress_callback);
    case ArchiveFormat::Cpio:
        return compress_cpio(sources, output, progress_callback);
    case ArchiveFormat::CpioGzip:
        return compress_cpio_gzip(sources, output, compression_level, progress_callback);
    case ArchiveFormat::Ar:
        return compress_ar(sources, output, progress_callback);
    default:
        throw ArchiveError(std::string("archive format recognized but not implemented for compression: ") +
                           archive_format_info(archive_format).key);
    }
}

// Purpose: Return the compression block-size labels shown in the Compress page.
// Inputs: `index` is the mutable block-size selection in UI state.
// Outputs: Returns a meaningful tuning label that maps to a supported archive block size.
std::wstring compression_block_size_text(int index) {
    constexpr std::array<std::wstring_view, kCompressionBlockSizeOptions.size()> labels{
        L"256 KiB", L"512 KiB", L"1 MiB", L"2 MiB", L"4 MiB", L"8 MiB", L"16 MiB",
    };
    const auto normalized = static_cast<std::size_t>(
        (index % static_cast<int>(labels.size()) + static_cast<int>(labels.size())) % static_cast<int>(labels.size()));
    return std::wstring(labels[normalized]);
}

// Purpose: Map a compression block-size selection to bytes accepted by the archive core.
// Inputs: `index` is the mutable block-size selection in UI state.
// Outputs: Returns a block size between `kMinArchiveBlockBytes` and `kMaxArchiveBlockBytes`.
std::uint32_t compression_block_size_bytes(int index) {
    const auto normalized = static_cast<std::size_t>((index % static_cast<int>(kCompressionBlockSizeOptions.size()) +
                                                      static_cast<int>(kCompressionBlockSizeOptions.size())) %
                                                     static_cast<int>(kCompressionBlockSizeOptions.size()));
    return kCompressionBlockSizeOptions[normalized];
}

// Purpose: Return a label from a circular option list.
// Inputs: `index` is the current selection and `labels` contains display choices.
// Outputs: Returns the normalized display label.
template <std::size_t Count> std::wstring option_text(int index, const std::array<std::wstring_view, Count>& labels) {
    static_assert(Count > 0);
    const auto normalized =
        static_cast<std::size_t>((index % static_cast<int>(Count) + static_cast<int>(Count)) % static_cast<int>(Count));
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
        L"Warning",
        L"Debug",
    };
    return option_text(index, labels);
}

// Purpose: Return the visible log-retention label.
// Inputs: `index` is the mutable preferences selection.
// Outputs: Returns a session preference label.
std::wstring log_retention_text(int index) {
    return std::wstring(log_retention_display_text(index));
}

// Purpose: Return a JSON-safe string literal for a path or status field.
// Inputs: `value` is UTF-8 text that may contain JSON metacharacters.
// Outputs: Returns a quoted JSON string with control characters escaped.
std::string json_string(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 2U);
    escaped.push_back('"');
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (ch < 0x20U) {
                constexpr char kHex[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped.push_back(kHex[(ch >> 4U) & 0x0FU]);
                escaped.push_back(kHex[ch & 0x0FU]);
            } else {
                escaped.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    escaped.push_back('"');
    return escaped;
}

// Purpose: Extract an integer setting from the versioned JSON config.
// Inputs: `json`, `key`, and fallback/bounds define the accepted value.
// Outputs: Returns the parsed integer clamped to the supported range.
int json_int_setting(std::string_view json, std::string_view key, int fallback, int minimum, int maximum) {
    const std::string quoted_key = json_string(key);
    const auto key_pos = json.find(std::string_view(quoted_key));
    if (key_pos == std::string_view::npos) {
        return fallback;
    }
    const auto colon_pos = json.find(':', key_pos + quoted_key.size());
    if (colon_pos == std::string_view::npos) {
        return fallback;
    }
    const auto value_pos = json.find_first_of("-0123456789", colon_pos + 1U);
    if (value_pos == std::string_view::npos) {
        return fallback;
    }
    std::size_t end_pos = value_pos;
    if (json[end_pos] == '-') {
        ++end_pos;
    }
    while (end_pos < json.size() && std::isdigit(static_cast<unsigned char>(json[end_pos])) != 0) {
        ++end_pos;
    }
    try {
        const int parsed = std::stoi(std::string(json.substr(value_pos, end_pos - value_pos)));
        return std::clamp(parsed, minimum, maximum);
    } catch (...) {
        return fallback;
    }
}

// Purpose: Extract a boolean setting from the versioned JSON config.
// Inputs: `json`, `key`, and `fallback` identify the setting and default.
// Outputs: Returns the parsed boolean or fallback when the setting is absent/malformed.
bool json_bool_setting(std::string_view json, std::string_view key, bool fallback) {
    const std::string quoted_key = json_string(key);
    const auto key_pos = json.find(std::string_view(quoted_key));
    if (key_pos == std::string_view::npos) {
        return fallback;
    }
    const auto colon_pos = json.find(':', key_pos + quoted_key.size());
    if (colon_pos == std::string_view::npos) {
        return fallback;
    }
    const auto value_pos = json.find_first_not_of(" \t\r\n", colon_pos + 1U);
    if (value_pos == std::string_view::npos) {
        return fallback;
    }
    if (json.substr(value_pos, 4) == "true") {
        return true;
    }
    if (json.substr(value_pos, 5) == "false") {
        return false;
    }
    return fallback;
}

// Purpose: Copy persisted settings into visible UI state.
// Inputs: `settings` is already validated and `state` is the UI model to mutate.
// Outputs: Updates Settings-owned fields without touching queue/history/runtime-only state.
void apply_settings_to_state(const AppSettings& settings, UiState& state) {
    state.compression_format_index = settings.compression_format_index;
    state.compression_level_index = settings.compression_level_index;
    state.compression_block_size_index = settings.compression_block_size_index;
    state.memory_policy_index = settings.memory_policy_index;
    state.log_level_index = settings.log_level_index;
    state.log_retention_index = settings.log_retention_index;
    state.performance_update_seconds = normalize_performance_update_seconds(settings.performance_update_seconds);
    state.open_destination_after_operation = settings.open_destination_after_operation;
    state.confirm_before_deleting = settings.confirm_before_deleting;
    state.show_operation_summary = settings.show_operation_summary;
    state.solid_archive = settings.solid_archive;
    state.store_timestamps = settings.store_timestamps;
    state.delete_after_compression = settings.delete_after_compression;
    state.verify_metadata_before_extract = settings.verify_metadata_before_extract;
    state.open_destination_after_extract = settings.open_destination_after_extract;
    state.gpu_required = settings.gpu_required;
    state.overwrite = settings.overwrite;
    state.integrity_hash_opt_in = settings.integrity_hash_opt_in;
    state.defender_scan_opt_in = settings.defender_scan_opt_in;
    state.verify_after_write_opt_in = settings.verify_after_write_opt_in;
}

// Purpose: Capture the Settings-owned fields from visible UI state.
// Inputs: `state` is the synchronized UI model.
// Outputs: Returns a validated settings snapshot suitable for persistence.
AppSettings settings_from_state(const UiState& state) {
    AppSettings settings;
    settings.compression_format_index = std::clamp(state.compression_format_index, 0, kCompressionFormatMaxIndex);
    settings.compression_level_index = std::clamp(state.compression_level_index, 0, 4);
    settings.compression_block_size_index =
        std::clamp(state.compression_block_size_index, 0, kCompressionBlockSizeMaxIndex);
    settings.memory_policy_index = std::clamp(state.memory_policy_index, 0, 2);
    settings.log_level_index = std::clamp(state.log_level_index, 0, 2);
    settings.log_retention_index = std::clamp(state.log_retention_index, 0, 2);
    settings.performance_update_seconds = normalize_performance_update_seconds(state.performance_update_seconds);
    settings.open_destination_after_operation = state.open_destination_after_operation;
    settings.confirm_before_deleting = state.confirm_before_deleting;
    settings.show_operation_summary = state.show_operation_summary;
    settings.solid_archive = state.solid_archive;
    settings.store_timestamps = state.store_timestamps;
    settings.delete_after_compression = state.delete_after_compression;
    settings.verify_metadata_before_extract = state.verify_metadata_before_extract;
    settings.open_destination_after_extract = state.open_destination_after_extract;
    settings.gpu_required = state.gpu_required;
    settings.overwrite = state.overwrite;
    settings.integrity_hash_opt_in = state.integrity_hash_opt_in;
    settings.defender_scan_opt_in = state.defender_scan_opt_in;
    settings.verify_after_write_opt_in = state.verify_after_write_opt_in;
    return settings;
}

// Purpose: Compare two Settings snapshots exactly.
// Inputs: `left` and `right` are validated snapshots.
// Outputs: Returns true when every persisted setting matches.
bool settings_equal(const AppSettings& left, const AppSettings& right) {
    return left.compression_format_index == right.compression_format_index &&
           left.compression_level_index == right.compression_level_index &&
           left.compression_block_size_index == right.compression_block_size_index &&
           left.memory_policy_index == right.memory_policy_index && left.log_level_index == right.log_level_index &&
           left.log_retention_index == right.log_retention_index &&
           left.performance_update_seconds == right.performance_update_seconds &&
           left.open_destination_after_operation == right.open_destination_after_operation &&
           left.confirm_before_deleting == right.confirm_before_deleting &&
           left.show_operation_summary == right.show_operation_summary && left.solid_archive == right.solid_archive &&
           left.store_timestamps == right.store_timestamps &&
           left.delete_after_compression == right.delete_after_compression &&
           left.verify_metadata_before_extract == right.verify_metadata_before_extract &&
           left.open_destination_after_extract == right.open_destination_after_extract &&
           left.gpu_required == right.gpu_required && left.overwrite == right.overwrite &&
           left.integrity_hash_opt_in == right.integrity_hash_opt_in &&
           left.defender_scan_opt_in == right.defender_scan_opt_in &&
           left.verify_after_write_opt_in == right.verify_after_write_opt_in;
}

// Purpose: Parse a settings JSON document into a validated snapshot.
// Inputs: `json` is the complete UTF-8 settings document.
// Outputs: Returns settings with missing or malformed values replaced by defaults.
AppSettings parse_settings_json(std::string_view json) {
    AppSettings settings;
    const bool migrate_format_rows = settings_uses_v1_format_rows(json);
    if (migrate_format_rows) {
        const int v1_index = json_int_setting(json, "compressionFormatIndex", 0, 0,
                                              static_cast<int>(kSettingsV1CompressionFormatExtensions.size()) - 1);
        settings.compression_format_index = migrate_settings_v1_compression_format_index(v1_index);
    } else {
        settings.compression_format_index = json_int_setting(
            json, "compressionFormatIndex", settings.compression_format_index, 0, kCompressionFormatMaxIndex);
    }
    settings.compression_level_index =
        json_int_setting(json, "compressionLevelIndex", settings.compression_level_index, 0, 4);
    settings.compression_block_size_index = json_int_setting(
        json, "compressionBlockSizeIndex", settings.compression_block_size_index, 0, kCompressionBlockSizeMaxIndex);
    settings.memory_policy_index = json_int_setting(json, "memoryPolicyIndex", settings.memory_policy_index, 0, 2);
    settings.log_level_index = json_int_setting(json, "logLevelIndex", settings.log_level_index, 0, 2);
    settings.log_retention_index = json_int_setting(json, "logRetentionIndex", settings.log_retention_index, 0, 2);
    settings.performance_update_seconds = normalize_performance_update_seconds(
        json_int_setting(json, "performanceUpdateSeconds", settings.performance_update_seconds, 1, 10));
    settings.open_destination_after_operation =
        json_bool_setting(json, "openDestinationAfterOperation", settings.open_destination_after_operation);
    settings.confirm_before_deleting =
        json_bool_setting(json, "confirmBeforeDeleting", settings.confirm_before_deleting);
    settings.show_operation_summary = json_bool_setting(json, "showOperationSummary", settings.show_operation_summary);
    settings.solid_archive = json_bool_setting(json, "solidArchive", settings.solid_archive);
    settings.store_timestamps = json_bool_setting(json, "storeTimestamps", settings.store_timestamps);
    settings.delete_after_compression =
        json_bool_setting(json, "deleteAfterCompression", settings.delete_after_compression);
    settings.verify_metadata_before_extract =
        json_bool_setting(json, "verifyMetadataBeforeExtract", settings.verify_metadata_before_extract);
    settings.open_destination_after_extract =
        json_bool_setting(json, "openDestinationAfterExtract", settings.open_destination_after_extract);
    settings.gpu_required = json_bool_setting(json, "gpuRequired", settings.gpu_required);
    settings.overwrite = json_bool_setting(json, "overwrite", settings.overwrite);
    settings.integrity_hash_opt_in = json_bool_setting(json, "integrityHashOptIn", settings.integrity_hash_opt_in);
    settings.defender_scan_opt_in = json_bool_setting(json, "defenderScanOptIn", settings.defender_scan_opt_in);
    settings.verify_after_write_opt_in =
        json_bool_setting(json, "verifyAfterWriteOptIn", settings.verify_after_write_opt_in);
    return settings;
}

// Purpose: Serialize a validated settings snapshot as deterministic JSON.
// Inputs: `settings` is the applied Settings snapshot.
// Outputs: Returns a UTF-8 JSON document with stable key order.
std::string settings_to_json(const AppSettings& settings) {
    auto bool_text = [](bool value) { return value ? "true" : "false"; };
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"superzip.settings.v2\",\n"
        << "  \"compressionFormatIndex\": " << settings.compression_format_index << ",\n"
        << "  \"compressionLevelIndex\": " << settings.compression_level_index << ",\n"
        << "  \"compressionBlockSizeIndex\": " << settings.compression_block_size_index << ",\n"
        << "  \"memoryPolicyIndex\": " << settings.memory_policy_index << ",\n"
        << "  \"logLevelIndex\": " << settings.log_level_index << ",\n"
        << "  \"logRetentionIndex\": " << settings.log_retention_index << ",\n"
        << "  \"performanceUpdateSeconds\": " << settings.performance_update_seconds << ",\n"
        << "  \"openDestinationAfterOperation\": " << bool_text(settings.open_destination_after_operation) << ",\n"
        << "  \"confirmBeforeDeleting\": " << bool_text(settings.confirm_before_deleting) << ",\n"
        << "  \"showOperationSummary\": " << bool_text(settings.show_operation_summary) << ",\n"
        << "  \"solidArchive\": " << bool_text(settings.solid_archive) << ",\n"
        << "  \"storeTimestamps\": " << bool_text(settings.store_timestamps) << ",\n"
        << "  \"deleteAfterCompression\": " << bool_text(settings.delete_after_compression) << ",\n"
        << "  \"verifyMetadataBeforeExtract\": " << bool_text(settings.verify_metadata_before_extract) << ",\n"
        << "  \"openDestinationAfterExtract\": " << bool_text(settings.open_destination_after_extract) << ",\n"
        << "  \"gpuRequired\": " << bool_text(settings.gpu_required) << ",\n"
        << "  \"overwrite\": " << bool_text(settings.overwrite) << ",\n"
        << "  \"integrityHashOptIn\": " << bool_text(settings.integrity_hash_opt_in) << ",\n"
        << "  \"defenderScanOptIn\": " << bool_text(settings.defender_scan_opt_in) << ",\n"
        << "  \"verifyAfterWriteOptIn\": " << bool_text(settings.verify_after_write_opt_in) << "\n"
        << "}\n";
    return out.str();
}

// Purpose: Resolve the per-user SuperZip settings file path.
// Inputs: None; uses Windows Known Folders, or a fixed temp file when GUI smoke redirect is explicitly enabled.
// Outputs: Returns a trusted settings path or throws on lookup failure.
std::filesystem::path settings_file_path() {
    wchar_t smoke_redirect[8]{};
    constexpr DWORD smoke_redirect_capacity = static_cast<DWORD>(sizeof(smoke_redirect) / sizeof(smoke_redirect[0]));
    const DWORD smoke_redirect_length =
        GetEnvironmentVariableW(L"SUPERZIP_GUI_SMOKE_SETTINGS_REDIRECT", smoke_redirect, smoke_redirect_capacity);
    if (smoke_redirect_length == 1 && smoke_redirect[0] == L'1') {
        return std::filesystem::temp_directory_path() / L"SuperZip" / L"gui-smoke-settings.json";
    }
    PWSTR local_app_data = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local_app_data);
    if (FAILED(result) || local_app_data == nullptr) {
        throw ArchiveError("could not resolve Local AppData for settings");
    }
    std::filesystem::path path(local_app_data);
    CoTaskMemFree(local_app_data);
    return path / L"SuperZip" / L"settings.json";
}

// Purpose: Read the per-user settings file when it exists.
// Inputs: `path` is the settings file location.
// Outputs: Returns parsed settings or defaults when the file is absent.
AppSettings read_settings_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return {};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw ArchiveError("could not open settings file for reading");
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return parse_settings_json(buffer.str());
}

// Purpose: Atomically write the per-user settings file.
// Inputs: `path` is the target file and `settings` is the validated snapshot.
// Outputs: Creates the settings directory and replaces the target with deterministic JSON.
void write_settings_file(const std::filesystem::path& path, const AppSettings& settings) {
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = path.parent_path() / (path.filename().wstring() + L".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ArchiveError("could not open settings file for writing");
        }
        const auto json = settings_to_json(settings);
        output.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!output) {
            throw ArchiveError("could not write complete settings file");
        }
    }
    if (!MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code cleanup_ec;
        std::filesystem::remove(temporary, cleanup_ec);
        throw ArchiveError("could not replace settings file atomically");
    }
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
        L"All",
        L"Success",
        L"Failure",
    };
    return option_text(index, labels);
}

// Purpose: Return the display options for an expanded dropdown.
// Inputs: `id` identifies the dropdown control.
// Outputs: Returns ordered labels matching the selectable rows.
std::vector<std::wstring> dropdown_options(DropdownId id) {
    switch (id) {
    case DropdownId::CompressFormat: {
        std::vector<std::wstring> formats;
        formats.reserve(kCompressionCreateFormatExtensions.size());
        for (int index = 0; index <= kCompressionFormatMaxIndex; ++index) {
            formats.push_back(compression_format_text(index));
        }
        return formats;
    }
    case DropdownId::CompressLevel:
        return {
            compression_level_text(0), compression_level_text(1), compression_level_text(2),
            compression_level_text(3), compression_level_text(4),
        };
    case DropdownId::CompressMethod:
        return {L"AMD HIP required", L"AMD HIP preferred"};
    case DropdownId::CompressBlockSize:
        return [] {
            std::vector<std::wstring> options;
            options.reserve(kCompressionBlockSizeOptions.size());
            for (int index = 0; index <= kCompressionBlockSizeMaxIndex; ++index) {
                options.push_back(compression_block_size_text(index));
            }
            return options;
        }();
    case DropdownId::ExtractOverwrite:
        return {L"Ask before overwriting", L"Overwrite without asking"};
    case DropdownId::HistoryOperation:
        return {history_operation_filter_text(0), history_operation_filter_text(1), history_operation_filter_text(2),
                history_operation_filter_text(3)};
    case DropdownId::HistoryStatus:
        return {history_status_filter_text(0), history_status_filter_text(1), history_status_filter_text(2)};
    case DropdownId::GpuUpdateSpeed:
        return {
            performance_update_option_text(0),
            performance_update_option_text(1),
            performance_update_option_text(2),
            performance_update_option_text(3),
        };
    case DropdownId::SettingsMemoryPolicy:
        return {memory_policy_text(0), memory_policy_text(1), memory_policy_text(2)};
    case DropdownId::SettingsLogLevel:
        return {log_level_text(0), log_level_text(1), log_level_text(2)};
    case DropdownId::SettingsLogRetention:
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
    case DropdownId::CompressFormat:
        return state.compression_format_index;
    case DropdownId::CompressLevel:
        return state.compression_level_index;
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
    case DropdownId::GpuUpdateSpeed:
        return performance_update_index_for_seconds(state.performance_update_seconds);
    case DropdownId::SettingsMemoryPolicy:
        return state.memory_policy_index;
    case DropdownId::SettingsLogLevel:
        return state.log_level_index;
    case DropdownId::SettingsLogRetention:
        return state.log_retention_index;
    case DropdownId::None:
        return 0;
    }
    return 0;
}

}  // namespace superzip::app
