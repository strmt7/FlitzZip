#include "app/main_window.hpp"

#include "app/drop_payload.hpp"
#include "app/log_retention.hpp"
#include "ar/ar_adapter.hpp"
#include "arc/arc_adapter.hpp"
#include "arj/arj_adapter.hpp"
#include "base64/base64_adapter.hpp"
#include "app/resource.h"
#include "bzip2/bzip2_adapter.hpp"
#include "cab/cab_adapter.hpp"
#include "core/archive.hpp"
#include "core/archive_format.hpp"
#include "cpio/cpio_adapter.hpp"
#include "core/defender_scan.hpp"
#include "core/integrity.hpp"
#include "core/result.hpp"
#include "gzip/gzip_adapter.hpp"
#include "gpu/gpu_codec.hpp"
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
#include "zstd/zstd_adapter.hpp"
#include "zip/zip_adapter.hpp"
#include "superzip_brand_logo.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <commdlg.h>
#include <fstream>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <dwmapi.h>
#include <knownfolders.h>
#include <ole2.h>
#include <objidl.h>
#include <gdiplus.h>
#include <iomanip>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <span>
#include <sstream>
#include <system_error>
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
#ifndef MSGFLT_ALLOW
#define MSGFLT_ALLOW 1
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
constexpr int kPageTitleTextHeight = 46;
constexpr int kQueueCheckboxColumnWidth = 44;
constexpr int kQueueResizeGripHalfWidth = 4;
constexpr int kOperationProgressHeight = 5;
constexpr int kPerformanceUpdateFieldWidth = 78;
constexpr int kClockSegmentWidth = 106;
constexpr UINT_PTR kAnimationTimer = 7;
constexpr UINT_PTR kSmokeAutoCloseTimer = 9;
constexpr UINT_PTR kSmokeClosePollTimer = 10;
constexpr UINT_PTR kProgressHoldTimer = 11;
constexpr UINT_PTR kClockTimer = 12;
constexpr UINT_PTR kTextTooltipTimer = 13;
constexpr UINT kProgressHoldMs = 15000;
constexpr UINT kClockPollMs = 50;
constexpr UINT kTextTooltipDelayMs = 1000;
constexpr int kPageTransitionMs = 120;
constexpr int kToggleTransitionMs = 105;
constexpr int kButtonReleaseTransitionMs = 130;
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

constexpr std::array<ArchiveFormat, 13> kCompressionCreateFormats{
    ArchiveFormat::SuperZip, ArchiveFormat::Zip,          ArchiveFormat::Tar,  ArchiveFormat::TarGzip,
    ArchiveFormat::TarBzip2, ArchiveFormat::TarZstd,      ArchiveFormat::Gzip, ArchiveFormat::Bzip2,
    ArchiveFormat::Zstd,     ArchiveFormat::UnixCompress, ArchiveFormat::Cpio, ArchiveFormat::CpioGzip,
    ArchiveFormat::Ar,
};
constexpr int kCompressionFormatMaxIndex = static_cast<int>(kCompressionCreateFormats.size()) - 1;
constexpr std::array<int, 4> kPerformanceUpdateSecondsOptions{1, 3, 5, 10};
constexpr std::array<std::uint32_t, 7> kCompressionBlockSizeOptions{
    256U * 1024U,       512U * 1024U,       superzip::kDefaultArchiveBlockBytes, 2U * 1024U * 1024U,
    4U * 1024U * 1024U, 8U * 1024U * 1024U, superzip::kMaxArchiveBlockBytes,
};
constexpr int kCompressionBlockSizeMaxIndex = static_cast<int>(kCompressionBlockSizeOptions.size()) - 1;

// Purpose: Convert UTF-8 text to UTF-16 for Win32 rendering.
// Inputs: `value` is UTF-8 text.
// Outputs: Returns UTF-16 text; returns an empty string for empty input.
std::wstring widen(const std::string& value);

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

// Purpose: Convert a shell HDROP payload into filesystem paths.
// Inputs: `drop` is a valid HDROP handle owned by the caller.
// Outputs: Returns every nonempty path advertised by the shell payload.
std::vector<std::filesystem::path> paths_from_hdrop(HDROP drop) {
    std::vector<std::filesystem::path> paths;
    if (drop == nullptr) {
        return paths;
    }
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    paths.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        const UINT length = DragQueryFileW(drop, i, nullptr, 0);
        if (length == 0) {
            continue;
        }
        std::wstring path(length + 1, L'\0');
        if (DragQueryFileW(drop, i, path.data(), length + 1) == 0) {
            continue;
        }
        path.resize(length);
        paths.emplace_back(std::move(path));
    }
    if (!paths.empty()) {
        return paths;
    }
    return paths_from_dropfiles_global(reinterpret_cast<HGLOBAL>(drop));
}

}  // namespace

class QueueDropTarget final : public IDropTarget {
  public:
    explicit QueueDropTarget(MainWindow& owner) : owner_(owner) {}

    // Purpose: Return the COM interfaces supported by the Queue drop target.
    // Inputs: `iid` selects the interface and `object` receives the result.
    // Outputs: Returns `S_OK` for IUnknown/IDropTarget or `E_NOINTERFACE`.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    // Purpose: Increment the COM lifetime count.
    // Inputs: None.
    // Outputs: Returns the new reference count.
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++ref_count_;
    }

    // Purpose: Decrement the COM lifetime count and delete when it reaches zero.
    // Inputs: None.
    // Outputs: Returns the remaining reference count.
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --ref_count_;
        if (remaining == 0U) {
            delete this;
        }
        return remaining;
    }

    // Purpose: Enter shell drag/drop tracking for the Queue table.
    // Inputs: `data`, key state, screen point, and requested effect are supplied by OLE.
    // Outputs: Enables copy only for HDROP data over the Queue table and updates highlight state.
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL point, DWORD* effect) override {
        data_object_has_files_ = data_has_hdrop(data);
        return update_effect(point, effect);
    }

    // Purpose: Update Queue table drag/drop highlight while the shell pointer moves.
    // Inputs: Screen point and requested effect are supplied by OLE.
    // Outputs: Enables copy only while still inside the Queue table.
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override {
        return update_effect(point, effect);
    }

    // Purpose: Clear Queue drag/drop highlighting when the shell drag leaves.
    // Inputs: None.
    // Outputs: Clears live highlighting.
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        owner_.set_queue_drop_highlight(false);
        data_object_has_files_ = false;
        return S_OK;
    }

    // Purpose: Accept a shell drop only inside the Queue table.
    // Inputs: `data`, key state, screen point, and requested effect are supplied by OLE.
    // Outputs: Queues dropped paths or rejects the drop with `DROPEFFECT_NONE`.
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL point, DWORD* effect) override {
        owner_.set_queue_drop_highlight(false);
        POINT client{point.x, point.y};
        ScreenToClient(owner_.hwnd_, &client);
        auto paths = paths_from_data_object(data);
        const bool accepted = owner_.accept_dropped_paths(std::move(paths), client);
        if (effect != nullptr) {
            *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }
        data_object_has_files_ = false;
        return S_OK;
    }

  private:
    // Purpose: Check whether an OLE data object offers shell file paths.
    // Inputs: `data` is the OLE data object from drag/drop.
    // Outputs: Returns true when `CF_HDROP` data can be queried.
    static bool data_has_hdrop(IDataObject* data) {
        if (data == nullptr) {
            return false;
        }
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return data->QueryGetData(&format) == S_OK;
    }

    // Purpose: Extract HDROP paths from an OLE data object.
    // Inputs: `data` is the OLE data object from a drop event.
    // Outputs: Returns shell paths and always releases OLE storage before returning.
    static std::vector<std::filesystem::path> paths_from_data_object(IDataObject* data) {
        if (data == nullptr) {
            return {};
        }
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        if (data->GetData(&format, &medium) != S_OK) {
            return {};
        }
        struct MediumGuard {
            // Purpose: Release OLE storage exactly once when extracting shell drop paths.
            // Inputs: `value` points to the STGMEDIUM returned by `IDataObject::GetData`.
            // Outputs: Stores the medium reference for scope-exit cleanup.
            explicit MediumGuard(STGMEDIUM& value) : medium(value) {}

            MediumGuard(const MediumGuard&) = delete;
            MediumGuard& operator=(const MediumGuard&) = delete;

            // Purpose: Release the OLE storage returned by `IDataObject::GetData`.
            // Inputs: None.
            // Outputs: Calls `ReleaseStgMedium` exactly once.
            ~MediumGuard() {
                ReleaseStgMedium(&medium);
            }

            STGMEDIUM& medium;
        } guard{medium};
        if (medium.tymed != TYMED_HGLOBAL || medium.hGlobal == nullptr) {
            return {};
        }
        std::vector<std::filesystem::path> paths = paths_from_dropfiles_global(medium.hGlobal);
        if (paths.empty()) {
            paths = paths_from_hdrop(reinterpret_cast<HDROP>(medium.hGlobal));
        }
        return paths;
    }

    // Purpose: Convert an OLE screen point into the current Queue-table copy/drop effect.
    // Inputs: `point` is in screen coordinates and `effect` receives the accepted operation.
    // Outputs: Updates table highlight and returns `S_OK`.
    HRESULT update_effect(POINTL point, DWORD* effect) {
        POINT client{point.x, point.y};
        ScreenToClient(owner_.hwnd_, &client);
        const bool allowed = data_object_has_files_ && owner_.queue_drop_target_contains(client);
        owner_.set_queue_drop_highlight(allowed);
        if (effect != nullptr) {
            *effect = allowed ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }
        return S_OK;
    }

    MainWindow& owner_;
    std::atomic_ulong ref_count_{1};
    bool data_object_has_files_ = false;
};

namespace {

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

// Purpose: Resolve the selected destination directory or the process directory.
// Inputs: `state` is the copied UI state.
// Outputs: Returns a usable destination folder path for display and jobs.
std::filesystem::path destination_directory_or_default(const UiState& state) {
    if (!state.destination_directory.empty()) {
        return state.destination_directory;
    }
    return safe_current_path();
}

// Purpose: Map the visible compression-format selection to an archive format.
// Inputs: `index` is the mutable compression-format selection in UI state.
// Outputs: Returns one implemented create-capable archive format.
ArchiveFormat compression_format_value(int index) {
    const auto normalized = static_cast<std::size_t>((index % static_cast<int>(kCompressionCreateFormats.size()) +
                                                      static_cast<int>(kCompressionCreateFormats.size())) %
                                                     static_cast<int>(kCompressionCreateFormats.size()));
    return kCompressionCreateFormats[normalized];
}

// Purpose: Return the user-facing compression-format label.
// Inputs: `index` is the mutable compression-format selection in UI state.
// Outputs: Returns a stable label matching the implemented GUI create backends.
std::wstring compression_format_text(int index) {
    return widen(archive_format_info(compression_format_value(index)).display_name);
}

// Purpose: Return the default output extension for a selected compression format.
// Inputs: `format` is an implemented create-capable archive format.
// Outputs: Returns the extension used for the default GUI archive name.
std::wstring compression_format_extension(ArchiveFormat format) {
    switch (format) {
    case ArchiveFormat::SuperZip:
        return L".suzip";
    case ArchiveFormat::Zip:
        return L".zip";
    case ArchiveFormat::Tar:
        return L".tar";
    case ArchiveFormat::TarGzip:
        return L".tar.gz";
    case ArchiveFormat::TarBzip2:
        return L".tar.bz2";
    case ArchiveFormat::TarZstd:
        return L".tar.zst";
    case ArchiveFormat::Gzip:
        return L".gz";
    case ArchiveFormat::Bzip2:
        return L".bz2";
    case ArchiveFormat::Zstd:
        return L".zst";
    case ArchiveFormat::UnixCompress:
        return L".Z";
    case ArchiveFormat::Cpio:
        return L".cpio";
    case ArchiveFormat::CpioGzip:
        return L".cpgz";
    case ArchiveFormat::Ar:
        return L".ar";
    default:
        return L".suzip";
    }
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
    return L"SuperZip-output" + compression_format_extension(compression_format_value(state.compression_format_index));
}

// Purpose: Resolve the visible compression output path.
// Inputs: `state` is the copied UI state.
// Outputs: Returns the destination archive path using the selected archive extension.
std::filesystem::path compression_output_path_for(const UiState& state) {
    return destination_directory_or_default(state) / compression_output_filename_for(state);
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
    settings.compression_format_index = json_int_setting(
        json, "compressionFormatIndex", settings.compression_format_index, 0, kCompressionFormatMaxIndex);
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
        << "  \"schema\": \"superzip.settings.v1\",\n"
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
        formats.reserve(kCompressionCreateFormats.size());
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

struct SystemMemoryUsage {
    double percent = 0.0;
    std::uint64_t total_bytes = 0;
    std::uint64_t used_bytes = 0;
};

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

// Purpose: Estimate remaining operation time from total work and average throughput.
// Inputs: `snapshot` is a worker progress sample whose throughput is average bytes per second since start.
// Outputs: Returns formatted remaining time or `--` when the estimate is unavailable.
std::wstring progress_time_remaining_text(const ProgressSnapshot& snapshot) {
    if (progress_ratio(snapshot) >= 1.0) {
        return L"0 sec";
    }
    if (snapshot.total_bytes <= snapshot.processed_bytes || snapshot.throughput_bytes_per_second <= 0.0) {
        return L"--";
    }
    const auto remaining_bytes = snapshot.total_bytes - snapshot.processed_bytes;
    return duration_remaining_text(static_cast<double>(remaining_bytes) / snapshot.throughput_bytes_per_second);
}

// Purpose: Format the current local time for the status strip.
// Inputs: None; reads the local system clock.
// Outputs: Returns fixed 12-hour text as `H:MM:SS am/pm` without a leading hour zero.
std::wstring current_user_time_text() {
    SYSTEMTIME local_time{};
    GetLocalTime(&local_time);
    const bool is_pm = local_time.wHour >= 12U;
    const WORD hour12_raw = static_cast<WORD>(local_time.wHour % 12U);
    const WORD hour12 = hour12_raw == 0U ? 12U : hour12_raw;
    std::wostringstream out;
    out << static_cast<unsigned int>(hour12) << L":" << std::setfill(L'0') << std::setw(2)
        << static_cast<unsigned int>(local_time.wMinute) << L":" << std::setw(2)
        << static_cast<unsigned int>(local_time.wSecond) << (is_pm ? L" pm" : L" am");
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

// Purpose: Return the detected archive format label for the extraction page.
// Inputs: `paths` is the current queue; only the first path is the extraction archive.
// Outputs: Returns a display label without throwing for empty queues or unreadable probes.
std::wstring detected_archive_format_text(const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) {
        return L"-";
    }
    const auto format = detect_archive_format(paths.front());
    return widen(archive_format_info(format).display_name);
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
    std::vector<POINT> points;
    points.reserve(values.size() + 2U);
    points.push_back(POINT{rect.left, rect.bottom});
    for (std::size_t index = 0; index < values.size(); ++index) {
        const double x_ratio =
            values.size() == 1U ? 1.0 : static_cast<double>(index) / static_cast<double>(values.size() - 1U);
        const double y_ratio = 1.0 - std::clamp(values[index], 0.0, 1.0);
        const int x = rect.left + static_cast<int>(std::round(x_ratio * static_cast<double>(width)));
        const int y = rect.top + static_cast<int>(std::round(y_ratio * static_cast<double>(height)));
        points.push_back(POINT{x, y});
    }
    points.push_back(POINT{rect.right, rect.bottom});

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
    for (std::size_t index = 1; index + 1U < points.size(); ++index) {
        if (index == 1U) {
            MoveToEx(dc, points[index].x, points[index].y, nullptr);
        } else {
            LineTo(dc, points[index].x, points[index].y);
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
    draw_text(dc, rect, text, color, format | DT_SINGLELINE | DT_END_ELLIPSIS);
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
    const int right_padding = 10;
    const int requested_width = std::max<int>(static_cast<int>(top_size.cx), static_cast<int>(bottom_size.cx)) + 10;
    const int available_width = std::max<int>(0, rect.right - rect.left - right_padding - 2);
    const int label_width = std::clamp(requested_width, 48, std::max(48, available_width));
    const int label_height =
        std::max<int>(16, std::max<int>(static_cast<int>(top_size.cy), static_cast<int>(bottom_size.cy)) + 4);
    draw_graph_axis_label(dc,
                          RECT{rect.right - label_width - right_padding, rect.top + 4, rect.right - right_padding,
                               rect.top + 4 + label_height},
                          top_label, axis_text, DT_RIGHT | DT_TOP);
    draw_graph_axis_label(dc,
                          RECT{rect.right - label_width - right_padding, rect.bottom - label_height - 6,
                               rect.right - right_padding, rect.bottom - 6},
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
void append_focus_target(std::vector<FocusTarget>& targets, FocusTargetKind kind, RECT rect, int index = 0) {
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

}  // namespace

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
    case WM_PRINT:
    case WM_PRINTCLIENT: {
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        layout_and_draw(reinterpret_cast<HDC>(wparam), rect);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        if (wparam != 0U) {
            fill_rect(reinterpret_cast<HDC>(wparam), rect, kBg);
        }
        return 1;
    }
    case WM_MOUSEMOVE:
        return handle_mouse_move(lparam);
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
LRESULT MainWindow::handle_mouse_move(LPARAM lparam) {
    mouse_position_ = POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    mouse_inside_client_ = true;
    if (primary_mouse_down_ && queue_column_resize_separator_ >= 0) {
        update_queue_column_resize(mouse_position_.x);
    }
    if (!mouse_tracking_) {
        TRACKMOUSEEVENT event{};
        event.cbSize = sizeof(event);
        event.dwFlags = TME_LEAVE;
        event.hwndTrack = hwnd_;
        mouse_tracking_ = TrackMouseEvent(&event) != FALSE;
    }
    if (queue_column_resize_separator_ >= 0) {
        SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
    } else {
        Page page = Page::Queue;
        {
            std::lock_guard lock(mutex_);
            page = state_.page;
        }
        bool over_resize_grip = false;
        if (page == Page::Queue) {
            const auto layout = queue_layout(content_rect());
            const int header_bottom = layout.table.top + scale(36);
            if (mouse_position_.y >= layout.table.top && mouse_position_.y < header_bottom) {
                const RECT header_row{layout.table.left, layout.table.top, layout.table.right, header_bottom};
                const auto columns = queue_column_layout(layout.table, header_row);
                over_resize_grip = std::ranges::any_of(columns.resize_grips, [this](const RECT& grip) {
                    return contains_point(grip, mouse_position_.x, mouse_position_.y);
                });
            }
        }
        SetCursor(LoadCursor(nullptr, over_resize_grip ? IDC_SIZEWE : IDC_ARROW));
    }
    update_text_tooltip_tracking();
    request_repaint();
    return 0;
}

// Purpose: Clear pointer hover state after the cursor leaves the client area.
// Inputs: None; uses current capture state.
// Outputs: Updates mouse state and returns the handled Win32 result.
LRESULT MainWindow::handle_mouse_leave() {
    mouse_inside_client_ = false;
    if (!mouse_capture_active_) {
        primary_mouse_down_ = false;
    }
    mouse_tracking_ = false;
    KillTimer(hwnd_, kTextTooltipTimer);
    text_tooltip_cell_active_ = false;
    text_tooltip_visible_ = false;
    text_tooltip_text_.clear();
    request_repaint();
    return 0;
}

// Purpose: Decide whether a text value overflows the visible cell.
// Inputs: `text` is rendered in `cell` with `font`.
// Outputs: Returns true when the text would be ellipsized.
bool MainWindow::text_overflows_cell(std::wstring_view text, const RECT& cell, HFONT font) const {
    if (text.empty() || cell.right <= cell.left) {
        return false;
    }
    return text_width(text, font) > (cell.right - cell.left);
}

// Purpose: Measure text with an existing GDI font.
// Inputs: `text` is UTF-16 content and `font` is one of the window-owned fonts.
// Outputs: Returns the text width in physical pixels, or zero when measurement is unavailable.
int MainWindow::text_width(std::wstring_view text, HFONT font) const {
    if (text.empty() || font == nullptr) {
        return 0;
    }
    HDC dc = GetDC(hwnd_);
    if (dc == nullptr) {
        return 0;
    }
    HGDIOBJ previous = SelectObject(dc, font);
    SIZE size{};
    const BOOL measured =
        GetTextExtentPoint32W(dc, text.data(), static_cast<int>(std::min<std::size_t>(text.size(), INT_MAX)), &size);
    SelectObject(dc, previous);
    ReleaseDC(hwnd_, dc);
    return measured == FALSE ? 0 : size.cx;
}

// Purpose: Return the ellipsized text under the current mouse, if any.
// Inputs: None; uses current page layout, queue/form state, and selected fonts.
// Outputs: Returns true with cell/text set only for eligible truncated text targets.
bool MainWindow::text_tooltip_candidate_at_mouse(RECT& cell, std::wstring& text) {
    if (!mouse_inside_client_ || primary_mouse_down_) {
        return false;
    }
    UiState state;
    {
        std::lock_guard lock(mutex_);
        state = state_;
    }
    return queue_text_tooltip_candidate_at_mouse(state, cell, text) ||
           field_text_tooltip_candidate_at_mouse(state, cell, text);
}

// Purpose: Return the ellipsized Queue text under the current mouse, if any.
// Inputs: `state` is a stable UI snapshot and `cell`/`text` receive tooltip data.
// Outputs: Returns true only for truncated Queue Name or Path cells.
bool MainWindow::queue_text_tooltip_candidate_at_mouse(const UiState& state, RECT& cell, std::wstring& text) {
    if (state.page != Page::Queue || state.queued_paths.empty()) {
        return false;
    }
    const auto layout = queue_layout(content_rect());
    const int header_bottom = layout.table.top + scale(36);
    const int row_height = scale(34);
    if (row_height <= 0 || mouse_position_.y < header_bottom || mouse_position_.y >= layout.table.bottom) {
        return false;
    }
    const int row_index = (mouse_position_.y - header_bottom) / row_height;
    if (row_index < 0 || row_index >= static_cast<int>(state.queued_paths.size())) {
        return false;
    }
    const RECT row{layout.table.left, header_bottom + (row_index * row_height), layout.table.right,
                   header_bottom + ((row_index + 1) * row_height)};
    const auto columns = queue_column_layout(layout.table, row);
    const auto& path = state.queued_paths[static_cast<std::size_t>(row_index)];
    const RECT name_cell = inset_rect(columns.name, scale(8), 0);
    const RECT path_cell = inset_rect(columns.path, scale(8), 0);
    if (contains_point(name_cell, mouse_position_.x, mouse_position_.y)) {
        auto value = path.filename().wstring();
        if (text_overflows_cell(value, name_cell, tiny_font_)) {
            cell = name_cell;
            text = std::move(value);
            return true;
        }
    }
    if (contains_point(path_cell, mouse_position_.x, mouse_position_.y)) {
        auto value = path.wstring();
        if (text_overflows_cell(value, path_cell, tiny_font_)) {
            cell = path_cell;
            text = std::move(value);
            return true;
        }
    }
    return false;
}

// Purpose: Return the ellipsized Compress/Extract field text under the current mouse, if any.
// Inputs: `state` is a stable UI snapshot and `cell`/`text` receive tooltip data.
// Outputs: Returns true only for truncated Archive/Destination fields explicitly allowed by the UI contract.
bool MainWindow::field_text_tooltip_candidate_at_mouse(const UiState& state, RECT& cell, std::wstring& text) {
    const RECT content = content_rect();
    if (state.page == Page::Compress) {
        const auto layout = compress_layout(content);
        return tooltip_candidate_for_field(layout.archive_name, compression_output_filename_for(state), cell, text) ||
               tooltip_candidate_for_field(layout.destination, destination_directory_or_default(state).wstring(), cell,
                                           text);
    }
    if (state.page == Page::Extract) {
        const auto layout = extract_layout(content);
        const auto archive =
            state.queued_paths.empty() ? L"Select an archive from the queue" : state.queued_paths.front().wstring();
        return tooltip_candidate_for_field(layout.archive, archive, cell, text) ||
               tooltip_candidate_for_field(layout.destination, extraction_output_path_for(state).wstring(), cell, text);
    }
    return false;
}

// Purpose: Check one form field value for delayed tooltip eligibility.
// Inputs: `field` is the full labeled field rectangle, `value` is the visible text, and `cell`/`text` receive data.
// Outputs: Returns true only when the mouse is over a truncated value cell.
bool MainWindow::tooltip_candidate_for_field(const RECT& field, const std::wstring& value, RECT& cell,
                                             std::wstring& text) const {
    const RECT value_cell = field_value_cell(field, false);
    if (!contains_point(value_cell, mouse_position_.x, mouse_position_.y) ||
        !text_overflows_cell(value, value_cell, tiny_font_)) {
        return false;
    }
    cell = value_cell;
    text = value;
    return true;
}

// Purpose: Resolve the drawn value area inside a labeled field.
// Inputs: `field` is the full labeled field rectangle and `select` reserves space for a dropdown arrow when true.
// Outputs: Returns the value text rectangle used by both drawing and tooltip hit testing.
RECT MainWindow::field_value_cell(const RECT& field, bool select) const {
    const RECT box{field.left, field.top + scale(20), field.right, field.bottom};
    return RECT{box.left + scale(10), box.top, box.right - scale(select ? 30 : 10), box.bottom};
}

// Purpose: Update delayed text-tooltip tracking from the current mouse position.
// Inputs: None; reads current page, eligible text cells, and mouse coordinates.
// Outputs: Arms, hides, or preserves the tooltip timer based on ellipsized hover state.
void MainWindow::update_text_tooltip_tracking() {
    RECT candidate_cell{};
    std::wstring candidate_text;
    if (!text_tooltip_candidate_at_mouse(candidate_cell, candidate_text)) {
        KillTimer(hwnd_, kTextTooltipTimer);
        if (text_tooltip_cell_active_ || text_tooltip_visible_) {
            text_tooltip_cell_active_ = false;
            text_tooltip_visible_ = false;
            text_tooltip_text_.clear();
            request_repaint();
        }
        return;
    }
    const bool same_cell = text_tooltip_cell_active_ && EqualRect(&text_tooltip_cell_, &candidate_cell) != FALSE &&
                           text_tooltip_text_ == candidate_text;
    const bool same_point =
        text_tooltip_anchor_point_.x == mouse_position_.x && text_tooltip_anchor_point_.y == mouse_position_.y;
    if (same_cell && same_point) {
        return;
    }
    text_tooltip_cell_ = candidate_cell;
    text_tooltip_anchor_point_ = mouse_position_;
    text_tooltip_cell_active_ = true;
    text_tooltip_visible_ = false;
    text_tooltip_text_ = std::move(candidate_text);
    KillTimer(hwnd_, kTextTooltipTimer);
    SetTimer(hwnd_, kTextTooltipTimer, kTextTooltipDelayMs, nullptr);
    request_repaint();
}

// Purpose: Handle primary-button press using the same geometry as rendering.
// Inputs: `lparam` contains client-coordinate click position from `WM_LBUTTONDOWN`.
// Outputs: Updates capture/pressed state, dispatches page or content clicks, and returns the handled Win32 result.
LRESULT MainWindow::handle_primary_mouse_down(LPARAM lparam) {
    // Use the same scaled geometry for hit testing that the renderer uses, so
    // high-DPI displays do not create visual/click drift.
    const int x = GET_X_LPARAM(lparam);
    const int y = GET_Y_LPARAM(lparam);
    mouse_position_ = POINT{x, y};
    mouse_inside_client_ = true;
    primary_mouse_down_ = true;
    SetCapture(hwnd_);
    mouse_capture_active_ = true;
    request_repaint();

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

// Purpose: Handle primary-button release and trigger the shared command release pulse.
// Inputs: `lparam` contains client-coordinate release position from `WM_LBUTTONUP`.
// Outputs: Releases capture, updates mouse state, queues animation, and returns the handled Win32 result.
LRESULT MainWindow::handle_primary_mouse_up(LPARAM lparam) {
    mouse_position_ = POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    primary_mouse_down_ = false;
    if (mouse_capture_active_) {
        ReleaseCapture();
        mouse_capture_active_ = false;
    }
    end_queue_column_resize();
    if (mouse_inside_client_) {
        start_button_release_animation(mouse_position_);
    }
    request_repaint();
    return 0;
}

// Purpose: Normalize mouse state after Windows changes capture ownership.
// Inputs: None.
// Outputs: Clears pressed/capture flags and returns the handled Win32 result.
LRESULT MainWindow::handle_capture_changed() {
    primary_mouse_down_ = false;
    mouse_capture_active_ = false;
    end_queue_column_resize();
    request_repaint();
    return 0;
}

// Purpose: Append native shell-dropped paths to the queue.
// Inputs: `wparam` contains the HDROP handle from `WM_DROPFILES`.
// Outputs: Updates queue selection, releases the HDROP handle, and returns the handled Win32 result.
LRESULT MainWindow::handle_drop_files(WPARAM wparam) {
    // Native shell drag/drop is a queue regression boundary and is covered by
    // the GUI smoke harness with an injected HDROP payload.
    auto drop = reinterpret_cast<HDROP>(wparam);
    if (drop == nullptr) {
        return 0;
    }
    POINT drop_point{};
    DragQueryPoint(drop, &drop_point);
    try {
        (void)accept_dropped_paths(paths_from_hdrop(drop), drop_point);
    } catch (const std::exception& error) {
        DragFinish(drop);
        {
            std::lock_guard lock(mutex_);
            state_.status = std::string("Drop failed: ") + error.what();
        }
        append_log_entry(LogSeverity::Warning, "Shell drop failed");
        return 0;
    } catch (...) {
        DragFinish(drop);
        {
            std::lock_guard lock(mutex_);
            state_.status = "Drop failed";
        }
        append_log_entry(LogSeverity::Warning, "Shell drop failed");
        return 0;
    }
    DragFinish(drop);
    return 0;
}

// Purpose: Return whether a client point is inside the active Queue table drop target.
// Inputs: `point` is a client-coordinate point.
// Outputs: Returns true only on the Queue page and inside the Queue table.
bool MainWindow::queue_drop_target_contains(POINT point) {
    UiState state;
    {
        std::lock_guard lock(mutex_);
        state = state_;
    }
    if (state.page != Page::Queue) {
        return false;
    }
    return contains_point(queue_layout(content_rect()).table, point.x, point.y);
}

// Purpose: Append dropped paths to the Queue when the drop target is inside the Queue table.
// Inputs: `paths` are filesystem paths from shell drag/drop and `point` is the client drop coordinate.
// Outputs: Returns true and mutates queue state when the drop is accepted; otherwise reports rejection.
bool MainWindow::accept_dropped_paths(std::vector<std::filesystem::path> paths, POINT point) {
    set_queue_drop_highlight(false);
    LogSeverity severity = LogSeverity::Debug;
    std::string message;
    {
        std::lock_guard lock(mutex_);
        if (state_.page != Page::Queue || !contains_point(queue_layout(content_rect()).table, point.x, point.y)) {
            state_.status = "Drop files or folders inside the Queue box";
            severity = LogSeverity::Warning;
            message = "Shell drop rejected outside the Queue table";
        } else if (paths.empty()) {
            state_.status = "No drop items received";
            severity = LogSeverity::Warning;
            message = "Shell drop did not contain usable paths";
        } else {
            const bool was_empty = state_.queued_paths.empty();
            for (auto& path : paths) {
                state_.queued_paths.emplace_back(std::move(path));
                state_.queued_enabled.push_back(true);
            }
            normalize_queue_selection_locked();
            if (was_empty && !state_.queued_paths.empty()) {
                state_.selected_queue_index = 0;
            }
            state_.status = "Dropped items added";
            message = "Shell drop added " + std::to_string(paths.size()) + " queued item(s)";
        }
    }
    if (!message.empty()) {
        append_log_entry(severity, std::move(message));
        return severity != LogSeverity::Warning;
    }
    request_repaint();
    return true;
}

// Purpose: Update live drag/drop highlighting for the Queue table.
// Inputs: `active` describes whether a drag is over the allowed table drop target.
// Outputs: Updates visual drag state and queues a repaint when changed.
void MainWindow::set_queue_drop_highlight(bool active) {
    if (queue_drop_highlight_ == active) {
        return;
    }
    queue_drop_highlight_ = active;
    request_repaint();
}

// Purpose: Allow shell file-drop messages through UIPI for elevated windows.
// Inputs: None; applies only to the main HWND and only when the process is elevated.
// Outputs: Returns true when no extra filter is needed or the narrow filter was applied.
bool MainWindow::enable_elevated_drag_drop_messages() const {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return true;
    }
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL queried = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned) != FALSE;
    CloseHandle(token);
    if (!queried || elevation.TokenIsElevated == 0) {
        return true;
    }
    const BOOL drop_allowed = ChangeWindowMessageFilterEx(hwnd_, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    const BOOL copy_allowed = ChangeWindowMessageFilterEx(hwnd_, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    const BOOL query_allowed = ChangeWindowMessageFilterEx(hwnd_, kDragQueryMessage, MSGFLT_ALLOW, nullptr);
    return drop_allowed != FALSE && copy_allowed != FALSE && query_allowed != FALSE;
}

// Purpose: Initialize drag/drop, performance sampling, and smoke timers during window creation.
// Inputs: None.
// Outputs: Arms timers and returns the handled Win32 result.
LRESULT MainWindow::handle_create() {
    initialize_settings();
    DragAcceptFiles(hwnd_, TRUE);
    const HRESULT ole_status = OleInitialize(nullptr);
    ole_initialized_ = SUCCEEDED(ole_status);
    if (ole_initialized_) {
        drop_target_ = new QueueDropTarget(*this);
        if (RegisterDragDrop(hwnd_, drop_target_) != S_OK) {
            drop_target_->Release();
            drop_target_ = nullptr;
            append_log_entry(LogSeverity::Warning, "OLE Queue drag/drop registration could not be applied");
        }
    } else {
        append_log_entry(LogSeverity::Warning, "OLE Queue drag/drop initialization failed");
    }
    if (!enable_elevated_drag_drop_messages()) {
        {
            std::lock_guard lock(mutex_);
            state_.status = "Elevated drag/drop filter unavailable";
        }
        append_log_entry(LogSeverity::Warning, "Elevated shell drop message filter could not be applied");
    } else {
        append_log_entry(LogSeverity::Information, "Application initialized");
    }
    initialize_performance_monitor();
    update_performance_sample();
    reset_performance_timer(state_.performance_update_seconds);
    last_clock_text_ = current_user_time_text();
    SetTimer(hwnd_, kClockTimer, kClockPollMs, nullptr);
    if (const UINT auto_close_ms = smoke_auto_close_ms(); auto_close_ms > 0) {
        // Smoke-only auto-close prevents orphaned GUI windows if the harness
        // exits before it can post WM_CLOSE.
        SetTimer(hwnd_, kSmokeAutoCloseTimer, auto_close_ms, nullptr);
    }
    if (!smoke_close_marker_path().empty()) {
        SetTimer(hwnd_, kSmokeClosePollTimer, 250, nullptr);
    }
    return 0;
}

// Purpose: Load persisted per-user settings and publish the current applied snapshot.
// Inputs: None; reads the current user's Local AppData settings file when present.
// Outputs: Applies validated settings, creates defaults when needed, and logs nonfatal parse/write failures.
void MainWindow::initialize_settings() {
    try {
        applied_settings_ = read_settings_file(settings_file_path());
        {
            std::lock_guard lock(mutex_);
            apply_settings_to_state(applied_settings_, state_);
            state_.status = "Settings loaded";
        }
        reset_performance_timer(applied_settings_.performance_update_seconds);
        write_settings_file(settings_file_path(), applied_settings_);
    } catch (const std::exception& error) {
        {
            std::lock_guard lock(mutex_);
            applied_settings_ = settings_from_state(state_);
            state_.status = std::string("Settings unavailable: ") + error.what();
        }
        append_log_entry(LogSeverity::Warning, "Settings file could not be loaded");
    }
}

// Purpose: Save the current Settings draft as the applied snapshot.
// Inputs: None; reads Settings fields from synchronized UI state.
// Outputs: Writes the per-user config atomically and updates the applied snapshot.
void MainWindow::apply_settings() {
    AppSettings draft;
    {
        std::lock_guard lock(mutex_);
        draft = settings_from_state(state_);
    }
    write_settings_file(settings_file_path(), draft);
    applied_settings_ = draft;
    {
        std::lock_guard lock(mutex_);
        apply_settings_to_state(applied_settings_, state_);
        state_.status = "Settings applied";
    }
    reset_performance_timer(applied_settings_.performance_update_seconds);
    append_history_entry("Settings", "Settings", "Applied for current session", true);
    append_log_entry(LogSeverity::Information, "Settings applied for current session");
}

// Purpose: Revert Settings page controls to the last applied snapshot.
// Inputs: None.
// Outputs: Mutates Settings-owned UI fields and re-arms dependent timers.
void MainWindow::revert_settings_draft() {
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        changed = !settings_equal(settings_from_state(state_), applied_settings_);
        apply_settings_to_state(applied_settings_, state_);
        if (changed) {
            state_.status = "Unapplied settings reverted";
        }
    }
    reset_performance_timer(applied_settings_.performance_update_seconds);
    if (changed) {
        append_log_entry(LogSeverity::Debug, "Unapplied settings reverted after leaving Settings");
    } else {
        request_repaint();
    }
}

// Purpose: Reset Settings draft and applied snapshot to safe defaults.
// Inputs: None.
// Outputs: Updates UI, applied snapshot, and the persisted config file.
void MainWindow::reset_settings_to_defaults() {
    applied_settings_ = {};
    write_settings_file(settings_file_path(), applied_settings_);
    {
        std::lock_guard lock(mutex_);
        apply_settings_to_state(applied_settings_, state_);
        state_.destination_directory.clear();
        normalize_queue_selection_locked();
        state_.selected_queue_index = state_.queued_paths.empty() ? -1 : 0;
        state_.history_operation_filter_index = 0;
        state_.history_status_filter_index = 0;
        state_.status = "Defaults restored";
    }
    reset_performance_timer(applied_settings_.performance_update_seconds);
    append_log_entry(LogSeverity::Information, "Default settings restored");
}

// Purpose: Dispatch timers owned by the main window.
// Inputs: `wparam` identifies the timer from `WM_TIMER`.
// Outputs: Runs animation, monitoring, or smoke shutdown work and returns the handled Win32 result.
LRESULT MainWindow::handle_timer(WPARAM wparam) {
    if (wparam == kAnimationTimer) {
        tick_animation();
        return 0;
    }
    if (wparam == kPerformanceTimer) {
        update_performance_sample();
        request_repaint();
        return 0;
    }
    if (wparam == kProgressHoldTimer) {
        clear_expired_progress();
        return 0;
    }
    if (wparam == kClockTimer) {
        const auto current = current_user_time_text();
        if (current != last_clock_text_) {
            last_clock_text_ = current;
            request_repaint();
        }
        return 0;
    }
    if (wparam == kTextTooltipTimer) {
        KillTimer(hwnd_, kTextTooltipTimer);
        RECT candidate_cell{};
        std::wstring candidate_text;
        const bool still_hovered = text_tooltip_candidate_at_mouse(candidate_cell, candidate_text);
        const bool stationary =
            text_tooltip_anchor_point_.x == mouse_position_.x && text_tooltip_anchor_point_.y == mouse_position_.y;
        text_tooltip_visible_ = still_hovered && stationary && text_tooltip_cell_active_ &&
                                EqualRect(&text_tooltip_cell_, &candidate_cell) != FALSE &&
                                text_tooltip_text_ == candidate_text;
        if (!text_tooltip_visible_) {
            text_tooltip_cell_active_ = false;
            text_tooltip_text_.clear();
        }
        request_repaint();
        return 0;
    }
    if (wparam == kSmokeAutoCloseTimer) {
        DestroyWindow(hwnd_);
        return 0;
    }
    if (wparam == kSmokeClosePollTimer) {
        // The marker file gives the smoke harness a second shutdown path that
        // does not depend on external window activation.
        if (smoke_close_requested()) {
            DestroyWindow(hwnd_);
        }
        return 0;
    }
    return DefWindowProcW(hwnd_, WM_TIMER, wparam, 0);
}

// Purpose: Stop timers and release monitor state before window destruction completes.
// Inputs: None.
// Outputs: Posts quit and returns the handled Win32 result.
LRESULT MainWindow::handle_destroy() {
    // Kill every timer owned by this window before shutdown so repaint,
    // telemetry sampling, and smoke cleanup cannot outlive the HWND.
    KillTimer(hwnd_, kAnimationTimer);
    KillTimer(hwnd_, kPerformanceTimer);
    KillTimer(hwnd_, kProgressHoldTimer);
    KillTimer(hwnd_, kClockTimer);
    KillTimer(hwnd_, kTextTooltipTimer);
    KillTimer(hwnd_, kSmokeAutoCloseTimer);
    KillTimer(hwnd_, kSmokeClosePollTimer);
    if (drop_target_ != nullptr) {
        RevokeDragDrop(hwnd_);
        drop_target_->Release();
        drop_target_ = nullptr;
    }
    if (ole_initialized_) {
        OleUninitialize();
        ole_initialized_ = false;
    }
    shutdown_performance_monitor();
    PostQuitMessage(0);
    return 0;
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

// Purpose: Draw the current frame using DPI-scaled layout regions.
// Inputs: `dc` is the off-screen device context and `rect` is the client rectangle in physical pixels.
// Outputs: Writes the shell, navigation, active page, overlays, and status strip into `dc`.
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
    draw_tab_transition(dc, RECT{rect.left, content.top, rect.right, content.bottom});
    draw_active_dropdown(dc, content, state);
    draw_keyboard_focus(dc, content, state);
    draw_text_tooltip(dc);
    draw_status_bar(dc, status, state);
}

// Purpose: Return whether the mouse is currently over a live clickable target.
// Inputs: `rect` is the clickable target and `enabled` must be false for static or disabled UI.
// Outputs: Returns true only for enabled controls under the mouse pointer.
bool MainWindow::interactive_hovered(const RECT& rect, bool enabled) const {
    return enabled && mouse_inside_client_ && contains_point(rect, mouse_position_.x, mouse_position_.y);
}

// Purpose: Compute the subtle clickable hover fill used by all interactive boxes.
// Inputs: `base` is the non-hover fill, `rect` is the clickable target, `enabled` gates interaction, and `accent`
// selects command-button treatment.
// Outputs: Returns a slightly lifted background color without changing borders or behavior.
COLORREF MainWindow::interactive_fill(COLORREF base, const RECT& rect, bool enabled, bool accent) const {
    if (!interactive_hovered(rect, enabled)) {
        return base;
    }
    return blend_color(base, accent ? RGB(255, 74, 84) : RGB(63, 82, 89), accent ? 0.26 : 0.34);
}

// Purpose: Draw the shared hover background for row-like controls.
// Inputs: `dc` is the target, `rect` is the clickable row, and `enabled` gates interaction.
// Outputs: Adds only a subtle background lift when the row is hovered.
void MainWindow::draw_interactive_hover_surface(HDC dc, const RECT& rect, bool enabled) {
    if (!interactive_hovered(rect, enabled)) {
        return;
    }
    fill_round_rect(dc, rect, interactive_fill(kPanel, rect, enabled), scale(3));
}

// Purpose: Draw the delayed text tooltip when an eligible ellipsized value is hovered.
// Inputs: `dc` is the target for the current frame.
// Outputs: Renders a compact tooltip near the source cell without affecting layout.
void MainWindow::draw_text_tooltip(HDC dc) {
    if (!text_tooltip_visible_ || text_tooltip_text_.empty()) {
        return;
    }
    const RECT content = content_rect();
    const int max_width = scale(620);
    RECT measured{0, 0, max_width - scale(20), 0};
    SelectObject(dc, tiny_font_);
    DrawTextW(dc, text_tooltip_text_.data(), static_cast<int>(text_tooltip_text_.size()), &measured,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    const int tooltip_width =
        std::clamp<int>(static_cast<int>(measured.right - measured.left) + scale(20), scale(160), max_width);
    const int tooltip_height =
        std::clamp<int>(static_cast<int>(measured.bottom - measured.top) + scale(16), scale(34), scale(132));
    int left = std::clamp<int>(static_cast<int>(text_tooltip_cell_.left), static_cast<int>(content.left) + scale(8),
                               static_cast<int>(content.right) - tooltip_width - scale(8));
    int top = text_tooltip_cell_.bottom + scale(6);
    if (top + tooltip_height > content.bottom - scale(8)) {
        top = text_tooltip_cell_.top - tooltip_height - scale(6);
    }
    top = std::clamp<int>(top, static_cast<int>(content.top) + scale(8),
                          static_cast<int>(content.bottom) - tooltip_height - scale(8));
    const RECT tooltip{left, top, left + tooltip_width, top + tooltip_height};
    fill_round_rect(dc, tooltip, RGB(33, 45, 50), scale(4));
    stroke_rect(dc, tooltip, RGB(80, 99, 106));
    draw_text(dc, inset_rect(tooltip, scale(10), scale(7)), text_tooltip_text_, kText,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
}

// Purpose: Draw the keyboard focus affordance for the current target.
// Inputs: `dc` is the target, `content` is the content area, and `state` is the copied UI state.
// Outputs: Renders a minimal non-hover focus indicator without mutating state.
void MainWindow::draw_keyboard_focus(HDC dc, const RECT& content, const UiState& state) {
    const auto targets = focus_targets_for(content, state);
    if (targets.empty()) {
        return;
    }
    const int index = (keyboard_focus_index_ % static_cast<int>(targets.size()) + static_cast<int>(targets.size())) %
                      static_cast<int>(targets.size());
    if (targets[static_cast<std::size_t>(index)].kind == FocusTargetKind::Navigation) {
        return;
    }
    RECT focus = targets[static_cast<std::size_t>(index)].rect;
    focus = inset_rect(focus, scale(2), scale(2));
    HPEN pen = CreatePen(PS_SOLID, std::max(1, scale(1)), RGB(99, 130, 140));
    HGDIOBJ previous_pen = SelectObject(dc, pen);
    HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, focus.left, focus.top, focus.right, focus.bottom, scale(4), scale(4));
    SelectObject(dc, previous_brush);
    SelectObject(dc, previous_pen);
    DeleteObject(pen);
}

// Purpose: Draw the persistent product shell strip.
// Inputs: `dc` is the target and `rect` is the full client rectangle.
// Outputs: Renders the brand chrome; page-specific actions stay inside their pages.
void MainWindow::draw_top_bar(HDC dc, const RECT& rect) {
    fill_rect(dc, rect, kShell);
    draw_line(dc, rect.left, rect.bottom - 1, rect.right, rect.bottom - 1, kBorder);

    const int rail_width = scale(kRailWidth);
    RECT logo{scale(18), scale(14), scale(38), scale(36)};
    draw_logo(dc, logo, kAccent);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{scale(48), rect.top, rail_width + scale(76), rect.bottom}, L"SuperZip", kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// Purpose: Draw the compact navigation rail.
// Inputs: `dc` is the target, `rect` is the rail rectangle, and `state` is the copied UI state.
// Outputs: Renders all primary pages with active-page highlighting.
void MainWindow::draw_navigation(HDC dc, const RECT& rect, const UiState& state) {
    fill_rect(dc, rect, kRail);
    draw_line(dc, rect.right - 1, rect.top, rect.right - 1, rect.bottom, kBorder);
    const std::array<Page, 8> pages{
        Page::Queue,   Page::Compress, Page::Extract,  Page::Security,
        Page::History, Page::Gpu,      Page::Settings, Page::About,
    };
    SelectObject(dc, tiny_font_);
    const int item_height = scale(63);
    int y = rect.top + scale(10);
    for (const auto page : pages) {
        RECT item{rect.left, y, rect.right, y + item_height};
        const bool active = state.page == page;
        const bool hovered = mouse_inside_client_ && contains_point(item, mouse_position_.x, mouse_position_.y);
        const bool pressed = hovered && primary_mouse_down_;
        const RECT surface{item.left + scale(8), item.top + scale(5), item.right - scale(8), item.bottom - scale(5)};
        if (active) {
            fill_rect(dc, RECT{item.left, item.top, item.left + scale(5), item.bottom}, kAccent);
            fill_round_rect(dc, surface,
                            pressed   ? RGB(103, 20, 28)
                            : hovered ? RGB(148, 30, 39)
                                      : RGB(126, 24, 31),
                            scale(4));
        } else if (pressed) {
            fill_round_rect(dc, surface, RGB(31, 47, 53), scale(4));
            stroke_rect(dc, surface, RGB(72, 95, 103));
        } else if (hovered) {
            fill_round_rect(dc, surface, kPanel2, scale(4));
            stroke_rect(dc, surface, RGB(70, 91, 99));
        }
        const int icon_size = scale(30);
        const int icon_left = item.left + ((item.right - item.left) - icon_size) / 2;
        const int icon_top = item.top + ((item.bottom - item.top) - icon_size) / 2;
        RECT icon{icon_left, icon_top, icon_left + icon_size, icon_top + icon_size};
        if (pressed) {
            OffsetRect(&icon, scale(1), scale(1));
        }
        const COLORREF nav_color = active ? kText : hovered ? RGB(198, 211, 215) : kMuted;
        draw_nav_icon(dc, page, icon, nav_color);
        y += item_height;
    }
}

// Purpose: Draw the persistent AMD GPU, operation progress, and local-clock status strip.
// Inputs: `dc` is the target, `rect` is the status-strip rectangle, and `state` is the copied UI state.
// Outputs: Renders backend status, stable progress columns when active, and the user's locale-formatted time.
void MainWindow::draw_status_bar(HDC dc, const RECT& rect, const UiState& state) {
    fill_rect(dc, rect, kShell);
    draw_line(dc, rect.left, rect.top, rect.right, rect.top, kBorder);
    const bool ready = gpu_ready(state);
    const int cy = (rect.top + rect.bottom) / 2;
    HBRUSH dot = CreateSolidBrush(ready ? kOk : kDanger);
    HGDIOBJ previous = SelectObject(dc, dot);
    Ellipse(dc, rect.left + scale(18), cy - scale(5), rect.left + scale(28), cy + scale(5));
    SelectObject(dc, previous);
    DeleteObject(dot);

    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{rect.left + scale(36), rect.top, rect.left + scale(202), rect.bottom},
              ready ? L"AMD GPU ready" : L"AMD GPU unavailable", ready ? kOk : kDanger,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_line(dc, rect.left + scale(220), rect.top + scale(8), rect.left + scale(220), rect.bottom - scale(8), kBorder);
    draw_line(dc, rect.left + scale(450), rect.top + scale(8), rect.left + scale(450), rect.bottom - scale(8), kBorder);

    if (progress_visible(state)) {
        const RECT progress_rect{rect.left + scale(468), rect.top, rect.left + scale(610), rect.bottom};
        const RECT throughput_rect{rect.left + scale(632), rect.top, rect.left + scale(824), rect.bottom};
        const RECT remaining_rect{rect.left + scale(846), rect.top, rect.right - scale(kClockSegmentWidth + 20),
                                  rect.bottom};
        draw_text(dc, progress_rect, L"Progress: " + progress_percent_text(state.progress), kMuted,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        draw_text(dc, throughput_rect, L"Throughput: " + rate_text(state.progress.throughput_bytes_per_second), kMuted,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        draw_text(dc, remaining_rect, L"Time remaining: " + progress_time_remaining_text(state.progress), kMuted,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    draw_line(dc, rect.right - scale(kClockSegmentWidth), rect.top + scale(8), rect.right - scale(kClockSegmentWidth),
              rect.bottom - scale(8), kBorder);
    draw_text(dc, RECT{rect.right - scale(kClockSegmentWidth), rect.top, rect.right, rect.bottom},
              current_user_time_text(), kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// Purpose: Dispatch the active page renderer into the content region.
// Inputs: `dc` is the paint target, `rect` is the content bounds, and `state` is the copied UI state.
// Outputs: Draws exactly one active page without mutating UI state.
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
    case Page::Settings:
        draw_settings_page(dc, rect, state);
        break;
    case Page::About:
        draw_about_page(dc, rect);
        break;
    }
}

// Purpose: Compute Queue page rectangles shared by rendering and hit testing.
// Inputs: `rect` is the content area in physical pixels.
// Outputs: Returns DPI-scaled Queue page control rectangles.
MainWindow::QueueLayout MainWindow::queue_layout(const RECT& rect) const {
    QueueLayout layout{};
    layout.area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    const int header_top = layout.area.top;
    const int header_bottom = header_top + scale(kPageHeaderHeight);
    layout.clear = RECT{layout.area.right - scale(72), header_top, layout.area.right, header_bottom};
    layout.add_folder =
        RECT{layout.clear.left - scale(12) - scale(108), header_top, layout.clear.left - scale(12), header_bottom};
    layout.add_files = RECT{layout.add_folder.left - scale(12) - scale(96), header_top,
                            layout.add_folder.left - scale(12), header_bottom};
    layout.table = RECT{layout.area.left, layout.area.top + scale(56), layout.area.right, layout.area.bottom};
    return layout;
}

// Purpose: Return the shared bottom-right primary action button rectangle for operation pages.
// Inputs: `area` is the DPI-scaled page content area.
// Outputs: Returns a 110x36 design-pixel command rectangle aligned with Settings Apply.
RECT MainWindow::primary_action_rect(const RECT& area) const {
    return RECT{area.right - scale(110), area.bottom - scale(54), area.right, area.bottom - scale(18)};
}

// Purpose: Return the History Clear History button rectangle with Restore Defaults-equivalent visual margins.
// Inputs: `area` is the DPI-scaled page content area.
// Outputs: Returns a right-aligned command rectangle sized from the active button font.
RECT MainWindow::history_clear_button_rect(const RECT& area) const {
    const int restore_width = scale(134);
    const int restore_text = text_width(L"Restore Defaults", tiny_font_);
    const int clear_text = text_width(L"Clear History", tiny_font_);
    const int margin = std::max(scale(12), (restore_width - restore_text) / 2);
    const int width = std::clamp(clear_text + (margin * 2), scale(92), restore_width);
    return RECT{area.right - width, area.top, area.right, area.top + scale(34)};
}

// Purpose: Compute fixed checkbox and resizable data-column geometry for the Queue table.
// Inputs: `table` is the full table and `row` is the header or one body row.
// Outputs: Returns rectangles shared by rendering and hit testing.
MainWindow::QueueColumnLayout MainWindow::queue_column_layout(const RECT& table, const RECT& row) const {
    QueueColumnLayout columns{};
    const int checkbox_width = scale(kQueueCheckboxColumnWidth);
    const int available = std::max(scale(360), static_cast<int>(table.right - table.left) - checkbox_width - scale(22));
    const std::array<int, 4> minimums{scale(90), scale(70), scale(70), scale(120)};
    std::array<int, 4> widths{};
    int requested = 0;
    for (std::size_t i = 0; i < widths.size(); ++i) {
        widths[i] = std::max(minimums[i], scale(queue_column_widths_[i]));
        requested += widths[i];
    }
    if (requested != available && requested > 0) {
        const double ratio = static_cast<double>(available) / static_cast<double>(requested);
        int used = 0;
        for (std::size_t i = 0; i + 1U < widths.size(); ++i) {
            widths[i] = std::max(minimums[i], static_cast<int>(std::round(static_cast<double>(widths[i]) * ratio)));
            used += widths[i];
        }
        widths.back() = std::max(minimums.back(), available - used);
    }

    columns.header_checkbox = RECT{table.left + scale(10), row.top, table.left + checkbox_width - scale(8), row.bottom};
    columns.checkbox = columns.header_checkbox;
    int left = table.left + checkbox_width;
    columns.name = RECT{left, row.top, left + widths[0], row.bottom};
    left = columns.name.right;
    columns.size = RECT{left, row.top, left + widths[1], row.bottom};
    left = columns.size.right;
    columns.type = RECT{left, row.top, left + widths[2], row.bottom};
    left = columns.type.right;
    columns.path = RECT{left, row.top, table.right - scale(12), row.bottom};
    const int grip = scale(kQueueResizeGripHalfWidth);
    columns.resize_grips = {
        RECT{columns.name.right - grip, row.top, columns.name.right + grip, row.bottom},
        RECT{columns.size.right - grip, row.top, columns.size.right + grip, row.bottom},
        RECT{columns.type.right - grip, row.top, columns.type.right + grip, row.bottom},
    };
    return columns;
}

// Purpose: Compute Compress page rectangles shared by rendering and hit testing.
// Inputs: `rect` is the content area in physical pixels.
// Outputs: Returns DPI-scaled Compress page control rectangles.
MainWindow::CompressLayout MainWindow::compress_layout(const RECT& rect) const {
    CompressLayout layout{};
    layout.area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    layout.start = primary_action_rect(layout.area);
    const int left = layout.area.left;
    const int mid = layout.area.left + (layout.area.right - layout.area.left) / 2 + scale(14);
    const int field_w = (layout.area.right - layout.area.left) / 2 - scale(26);
    layout.archive_name = RECT{left, layout.area.top + scale(54), left + field_w, layout.area.top + scale(104)};
    layout.destination = RECT{mid, layout.area.top + scale(54), mid + field_w, layout.area.top + scale(104)};
    layout.format = RECT{left, layout.area.top + scale(124), left + field_w, layout.area.top + scale(174)};
    layout.compression_level = RECT{mid, layout.area.top + scale(124), mid + field_w, layout.area.top + scale(174)};
    layout.method = RECT{left, layout.area.top + scale(194), left + field_w, layout.area.top + scale(244)};
    layout.block_size = RECT{mid, layout.area.top + scale(194), mid + field_w, layout.area.top + scale(244)};
    layout.advanced = RECT{left, layout.area.top + scale(270), layout.area.right, layout.area.top + scale(390)};
    layout.solid_archive = RECT{layout.advanced.left + scale(18), layout.advanced.top + scale(48),
                                layout.advanced.left + scale(310), layout.advanced.top + scale(76)};
    layout.store_timestamps = RECT{layout.advanced.left + scale(18), layout.advanced.top + scale(80),
                                   layout.advanced.left + scale(310), layout.advanced.top + scale(108)};
    layout.delete_after_compression = RECT{layout.advanced.left + scale(342), layout.advanced.top + scale(48),
                                           layout.advanced.left + scale(680), layout.advanced.top + scale(76)};
    layout.verify = RECT{layout.advanced.left + scale(342), layout.advanced.top + scale(80),
                         layout.advanced.left + scale(710), layout.advanced.top + scale(108)};
    layout.security = RECT{left, layout.area.top + scale(410), layout.area.right, layout.area.top + scale(528)};
    layout.sha = RECT{layout.security.left + scale(18), layout.security.top + scale(46),
                      layout.security.left + scale(420), layout.security.top + scale(78)};
    layout.defender = RECT{layout.security.left + scale(18), layout.security.top + scale(82),
                           layout.security.left + scale(420), layout.security.top + scale(114)};
    return layout;
}

// Purpose: Compute Extract page rectangles shared by rendering and hit testing.
// Inputs: `rect` is the content area in physical pixels.
// Outputs: Returns DPI-scaled Extract page control rectangles.
MainWindow::ExtractLayout MainWindow::extract_layout(const RECT& rect) const {
    ExtractLayout layout{};
    layout.area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    layout.start = primary_action_rect(layout.area);
    const int left = layout.area.left;
    const int mid = layout.area.left + (layout.area.right - layout.area.left) / 2 + scale(14);
    const int field_w = (layout.area.right - layout.area.left) / 2 - scale(26);
    layout.archive = RECT{left, layout.area.top + scale(54), left + field_w, layout.area.top + scale(104)};
    layout.destination = RECT{mid, layout.area.top + scale(54), mid + field_w, layout.area.top + scale(104)};
    layout.path_mode = RECT{left, layout.area.top + scale(124), left + field_w, layout.area.top + scale(174)};
    layout.overwrite_policy = RECT{mid, layout.area.top + scale(124), mid + field_w, layout.area.top + scale(174)};
    layout.checks =
        RECT{layout.area.left, layout.area.top + scale(200), layout.area.right, layout.area.top + scale(332)};
    layout.verify_metadata = RECT{layout.checks.left + scale(18), layout.checks.top + scale(48),
                                  layout.checks.left + scale(420), layout.checks.top + scale(78)};
    layout.open_destination_after_extract = RECT{layout.checks.left + scale(18), layout.checks.top + scale(80),
                                                 layout.checks.left + scale(420), layout.checks.top + scale(110)};
    layout.sha = RECT{layout.checks.left + scale(470), layout.checks.top + scale(48), layout.checks.right - scale(20),
                      layout.checks.top + scale(80)};
    layout.defender = RECT{layout.checks.left + scale(470), layout.checks.top + scale(84),
                           layout.checks.right - scale(20), layout.checks.top + scale(116)};
    return layout;
}

// Purpose: Compute Settings page rectangles shared by rendering and hit testing.
// Inputs: `rect` is the content area in physical pixels.
// Outputs: Returns DPI-scaled Settings page control rectangles.
MainWindow::SettingsLayout MainWindow::settings_layout(const RECT& rect) const {
    SettingsLayout layout{};
    layout.area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    layout.restore_defaults = RECT{layout.area.right - scale(260), layout.area.bottom - scale(54),
                                   layout.area.right - scale(126), layout.area.bottom - scale(18)};
    layout.apply = primary_action_rect(layout.area);
    const int panel_top = layout.area.top + scale(54);
    const int panel_bottom = panel_top + scale(168);
    layout.general = RECT{layout.area.left, panel_top, layout.area.left + scale(470), panel_bottom};
    layout.security = RECT{layout.general.left, layout.general.bottom + scale(16), layout.general.right,
                           layout.general.bottom + scale(176)};
    layout.performance = RECT{layout.general.right + scale(18), layout.general.top, layout.area.right, panel_bottom};
    layout.logging = RECT{layout.performance.left, layout.performance.bottom + scale(16), layout.area.right,
                          layout.performance.bottom + scale(176)};
    layout.sha = RECT{layout.security.left + scale(18), layout.security.top + scale(48),
                      layout.security.right - scale(16), layout.security.top + scale(80)};
    layout.defender = RECT{layout.security.left + scale(18), layout.security.top + scale(84),
                           layout.security.right - scale(16), layout.security.top + scale(116)};
    layout.gpu = RECT{layout.security.left + scale(18), layout.security.top + scale(120),
                      layout.security.right - scale(16), layout.security.top + scale(152)};
    layout.verify = RECT{layout.performance.left + scale(18), layout.performance.top + scale(48),
                         layout.performance.right - scale(18), layout.performance.top + scale(80)};
    const int performance_half_right =
        layout.performance.left + (layout.performance.right - layout.performance.left) / 2;
    const int logging_half_right = layout.logging.left + (layout.logging.right - layout.logging.left) / 2;
    layout.memory_policy = RECT{layout.performance.left + scale(18), layout.performance.top + scale(94),
                                performance_half_right, layout.performance.top + scale(140)};
    layout.log_level = RECT{layout.logging.left + scale(18), layout.logging.top + scale(48), logging_half_right,
                            layout.logging.top + scale(94)};
    layout.log_retention = RECT{layout.logging.left + scale(18), layout.logging.top + scale(106), logging_half_right,
                                layout.logging.top + scale(152)};
    layout.open_destination_after_operation = RECT{layout.general.left + scale(18), layout.general.top + scale(48),
                                                   layout.general.right - scale(16), layout.general.top + scale(78)};
    layout.confirm_before_deleting = RECT{layout.general.left + scale(18), layout.general.top + scale(82),
                                          layout.general.right - scale(16), layout.general.top + scale(112)};
    layout.show_operation_summary = RECT{layout.general.left + scale(18), layout.general.top + scale(116),
                                         layout.general.right - scale(16), layout.general.top + scale(146)};
    return layout;
}

// Purpose: Draw the queue page with the file/folder selection table only.
// Inputs: `dc` is the target, `rect` is the content area, and `state` is copied UI state.
// Outputs: Renders queue selection controls; operation configuration remains on later pages.
void MainWindow::draw_queue_page(HDC dc, const RECT& rect, const UiState& state) {
    const auto layout = queue_layout(rect);
    const RECT area = layout.area;
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, layout.add_files.left - scale(18), area.top + scale(kPageTitleTextHeight)},
              L"Queue", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_button(dc, layout.add_files, L"+ Add files", false);
    draw_button(dc, layout.add_folder, L"+ Add folder", false);
    draw_button(dc, layout.clear, L"Clear", false);
    SelectObject(dc, tiny_font_);
    const auto count_text =
        std::to_wstring(state.queued_paths.size()) + L" item" + (state.queued_paths.size() == 1 ? L"" : L"s");
    draw_text(dc,
              RECT{layout.add_files.left - scale(120), area.top, layout.add_files.left - scale(18),
                   area.top + scale(kPageHeaderHeight)},
              count_text, kMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    RECT table = layout.table;
    const COLORREF table_fill = queue_drop_highlight_ ? blend_color(kPanel, kInfo, 0.16) : kPanel;
    fill_round_rect(dc, table, table_fill, scale(4));
    stroke_rect(dc, table, queue_drop_highlight_ ? RGB(70, 116, 130) : kBorder);
    SelectObject(dc, tiny_font_);
    const int header_bottom = table.top + scale(36);
    const RECT header_row{table.left, table.top, table.right, header_bottom};
    const RECT header_band{table.left + scale(1), table.top + scale(1), table.right - scale(1), header_bottom};
    fill_rect(dc, header_band, blend_color(kPanel, kPanel2, 0.52));
    const auto header_columns = queue_column_layout(table, header_row);
    const bool has_entries = !state.queued_paths.empty();
    const bool all_enabled =
        has_entries && state.queued_enabled.size() >= state.queued_paths.size() &&
        std::all_of(state.queued_enabled.begin(),
                    state.queued_enabled.begin() + static_cast<std::ptrdiff_t>(state.queued_paths.size()),
                    [](bool enabled) { return enabled; });
    draw_checkbox(dc, header_columns.header_checkbox, L"", has_entries && all_enabled, has_entries);
    draw_text(dc, inset_rect(header_columns.name, scale(8), 0), L"Name", kMuted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw_text(dc, inset_rect(header_columns.size, scale(8), 0), L"Size", kMuted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw_text(dc, inset_rect(header_columns.type, scale(8), 0), L"Type", kMuted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw_text(dc, inset_rect(header_columns.path, scale(8), 0), L"Path", kMuted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    for (const auto& grip : header_columns.resize_grips) {
        draw_line(dc, (grip.left + grip.right) / 2, table.top + scale(8), (grip.left + grip.right) / 2,
                  header_bottom - scale(8), kBorder);
    }
    draw_line(dc, table.left + scale(1), header_bottom - scale(1), table.right - scale(1), header_bottom - scale(1),
              RGB(73, 95, 102));
    draw_line(dc, table.left + scale(1), header_bottom, table.right - scale(1), header_bottom, RGB(9, 15, 18));

    int y = header_bottom;
    if (state.queued_paths.empty()) {
        SelectObject(dc, body_font_);
        const RECT empty_drop_zone{table.left + scale(40), table.top + scale(36), table.right - scale(40),
                                   table.bottom - scale(36)};
        draw_text(dc, empty_drop_zone, L"Drag & drop files or folders here, or use the Add files / Add folder buttons.",
                  queue_drop_highlight_ ? kText : kMuted,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else {
        int row_index = 0;
        for (const auto& path : state.queued_paths) {
            const int row_bottom = y + scale(34);
            const bool selected = row_index == state.selected_queue_index;
            const RECT row_rect{table.left, y, table.right, row_bottom};
            const auto columns = queue_column_layout(table, row_rect);
            const bool enabled = static_cast<std::size_t>(row_index) >= state.queued_enabled.size()
                                     ? true
                                     : state.queued_enabled[static_cast<std::size_t>(row_index)];
            const COLORREF base_row_fill = selected ? kPanel3 : (((y / scale(34)) % 2 == 0) ? kPanel2 : kPanel);
            const COLORREF row_fill = interactive_fill(base_row_fill, row_rect);
            fill_rect(dc, RECT{table.left + scale(1), y + scale(1), table.right - scale(1), row_bottom}, row_fill);
            draw_checkbox(dc, columns.checkbox, L"", enabled);
            draw_text(dc, inset_rect(columns.name, scale(8), 0), path.filename().wstring(), kText,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            draw_text(dc, inset_rect(columns.size, scale(8), 0), entry_size_text(path), kMuted,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            draw_text(dc, inset_rect(columns.type, scale(8), 0), entry_type_text(path), kMuted,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            draw_text(dc, inset_rect(columns.path, scale(8), 0), path.wstring(), kMuted,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y = row_bottom;
            ++row_index;
            if (y > table.bottom - scale(34)) {
                break;
            }
        }
    }
}

// Purpose: Draw the compression settings page.
// Inputs: `dc` is the target, `rect` is the content area, and `state` contains opt-in settings.
// Outputs: Renders archive destination, format, compression level, advanced options, integrity toggles, and start
// control.
void MainWindow::draw_compress_page(HDC dc, const RECT& rect, const UiState& state) {
    const auto layout = compress_layout(rect);
    const RECT area = layout.area;
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(kPageTitleTextHeight)}, L"Compress", kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_button(dc, layout.start, L"Start", true);
    draw_operation_progress_bar(dc,
                                RECT{layout.start.left, layout.start.bottom + scale(4), layout.start.right,
                                     layout.start.bottom + scale(4) + scale(kOperationProgressHeight)},
                                state, OperationKind::Compress);

    const auto format = compression_format_value(state.compression_format_index);
    const bool suzip_tuning = compression_format_uses_suzip_tuning(format);
    const bool level_tuning = compression_format_uses_level(format);
    draw_field(dc, layout.archive_name, L"Archive name", compression_output_filename_for(state), false);
    draw_field(dc, layout.destination, L"Destination", destination_directory_or_default(state).wstring(), false, true,
               true);
    draw_field(dc, layout.format, L"Archive format", compression_format_text(state.compression_format_index), true);
    draw_field(dc, layout.compression_level, L"Compression level",
               level_tuning ? compression_level_text(state.compression_level_index) : L"-", level_tuning, level_tuning);
    draw_field(dc, layout.method, L"Compression method",
               suzip_tuning ? (state.gpu_required ? L"AMD HIP required" : L"AMD HIP preferred") : L"-", suzip_tuning,
               suzip_tuning);
    draw_field(dc, layout.block_size, L"Block size",
               suzip_tuning ? compression_block_size_text(state.compression_block_size_index) : L"-", suzip_tuning,
               suzip_tuning);

    RECT advanced = layout.advanced;
    fill_round_rect(dc, advanced, kPanel, scale(4));
    stroke_rect(dc, advanced, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{advanced.left + scale(16), advanced.top + scale(12), advanced.right, advanced.top + scale(36)},
              L"Advanced", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_toggle(dc, layout.solid_archive, L"Solid archive", state.solid_archive, ToggleId::SolidArchive);
    draw_toggle(dc, layout.store_timestamps, L"Store timestamps", state.store_timestamps, ToggleId::StoreTimestamps);
    draw_toggle(dc, layout.delete_after_compression, L"Delete files after compression", state.delete_after_compression,
                ToggleId::DeleteAfterCompression);
    draw_toggle(dc, layout.verify, L"Verify archive after write", state.verify_after_write_opt_in,
                ToggleId::VerifyAfterWrite);

    RECT security = layout.security;
    fill_round_rect(dc, security, kPanel, scale(4));
    stroke_rect(dc, security, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{security.left + scale(16), security.top + scale(12), security.right, security.top + scale(36)},
              L"Integrity and Security", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_toggle(dc, layout.sha, L"SHA-256 integrity check", state.integrity_hash_opt_in, ToggleId::IntegrityHash);
    draw_toggle(dc, layout.defender, L"Microsoft Defender scan", state.defender_scan_opt_in, ToggleId::DefenderScan);
}

// Purpose: Draw the extraction settings page.
// Inputs: `dc` is the target, `rect` is the content area, and `state` contains opt-in settings.
// Outputs: Renders archive/destination fields, overwrite policy, integrity toggles, and start control.
void MainWindow::draw_extract_page(HDC dc, const RECT& rect, const UiState& state) {
    const auto layout = extract_layout(rect);
    const RECT area = layout.area;
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(kPageTitleTextHeight)}, L"Extract", kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_button(dc, layout.start, L"Start", true);
    draw_operation_progress_bar(dc,
                                RECT{layout.start.left, layout.start.bottom + scale(4), layout.start.right,
                                     layout.start.bottom + scale(4) + scale(kOperationProgressHeight)},
                                state, OperationKind::Extract);

    const auto archive =
        state.queued_paths.empty() ? L"Select an archive from the queue" : state.queued_paths.front().wstring();
    draw_field(dc, layout.archive, L"Archive", archive, false, true, false,
               state.queued_paths.empty() ? kMuted : CLR_INVALID);
    draw_field(dc, layout.destination, L"Destination", extraction_output_path_for(state).wstring(), false, true, true);
    draw_field(dc, layout.path_mode, L"Archive format",
               state.queued_paths.empty() ? L"-" : detected_archive_format_text(state.queued_paths), false,
               !state.queued_paths.empty());
    draw_field(dc, layout.overwrite_policy, L"Overwrite policy",
               state.overwrite ? L"Overwrite without asking" : L"Ask before overwriting", true);

    RECT checks = layout.checks;
    fill_round_rect(dc, checks, kPanel, scale(4));
    stroke_rect(dc, checks, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{checks.left + scale(16), checks.top + scale(12), checks.right, checks.top + scale(36)},
              L"Integrity and Security", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_toggle(dc, layout.verify_metadata, L"Verify archive metadata before extraction",
                state.verify_metadata_before_extract, ToggleId::VerifyMetadata);
    draw_toggle(dc, layout.open_destination_after_extract, L"Open destination folder after extraction",
                state.open_destination_after_extract, ToggleId::OpenDestinationAfterExtract);
    draw_toggle(dc, layout.sha, L"SHA-256 integrity check", state.integrity_hash_opt_in, ToggleId::IntegrityHash);
    draw_toggle(dc, layout.defender, L"Microsoft Defender scan", state.defender_scan_opt_in, ToggleId::DefenderScan);
}

// Purpose: Draw the security review page.
// Inputs: `dc` is the target, `rect` is the content area, and `state` contains security choices.
// Outputs: Renders path, CRC, integrity, Defender, and overwrite checks with explicit status.
void MainWindow::draw_security_page(HDC dc, const RECT& rect, const UiState& state) {
    RECT area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(kPageTitleTextHeight)}, L"Security", kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const RECT verify_button = primary_action_rect(area);
    draw_button(dc, verify_button, L"Verify", true);
    draw_operation_progress_bar(dc,
                                RECT{verify_button.left, verify_button.bottom + scale(4), verify_button.right,
                                     verify_button.bottom + scale(4) + scale(kOperationProgressHeight)},
                                state, OperationKind::Verify);

    RECT summary{area.left, area.top + scale(54), area.left + scale(430), area.bottom - scale(80)};
    fill_round_rect(dc, summary, kPanel, scale(4));
    stroke_rect(dc, summary, kBorder);
    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{summary.left + scale(16), summary.top, summary.left + scale(210), summary.top + scale(36)},
              L"Check", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{summary.left + scale(230), summary.top, summary.right - scale(16), summary.top + scale(36)},
              L"Status", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_line(dc, summary.left, summary.top + scale(36), summary.right, summary.top + scale(36), kBorder);
    struct Row {
        const wchar_t* name;
        const wchar_t* status;
        COLORREF color;
    };
    const Row rows[] = {
        {L"Path safety", L"Safe", kOk},
        {L"CRC metadata", L"Verified", kOk},
        {L"Post-write verify", state.verify_after_write_opt_in ? L"Selected" : L"Not selected",
         state.verify_after_write_opt_in ? kOk : kWarn},
        {L"SHA-256 optional", state.integrity_hash_opt_in ? L"Selected" : L"Not selected",
         state.integrity_hash_opt_in ? kOk : kWarn},
        {L"Defender optional", state.defender_scan_opt_in ? L"Selected" : L"Not selected",
         state.defender_scan_opt_in ? kOk : kWarn},
        {L"Overwrite policy", state.overwrite ? L"Overwrite without asking" : L"Ask before overwriting",
         state.overwrite ? kWarn : kOk},
        {L"GPU requirement", state.gpu_required ? L"AMD HIP required" : L"Fallback allowed",
         state.gpu_required ? kInfo : kWarn},
    };
    int y = summary.top + scale(36);
    for (const auto& row : rows) {
        const int bottom = y + scale(38);
        fill_rect(dc, RECT{summary.left + scale(1), y + scale(1), summary.right - scale(1), bottom},
                  ((y / scale(38)) % 2 == 0) ? kPanel2 : kPanel);
        draw_text(dc, RECT{summary.left + scale(16), y, summary.left + scale(210), bottom}, row.name, kText,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        draw_text(dc, RECT{summary.left + scale(230), y, summary.right - scale(16), bottom}, row.status, row.color,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y = bottom;
    }

    RECT detail{summary.right + scale(22), summary.top, area.right, summary.bottom};
    fill_round_rect(dc, detail, kPanel, scale(4));
    stroke_rect(dc, detail, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{detail.left + scale(18), detail.top + scale(14), detail.right, detail.top + scale(40)},
              L"Archive boundary contract", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_text(
        dc, RECT{detail.left + scale(18), detail.top + scale(56), detail.right - scale(18), detail.top + scale(154)},
        L"Extraction publishes final files only after each archive entry is normalized, decoded, and verified inside "
        L"the selected destination. Absolute paths, drive-rooted paths, UNC paths, traversal, unsafe names, malformed "
        L"block metadata, CRC mismatches, and overwrite attempts are rejected before final publication.",
        kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    draw_field(
        dc, RECT{detail.left + scale(18), detail.top + scale(184), detail.right - scale(18), detail.top + scale(234)},
        L"Archive",
        state.queued_paths.empty() ? L"Select an archive from the queue" : state.queued_paths.front().wstring(), false,
        true, false, state.queued_paths.empty() ? kMuted : CLR_INVALID);
    draw_field(
        dc, RECT{detail.left + scale(18), detail.top + scale(252), detail.left + scale(248), detail.top + scale(302)},
        L"Files", std::to_wstring(state.queued_paths.size()), false);
    draw_field(
        dc, RECT{detail.left + scale(270), detail.top + scale(252), detail.right - scale(18), detail.top + scale(302)},
        L"Total size", L"Calculated during job", false);
}

// Purpose: Draw the operation history page.
// Inputs: `dc` is the target, `rect` is the content area, and `state` contains session history.
// Outputs: Renders filters, history rows, and selected-operation details.
void MainWindow::draw_history_page(HDC dc, const RECT& rect, const UiState& state) {
    RECT area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(kPageTitleTextHeight)}, L"History", kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_button(dc, history_clear_button_rect(area), L"Clear History", false);
    draw_field(dc, RECT{area.left, area.top + scale(48), area.left + scale(220), area.top + scale(92)}, L"Operation",
               history_operation_filter_text(state.history_operation_filter_index), true);
    draw_field(dc, RECT{area.left + scale(238), area.top + scale(48), area.left + scale(458), area.top + scale(92)},
               L"Status", history_status_filter_text(state.history_status_filter_index), true);

    RECT table{area.left, area.top + scale(112), area.right, area.bottom - scale(96)};
    fill_round_rect(dc, table, kPanel, scale(4));
    stroke_rect(dc, table, kBorder);
    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{table.left + scale(14), table.top, table.left + scale(150), table.top + scale(34)}, L"Time",
              kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{table.left + scale(160), table.top, table.left + scale(290), table.top + scale(34)},
              L"Operation", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{table.left + scale(300), table.top, table.left + scale(620), table.top + scale(34)}, L"Archive",
              kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{table.left + scale(632), table.top, table.right - scale(16), table.top + scale(34)}, L"Status",
              kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_line(dc, table.left, table.top + scale(34), table.right, table.top + scale(34), kBorder);
    int y = table.top + scale(34);
    if (state.history.empty()) {
        draw_text(dc, RECT{table.left + scale(18), y + scale(28), table.right - scale(18), y + scale(70)},
                  L"No completed operations in this session yet.", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else {
        bool rendered_any = false;
        for (const auto& entry : state.history) {
            const bool operation_match = state.history_operation_filter_index == 0 ||
                                         (state.history_operation_filter_index == 1 && entry.operation == "Compress") ||
                                         (state.history_operation_filter_index == 2 && entry.operation == "Extract") ||
                                         (state.history_operation_filter_index == 3 && entry.operation == "Security");
            const bool status_match = state.history_status_filter_index == 0 ||
                                      (state.history_status_filter_index == 1 && entry.success) ||
                                      (state.history_status_filter_index == 2 && !entry.success);
            if (!operation_match || !status_match) {
                continue;
            }
            const int bottom = y + scale(34);
            draw_text(dc, RECT{table.left + scale(14), y, table.left + scale(150), bottom}, L"Session", kMuted,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            draw_text(dc, RECT{table.left + scale(160), y, table.left + scale(290), bottom}, widen(entry.operation),
                      kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            draw_text(dc, RECT{table.left + scale(300), y, table.left + scale(620), bottom}, widen(entry.subject),
                      kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            draw_text(dc, RECT{table.left + scale(632), y, table.right - scale(16), bottom},
                      entry.success ? L"Success" : L"Failure", entry.success ? kOk : kDanger,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            y = bottom;
            rendered_any = true;
            if (y > table.bottom - scale(34)) {
                break;
            }
        }
        if (!rendered_any) {
            draw_text(dc, RECT{table.left + scale(18), y + scale(28), table.right - scale(18), y + scale(70)},
                      L"No operations match the current filters.", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
    }
    RECT details{area.left, area.bottom - scale(76), area.right, area.bottom};
    fill_round_rect(dc, details, kPanel, scale(4));
    stroke_rect(dc, details, kBorder);
    draw_text(dc, RECT{details.left + scale(18), details.top + scale(10), details.right - scale(18), details.bottom},
              L"Selected operation details appear here after a job runs.", kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

// Purpose: Draw the System page with current runtime and resource-status details.
// Inputs: `dc` is the double-buffered paint target, `rect` is the page bounds, and `state` supplies live GPU/status
// text. Outputs: Renders system diagnostics controls and informational panels without mutating state.
void MainWindow::draw_gpu_page(HDC dc, const RECT& rect, const UiState& state) {
    RECT area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(kPageTitleTextHeight)}, L"System", kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const bool ready = gpu_ready(state);
    RECT top{area.left, area.top + scale(54), area.right, area.top + scale(142)};
    fill_round_rect(dc, top, kPanel, scale(4));
    stroke_rect(dc, top, kBorder);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{top.left + scale(18), top.top + scale(12), top.left + scale(180), top.top + scale(40)},
              L"HIP status", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{top.left + scale(190), top.top + scale(12), top.right - scale(18), top.top + scale(40)},
              widen(state.gpu_status), ready ? kOk : kWarn, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{top.left + scale(18), top.top + scale(50), top.right - scale(18), top.bottom - scale(12)},
              ready ? L"SuperZip will use AMD HIP for native .suzip jobs that require GPU acceleration."
                    : L"This build or host is not reporting an active AMD HIP device. GPU-required jobs fail instead "
                      L"of silently using a different vendor path.",
              kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);

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
    draw_text(dc, RECT{gpu.left + scale(16), gpu.top + scale(12), gpu.right, gpu.top + scale(36)}, L"GPU", kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{memory.left + scale(16), memory.top + scale(12), memory.right, memory.top + scale(36)}, L"RAM",
              kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{accel.left + scale(16), accel.top + scale(12), accel.right, accel.top + scale(36)},
              L"Acceleration", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    const std::wstring gpu_detail =
        ready ? (std::wstring(L"Backend: AMD HIP\nDevice: ") +
                 (state.gpu_device_name.empty() ? L"Detected by HIP runtime" : widen(state.gpu_device_name)) +
                 L"\nArchitecture: " + (state.gpu_arch.empty() ? L"Runtime default" : widen(state.gpu_arch)))
              : L"Backend unavailable\nNo CUDA/WebGPU fallback\nHost stays AMD-only";
    draw_text(dc, RECT{gpu.left + scale(16), gpu.top + scale(48), gpu.right - scale(16), gpu.bottom - scale(14)},
              gpu_detail, kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    draw_text(
        dc, RECT{memory.left + scale(16), memory.top + scale(48), memory.right - scale(16), memory.bottom - scale(14)},
        L"Bounded chunks keep archive work from loading whole archives into RAM. Archive and compression adapters "
        L"cover ZIP, TAR filters, Gzip, Bzip2, Zstandard, Unix Compress, CAB, RPM, XZ, and LZMA; legacy transfer "
        L"decoders remain extract-only and path-validated.",
        kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    draw_text(dc,
              RECT{accel.left + scale(16), accel.top + scale(48), accel.right - scale(16), accel.bottom - scale(14)},
              state.gpu_required ? L"Mode: GPU required\nFallback: blocked for .suzip jobs\nDevice scope: AMD HIP only"
                                 : L"Mode: GPU preferred\nFallback: CPU codec allowed\nDevice scope: AMD HIP only",
              kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);

    draw_performance_monitor(dc, RECT{area.left, area.top + scale(342), area.right, area.bottom}, state);
}

// Purpose: Draw one metric card inside the live performance monitor.
// Inputs: `dc` is the target; text/value fields are preformatted; `history` contains normalized samples.
// Outputs: Renders a bordered Task Manager-style history graph without overflowing text.
void MainWindow::draw_performance_monitor_card(HDC dc, const RECT& graph, const wchar_t* label,
                                               const std::wstring& value, const std::wstring& detail,
                                               std::span<const double> history, COLORREF color,
                                               const std::wstring& graph_top_label,
                                               const std::wstring& graph_bottom_label) {
    fill_round_rect(dc, graph, kPanel2, scale(4));
    stroke_rect(dc, graph, kBorder);
    RECT label_rect{graph.left + scale(12), graph.top + scale(8), graph.right - scale(12), graph.top + scale(30)};
    RECT value_rect{graph.left + scale(12), graph.top + scale(30), graph.right - scale(12), graph.top + scale(60)};
    RECT plot{graph.left + scale(12), graph.top + scale(70), graph.right - scale(12), graph.bottom - scale(58)};
    RECT detail_rect{graph.left + scale(12), graph.bottom - scale(50), graph.right - scale(12),
                     graph.bottom - scale(8)};
    SelectObject(dc, small_font_);
    draw_text(dc, label_rect, label, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, body_font_);
    draw_text(dc, value_rect, value, color, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, tiny_font_);
    draw_graph_grid(dc, plot);
    const RECT graph_area = inset_rect(plot, scale(1), scale(1));
    draw_graph_series(dc, graph_area, history, color);
    draw_graph_axis_labels(dc, graph_area, graph_top_label, graph_bottom_label);
    draw_text(dc, detail_rect, detail, kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
}

// Purpose: Draw one paired metric card inside the live performance monitor.
// Inputs: `dc` is the target; primary and secondary histories are normalized samples.
// Outputs: Renders a shared-grid dual-line history graph without overflowing text.
void MainWindow::draw_dual_performance_monitor_card(HDC dc, const RECT& graph, const wchar_t* label,
                                                    const std::wstring& value, const std::wstring& detail,
                                                    std::span<const double> primary_history,
                                                    std::span<const double> secondary_history, COLORREF primary,
                                                    COLORREF secondary, const std::wstring& graph_top_label,
                                                    const std::wstring& graph_bottom_label) {
    fill_round_rect(dc, graph, kPanel2, scale(4));
    stroke_rect(dc, graph, kBorder);
    RECT label_rect{graph.left + scale(12), graph.top + scale(8), graph.right - scale(12), graph.top + scale(30)};
    RECT value_rect{graph.left + scale(12), graph.top + scale(30), graph.right - scale(12), graph.top + scale(60)};
    RECT plot{graph.left + scale(12), graph.top + scale(70), graph.right - scale(12), graph.bottom - scale(58)};
    RECT detail_rect{graph.left + scale(12), graph.bottom - scale(50), graph.right - scale(12),
                     graph.bottom - scale(8)};
    SelectObject(dc, small_font_);
    draw_text(dc, label_rect, label, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, body_font_);
    draw_text(dc, value_rect, value, kWarn, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, tiny_font_);
    draw_graph_grid(dc, plot);
    const RECT inset_plot = inset_rect(plot, scale(1), scale(1));
    draw_graph_series(dc, inset_plot, primary_history, primary);
    draw_graph_series(dc, inset_plot, secondary_history, secondary);
    draw_graph_axis_labels(dc, inset_plot, graph_top_label, graph_bottom_label);
    draw_text(dc, detail_rect, detail, kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
}

// Purpose: Return the four live-monitor card rectangles shared by rendering and hit testing.
// Inputs: `monitor` is the complete Performance Monitor panel.
// Outputs: Returns CPU, RAM, I/O, and GPU card rectangles in display order.
std::array<RECT, 4> MainWindow::performance_monitor_card_rects(const RECT& monitor) const {
    const int graph_top = monitor.top + scale(58);
    const int graph_bottom = monitor.bottom - scale(18);
    const int graph_w = (monitor.right - monitor.left - scale(86)) / 4;
    std::array<RECT, 4> cards{};
    for (int i = 0; i < 4; ++i) {
        cards[static_cast<std::size_t>(i)] =
            RECT{monitor.left + scale(18) + i * (graph_w + scale(16)), graph_top,
                 monitor.left + scale(18) + i * (graph_w + scale(16)) + graph_w, graph_bottom};
    }
    return cards;
}

// Purpose: Return the System update-speed field rectangle aligned to the GPU card.
// Inputs: `monitor` is the complete Performance Monitor panel.
// Outputs: Returns a narrow same-row field rectangle whose right edge matches the GPU card.
RECT MainWindow::performance_update_speed_rect(const RECT& monitor) const {
    const auto cards = performance_monitor_card_rects(monitor);
    const RECT gpu_card = cards.back();
    const int box_w = scale(kPerformanceUpdateFieldWidth);
    return RECT{gpu_card.right - box_w, monitor.top + scale(10), gpu_card.right, monitor.top + scale(40)};
}

// Purpose: Draw the live performance monitor section on the System page.
// Inputs: `dc` is the target, `monitor` is the panel rectangle, and `state` contains the latest counters.
// Outputs: Renders CPU, total RAM, process read/write I/O, and VRAM history cards.
void MainWindow::draw_performance_monitor(HDC dc, const RECT& monitor, const UiState& state) {
    const auto& sample = state.performance;
    fill_round_rect(dc, monitor, kPanel, scale(4));
    stroke_rect(dc, monitor, kBorder);
    SelectObject(dc, small_font_);
    const RECT update_speed = performance_update_speed_rect(monitor);
    draw_text(
        dc,
        RECT{monitor.left + scale(16), monitor.top + scale(12), update_speed.left - scale(18), monitor.top + scale(36)},
        L"Performance Monitor", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_text(
        dc, RECT{update_speed.left - scale(116), update_speed.top, update_speed.left - scale(10), update_speed.bottom},
        L"Refresh interval", kMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    draw_field(dc, RECT{update_speed.left, update_speed.top - scale(20), update_speed.right, update_speed.bottom}, L"",
               performance_update_speed_text(state.performance_update_seconds), true);

    std::array<double, 96> cpu{};
    std::array<double, 96> memory{};
    std::array<double, 96> read{};
    std::array<double, 96> write{};
    std::array<double, 96> vram{};
    const auto sample_at = [this](std::size_t index) -> const PerformanceMonitorSample& {
        const auto start = performance_history_next_ + performance_history_.size() - performance_history_count_;
        return performance_history_[(start + index) % performance_history_.size()];
    };
    for (std::size_t i = 0; i < performance_history_count_; ++i) {
        const auto& item = sample_at(i);
        cpu[i] = item.cpu_percent / 100.0;
        memory[i] = item.system_memory_percent / 100.0;
        read[i] = std::min(1.0, item.io_read_bytes_per_second / (1024.0 * 1024.0 * 1024.0));
        write[i] = std::min(1.0, item.io_write_bytes_per_second / (1024.0 * 1024.0 * 1024.0));
        vram[i] = item.vram_total_bytes == 0U ? 0.0
                                              : static_cast<double>(item.vram_total_bytes - item.vram_free_bytes) /
                                                    static_cast<double>(item.vram_total_bytes);
    }

    const auto cards = performance_monitor_card_rects(monitor);
    const std::span<const double> cpu_span(cpu.data(), performance_history_count_);
    const std::span<const double> memory_span(memory.data(), performance_history_count_);
    const std::span<const double> read_span(read.data(), performance_history_count_);
    const std::span<const double> write_span(write.data(), performance_history_count_);
    const std::span<const double> vram_span(vram.data(), performance_history_count_);
    const std::wstring percent_top = L"100%";
    const std::wstring zero_percent = L"0%";
    const std::wstring io_top = L"1 GiB/s";
    const std::wstring zero_gib_rate = L"0 GiB/s";
    for (int i = 0; i < 4; ++i) {
        RECT card = cards[static_cast<std::size_t>(i)];
        if (!sample.live) {
            draw_performance_monitor_card(dc, card,
                                          i == 0   ? L"CPU"
                                          : i == 1 ? L"RAM"
                                          : i == 2 ? L"I/O"
                                                   : L"GPU",
                                          L"Collecting", L"Waiting for first sample", std::span<const double>{},
                                          kSubtle, percent_top, zero_percent);
        } else if (i == 0) {
            const auto detail = std::wstring(L"CPU used (total): ") + percentage_text(sample.cpu_percent) +
                                L"\nCPU used (dedicated): " + percentage_text(sample.process_cpu_percent);
            draw_performance_monitor_card(dc, card, L"CPU", percentage_text(sample.cpu_percent), detail, cpu_span,
                                          kInfo, percent_top, zero_percent);
        } else if (i == 1) {
            const auto value = percentage_text(sample.system_memory_percent);
            const auto detail = std::wstring(L"RAM used (total): ") +
                                widen(human_bytes(static_cast<double>(sample.system_memory_used_bytes))) + L" / " +
                                widen(human_bytes(static_cast<double>(sample.system_memory_total_bytes))) +
                                L"\nRAM used (dedicated): " +
                                widen(human_bytes(static_cast<double>(sample.private_bytes)));
            draw_performance_monitor_card(dc, card, L"RAM", value, detail, memory_span, kOk, percent_top, zero_percent);
        } else if (i == 2) {
            const double total_io = sample.io_read_bytes_per_second + sample.io_write_bytes_per_second;
            const auto detail = std::wstring(L"Read: ") + rate_text(sample.io_read_bytes_per_second) + L"\nWrite: " +
                                rate_text(sample.io_write_bytes_per_second);
            draw_dual_performance_monitor_card(dc, card, L"I/O", rate_text(total_io), detail, read_span, write_span,
                                               kInfo, kWarn, io_top, zero_gib_rate);
        } else {
            const bool has_vram = sample.vram_total_bytes > 0U;
            const auto used = sample.vram_total_bytes - sample.vram_free_bytes;
            const double vram_percent =
                has_vram ? (static_cast<double>(used) / static_cast<double>(sample.vram_total_bytes)) * 100.0 : 0.0;
            const auto value = has_vram ? percentage_text(vram_percent) : L"Unavailable";
            const auto detail =
                has_vram ? std::wstring(L"VRAM used (total): ") + widen(human_bytes(static_cast<double>(used))) +
                               L" / " + widen(human_bytes(static_cast<double>(sample.vram_total_bytes))) +
                               L"\nVRAM used (dedicated): " +
                               widen(human_bytes(static_cast<double>(sample.process_dedicated_vram_bytes)))
                         : L"HIP VRAM unavailable\nGPU memory counter unavailable";
            draw_performance_monitor_card(dc, card, L"GPU", value, detail, vram_span, kAccent, percent_top,
                                          zero_percent);
        }
    }
}

// Purpose: Draw the preferences page.
// Inputs: `dc` is the target, `rect` is the content area, and `state` contains toggled defaults.
// Outputs: Renders general, security, performance, and logging settings.
void MainWindow::draw_settings_page(HDC dc, const RECT& rect, const UiState& state) {
    const auto layout = settings_layout(rect);
    RECT area = layout.area;
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(kPageTitleTextHeight)}, L"Settings", kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
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
    draw_text(dc, RECT{general.left + scale(16), general.top + scale(12), general.right, general.top + scale(36)},
              L"General", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{security.left + scale(16), security.top + scale(12), security.right, security.top + scale(36)},
              L"Security", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(
        dc,
        RECT{performance.left + scale(16), performance.top + scale(12), performance.right, performance.top + scale(36)},
        L"Performance", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{logging.left + scale(16), logging.top + scale(12), logging.right, logging.top + scale(36)},
              L"Logging", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_toggle(dc, layout.open_destination_after_operation, L"Open destination folder after operation",
                state.open_destination_after_operation, ToggleId::OpenDestinationAfterOperation);
    draw_toggle(dc, layout.confirm_before_deleting, L"Confirm before deleting files", state.confirm_before_deleting,
                ToggleId::ConfirmBeforeDeleting);
    draw_toggle(dc, layout.show_operation_summary, L"Show operation summary", state.show_operation_summary,
                ToggleId::ShowOperationSummary);
    draw_toggle(dc, layout.sha, L"SHA-256 integrity check", state.integrity_hash_opt_in, ToggleId::IntegrityHash);
    draw_toggle(dc, layout.defender, L"Microsoft Defender scan", state.defender_scan_opt_in, ToggleId::DefenderScan);
    draw_toggle(dc, layout.gpu, L"Require AMD GPU acceleration", state.gpu_required, ToggleId::GpuRequired);
    draw_toggle(dc, layout.verify, L"Verify archive after write", state.verify_after_write_opt_in,
                ToggleId::VerifyAfterWrite);
    draw_field(dc, layout.memory_policy, L"Memory policy", memory_policy_text(state.memory_policy_index), true);
    draw_field(dc, layout.log_level, L"Log level", log_level_text(state.log_level_index), true);
    draw_field(dc, layout.log_retention, L"Log retention", log_retention_text(state.log_retention_index), true);
}

// Purpose: Render the About page brand, version, and compatibility boundary summary.
// Inputs: `dc` is the target device context and `rect` is the DPI-scaled page bounds.
// Outputs: Draws into `dc`; does not mutate archive state or perform I/O.
void MainWindow::draw_about_page(HDC dc, const RECT& rect) {
    RECT area = inset_rect(rect, scale(kPageInsetX), scale(kPageInsetY));
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{area.left, area.top, area.right, area.top + scale(kPageTitleTextHeight)}, L"About", kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT card{area.left, area.top + scale(54), area.right, area.bottom - scale(60)};
    fill_round_rect(dc, card, kPanel, scale(4));
    stroke_rect(dc, card, kBorder);
    RECT logo{card.left + scale(42), card.top + scale(56), card.left + scale(112), card.top + scale(126)};
    draw_logo(dc, logo, kAccent);
    SelectObject(dc, title_font_);
    draw_text(dc, RECT{card.left + scale(142), card.top + scale(50), card.right - scale(40), card.top + scale(90)},
              L"SuperZip", kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, small_font_);
    draw_text(dc, RECT{card.left + scale(142), card.top + scale(94), card.right - scale(40), card.top + scale(122)},
              std::wstring(kProductTagline), kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{card.left + scale(142), card.top + scale(130), card.right - scale(40), card.top + scale(150)},
              L"Author: Efstratios Mitridis", kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, RECT{card.left + scale(142), card.top + scale(146), card.right - scale(40), card.top + scale(166)},
              widen(std::string("Version: ") + SUPERZIP_VERSION), kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(
        dc, RECT{card.left + scale(42), card.top + scale(184), card.right - scale(42), card.top + scale(294)},
        L"SuperZip separates native .suzip GPU archive jobs from ZIP, TAR, compressed TAR/CPIO, Gzip, Bzip2, XZ, "
        L"LZMA, lzip, Zstandard, Unix Compress, CAB, 7z, LHA/LZH, CPIO, AR, DEB, ISO, and RPM standard "
        L"archive/compression handling. Legacy transfer decoders remain extract-only. AMD HIP is the only GPU "
        L"acceleration boundary; security-sensitive extraction validates paths and metadata before writing output.",
        kText, DT_LEFT | DT_TOP | DT_WORDBREAK);
    draw_text(dc, RECT{card.left + scale(42), card.bottom - scale(80), card.right - scale(42), card.bottom - scale(38)},
              L"Built for 64-bit Windows, high-DPI displays, and responsive background archive work.", kMuted,
              DT_LEFT | DT_TOP | DT_WORDBREAK);
}

// Purpose: Draw a simple DPI-scaled command or navigation button.
// Inputs: `dc` is the target, `rect` is the button rectangle, `text` is display text, and `active` selects accent
// styling. Outputs: Renders hover, press, release, border, and ellipsized text states.
void MainWindow::draw_button(HDC dc, const RECT& rect, const wchar_t* text, bool active) {
    const bool hovered = mouse_inside_client_ && contains_point(rect, mouse_position_.x, mouse_position_.y);
    const bool pressed = hovered && primary_mouse_down_;
    const double release_progress = button_release_progress(rect);
    const COLORREF base_fill = active ? kAccent : kPanel2;
    const COLORREF pressed_fill = active ? RGB(144, 22, 31) : RGB(38, 54, 60);
    const COLORREF fill = pressed ? pressed_fill : interactive_fill(base_fill, rect, true, active);
    const COLORREF border = active ? (pressed ? RGB(111, 18, 26) : kAccent2) : kBorder;
    fill_round_rect(dc, rect, fill, scale(4));
    stroke_rect(dc, rect, border);
    if (release_progress < 1.0) {
        const double eased = ease_out(release_progress);
        const int inset = static_cast<int>(std::round(static_cast<double>(scale(7)) * eased));
        const COLORREF pulse = blend_color(active ? RGB(255, 123, 131) : RGB(126, 151, 159), fill, eased);
        stroke_rect(dc, inset_rect(rect, std::max(1, inset), std::max(1, inset)), pulse);
    }
    SelectObject(dc, tiny_font_);
    RECT label = inset_rect(rect, scale(12), 0);
    if (pressed) {
        OffsetRect(&label, scale(1), scale(1));
    }
    draw_text(dc, label, text, kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// Purpose: Draw a slim progress indicator for the matching active operation.
// Inputs: `dc` is the target, `rect` is the fixed bar slot, `state` is copied UI state, and `operation` selects a tab.
// Outputs: Renders no pixels unless the active progress snapshot matches the requested operation.
void MainWindow::draw_operation_progress_bar(HDC dc, const RECT& rect, const UiState& state, OperationKind operation) {
    if (state.progress.operation != operation || !progress_bar_active(state)) {
        return;
    }
    const double ratio = std::clamp(progress_ratio(state.progress), 0.02, 1.0);
    fill_round_rect(dc, rect, RGB(34, 50, 56), scale(4));
    RECT fill = rect;
    fill.right = fill.left + static_cast<int>(std::round(static_cast<double>(fill.right - fill.left) * ratio));
    if (fill.right > fill.left) {
        fill_round_rect(dc, fill, operation == OperationKind::Verify ? kInfo : kAccent, scale(4));
    }
}

// Purpose: Draw a DPI-scaled opt-in settings toggle row.
// Inputs: `dc` is the target, `rect` is the row rectangle, `text` is display text, and `enabled` selects checked
// styling. Outputs: Renders the toggle and label into `dc`.
void MainWindow::draw_toggle(HDC dc, const RECT& rect, const wchar_t* text, bool enabled, ToggleId id) {
    draw_interactive_hover_surface(dc, rect, true);
    draw_text(dc, RECT{rect.left + scale(54), rect.top, rect.right, rect.bottom}, text, kText,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const int cy = (rect.top + rect.bottom) / 2;
    const int track_height = scale(18);
    RECT track{rect.left, cy - track_height / 2, rect.left + scale(42), cy + track_height / 2};
    const double position = toggle_visual_position(id, enabled);
    fill_round_rect(dc, track, blend_color(RGB(57, 69, 75), RGB(43, 111, 72), position), scale(14));
    HBRUSH knob = CreateSolidBrush(blend_color(kMuted, kOk, position));
    HGDIOBJ previous = SelectObject(dc, knob);
    const int knob_size = scale(14);
    const int knob_travel = (track.right - track.left) - knob_size - scale(6);
    const int knob_left =
        track.left + scale(3) + static_cast<int>(std::round(position * static_cast<double>(std::max(0, knob_travel))));
    Ellipse(dc, knob_left, cy - knob_size / 2, knob_left + knob_size, cy + knob_size / 2);
    SelectObject(dc, previous);
    DeleteObject(knob);
}

// Purpose: Draw a DPI-scaled checkbox row.
// Inputs: `dc`, `rect`, `text`, `checked`, and `interactive` describe the visual state.
// Outputs: Renders a checkbox and label into `dc`.
void MainWindow::draw_checkbox(HDC dc, const RECT& rect, const wchar_t* text, bool checked, bool interactive) {
    draw_interactive_hover_surface(dc, rect, interactive);
    const int box_size = scale(16);
    const int cy = rect.top + ((rect.bottom - rect.top) / 2);
    RECT box{rect.left, cy - (box_size / 2), rect.left + box_size, cy - (box_size / 2) + box_size};
    const COLORREF fill = checked ? kAccent : (interactive ? interactive_fill(kPanel2, rect, true) : kPanel);
    const COLORREF stroke = checked ? kAccent2 : (interactive ? kBorder : RGB(42, 52, 56));
    const COLORREF text_color = interactive ? kText : kMuted;
    fill_rect(dc, box, fill);
    stroke_rect(dc, box, stroke);
    if (checked) {
        draw_line(dc, box.left + scale(3), box.top + scale(8), box.left + scale(7), box.bottom - scale(4), kText);
        draw_line(dc, box.left + scale(7), box.bottom - scale(4), box.right - scale(3), box.top + scale(4), kText);
    }
    if (text != nullptr && text[0] != L'\0') {
        draw_text(dc, RECT{rect.left + scale(26), rect.top, rect.right, rect.bottom}, text, text_color,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

// Purpose: Draw a form field or select-style value box.
// Inputs: `dc` is the target, `rect` is the box, `label` names the field, `value` is display text, `select` adds an
// affordance, `enabled` controls disabled styling, and `clickable` enables hover without a menu arrow. Outputs: Renders
// label and bordered field with ellipsized value.
void MainWindow::draw_field(HDC dc, const RECT& rect, const wchar_t* label, const std::wstring& value, bool select,
                            bool enabled, bool clickable, COLORREF value_color_override) {
    SelectObject(dc, tiny_font_);
    draw_text(dc, RECT{rect.left, rect.top, rect.right, rect.top + scale(18)}, label, enabled ? kMuted : kSubtle,
              DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT box{rect.left, rect.top + scale(20), rect.right, rect.bottom};
    const COLORREF base = enabled ? kPanel2 : RGB(20, 28, 31);
    fill_round_rect(dc, box, interactive_fill(base, box, enabled && (select || clickable)), scale(3));
    stroke_rect(dc, box, enabled ? kBorder : RGB(39, 50, 55));
    const COLORREF value_color =
        value_color_override == CLR_INVALID ? (enabled ? kText : kSubtle) : value_color_override;
    draw_text(dc, field_value_cell(rect, select), value, value_color,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (select && enabled) {
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

// Purpose: Draw the currently expanded select/dropdown menu.
// Inputs: `dc` is the target, `content` is the active content area, and `state` is copied UI state.
// Outputs: Renders the active dropdown menu using the same row height as hit testing.
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

    const int row_height = scale(options.size() > 10U ? 28 : 32);
    const int selected = dropdown_selected_index(state, state.active_dropdown);
    const int keyboard_selected =
        dropdown_keyboard_index_ >= 0 && dropdown_keyboard_index_ < static_cast<int>(options.size())
            ? dropdown_keyboard_index_
            : selected;
    SelectObject(dc, tiny_font_);
    for (int index = 0; index < static_cast<int>(options.size()); ++index) {
        const RECT row{menu.left + scale(1), menu.top + scale(1) + (index * row_height), menu.right - scale(1),
                       menu.top + scale(1) + ((index + 1) * row_height)};
        const bool is_selected = index == selected;
        const bool is_keyboard_selected = index == keyboard_selected;
        const COLORREF base = is_selected ? RGB(126, 24, 31) : ((index % 2 == 0) ? kPanel2 : kPanel);
        const COLORREF keyed = is_keyboard_selected && !is_selected ? RGB(46, 63, 70) : base;
        fill_rect(dc, row, interactive_fill(keyed, row, true, is_selected));
        if (index > 0) {
            draw_line(dc, menu.left + scale(8), row.top, menu.right - scale(8), row.top, kBorder);
        }
        if (is_selected) {
            const int check_mid = (row.top + row.bottom) / 2;
            draw_line(dc, row.left + scale(12), check_mid, row.left + scale(17), check_mid + scale(5), kText);
            draw_line(dc, row.left + scale(17), check_mid + scale(5), row.left + scale(27), check_mid - scale(6),
                      kText);
        }
        draw_text(dc, RECT{row.left + scale(36), row.top, row.right - scale(12), row.bottom},
                  options[static_cast<std::size_t>(index)], is_selected ? kText : kMuted,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

// Purpose: Resolve the field rectangle that owns a dropdown menu.
// Inputs: `id` identifies the dropdown and `content` is the current page content rectangle.
// Outputs: Returns a DPI-scaled anchor rectangle or an empty rectangle for inactive IDs.
RECT MainWindow::dropdown_anchor_rect(DropdownId id, const RECT& content) const {
    switch (id) {
    case DropdownId::CompressFormat:
        return compress_layout(content).format;
    case DropdownId::CompressLevel:
        return compress_layout(content).compression_level;
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
    case DropdownId::GpuUpdateSpeed: {
        const RECT area = inset_rect(content, scale(kPageInsetX), scale(kPageInsetY));
        const RECT monitor{area.left, area.top + scale(342), area.right, area.bottom};
        return performance_update_speed_rect(monitor);
    }
    case DropdownId::SettingsMemoryPolicy:
        return settings_layout(content).memory_policy;
    case DropdownId::SettingsLogLevel:
        return settings_layout(content).log_level;
    case DropdownId::SettingsLogRetention:
        return settings_layout(content).log_retention;
    case DropdownId::None:
        return RECT{};
    }
    return RECT{};
}

// Purpose: Resolve the overlay menu rectangle for a dropdown.
// Inputs: `id` identifies the dropdown and `content` is the current content rectangle.
// Outputs: Returns a DPI-scaled menu rectangle positioned inside the content area.
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
    const int row_height = scale(options.size() > 10U ? 28 : 32);
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

// Purpose: Draw the permanent top accent rule for the content surface.
// Inputs: `dc` is the target and `rect` is the content surface rectangle.
// Outputs: Renders a stable accent line without page-change animation.
void MainWindow::draw_tab_transition(HDC dc, const RECT& rect) {
    fill_rect(dc, RECT{rect.left, rect.top, rect.right, rect.top + scale(2)}, kAccent);
}

RECT MainWindow::content_rect() const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int top_bar = scale(kTopBar);
    const int rail_width = scale(kRailWidth);
    const int status_bar = scale(kStatusBar);
    return RECT{rail_width, top_bar, client.right, client.bottom - status_bar};
}

// Purpose: Return keyboard-focusable controls for the current page.
// Inputs: `content` is the current content rectangle and `state` is a copied UI snapshot.
// Outputs: Returns controls in Tab order using the same geometry as mouse hit testing.
std::vector<FocusTarget> MainWindow::focus_targets_for(const RECT& content, const UiState& state) const {
    std::vector<FocusTarget> targets;
    targets.reserve(32);
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int item_height = scale(63);
    const int nav_top = scale(kTopBar) + scale(10);
    for (int index = 0; index < 8; ++index) {
        append_focus_target(targets, FocusTargetKind::Navigation,
                            RECT{client.left, nav_top + (index * item_height), client.left + scale(kRailWidth),
                                 nav_top + ((index + 1) * item_height)},
                            index);
    }
    switch (state.page) {
    case Page::Queue:
        add_queue_focus_targets(targets, content, state);
        break;
    case Page::Compress:
        add_compress_focus_targets(targets, content, state);
        break;
    case Page::Extract:
        add_extract_focus_targets(targets, content);
        break;
    case Page::Security: {
        RECT area = inset_rect(content, scale(kPageInsetX), scale(kPageInsetY));
        append_focus_target(targets, FocusTargetKind::SecurityVerify, primary_action_rect(area));
    } break;
    case Page::History: {
        RECT area = inset_rect(content, scale(kPageInsetX), scale(kPageInsetY));
        append_focus_target(targets, FocusTargetKind::HistoryOperation,
                            RECT{area.left, area.top + scale(48), area.left + scale(220), area.top + scale(92)});
        append_focus_target(
            targets, FocusTargetKind::HistoryStatus,
            RECT{area.left + scale(238), area.top + scale(48), area.left + scale(458), area.top + scale(92)});
        append_focus_target(targets, FocusTargetKind::HistoryClear, history_clear_button_rect(area));
    } break;
    case Page::Gpu:
        append_focus_target(targets, FocusTargetKind::SystemUpdateSpeed,
                            dropdown_anchor_rect(DropdownId::GpuUpdateSpeed, content));
        break;
    case Page::Settings:
        add_settings_focus_targets(targets, content);
        break;
    case Page::About:
        break;
    }
    return targets;
}

// Purpose: Append all Queue page keyboard-focus targets.
// Inputs: `targets` receives controls, `content` is the content rectangle, and `state` is a copied UI snapshot.
// Outputs: Adds Queue action buttons, header tick, and visible row targets.
void MainWindow::add_queue_focus_targets(std::vector<FocusTarget>& targets, const RECT& content,
                                         const UiState& state) const {
    const auto layout = queue_layout(content);
    append_focus_target(targets, FocusTargetKind::QueueAddFiles, layout.add_files);
    append_focus_target(targets, FocusTargetKind::QueueAddFolder, layout.add_folder);
    append_focus_target(targets, FocusTargetKind::QueueClear, layout.clear);
    if (state.queued_paths.empty()) {
        return;
    }
    const int header_bottom = layout.table.top + scale(36);
    const RECT header_row{layout.table.left, layout.table.top, layout.table.right, header_bottom};
    append_focus_target(targets, FocusTargetKind::QueueHeaderCheckbox,
                        queue_column_layout(layout.table, header_row).header_checkbox);
    const int row_height = scale(34);
    const int visible_rows = row_height <= 0 ? 0 : (layout.table.bottom - header_bottom) / row_height;
    const int row_count = std::min(static_cast<int>(state.queued_paths.size()), std::max(0, visible_rows));
    for (int index = 0; index < row_count; ++index) {
        const int top = header_bottom + (index * row_height);
        append_focus_target(targets, FocusTargetKind::QueueRow,
                            RECT{layout.table.left, top, layout.table.right, top + row_height}, index);
    }
}

// Purpose: Append all Compress page keyboard-focus targets.
// Inputs: `targets` receives controls, `content` is the content rectangle, and `state` is a copied UI snapshot.
// Outputs: Adds command, destination, format, tuning, and toggle targets that are currently enabled.
void MainWindow::add_compress_focus_targets(std::vector<FocusTarget>& targets, const RECT& content,
                                            const UiState& state) const {
    const auto layout = compress_layout(content);
    const auto format = compression_format_value(state.compression_format_index);
    append_focus_target(targets, FocusTargetKind::CompressStart, layout.start);
    append_focus_target(targets, FocusTargetKind::CompressDestination, layout.destination);
    append_focus_target(targets, FocusTargetKind::CompressFormat, layout.format);
    if (compression_format_uses_level(format)) {
        append_focus_target(targets, FocusTargetKind::CompressLevel, layout.compression_level);
    }
    if (compression_format_uses_suzip_tuning(format)) {
        append_focus_target(targets, FocusTargetKind::CompressMethod, layout.method);
        append_focus_target(targets, FocusTargetKind::CompressBlockSize, layout.block_size);
    }
    append_focus_target(targets, FocusTargetKind::CompressSolidArchive, layout.solid_archive);
    append_focus_target(targets, FocusTargetKind::CompressStoreTimestamps, layout.store_timestamps);
    append_focus_target(targets, FocusTargetKind::CompressDeleteAfterCompression, layout.delete_after_compression);
    append_focus_target(targets, FocusTargetKind::CompressVerifyAfterWrite, layout.verify);
    append_focus_target(targets, FocusTargetKind::CompressIntegrityHash, layout.sha);
    append_focus_target(targets, FocusTargetKind::CompressDefenderScan, layout.defender);
}

// Purpose: Append all Extract page keyboard-focus targets.
// Inputs: `targets` receives controls and `content` is the content rectangle.
// Outputs: Adds command, destination, overwrite, and integrity/security toggles.
void MainWindow::add_extract_focus_targets(std::vector<FocusTarget>& targets, const RECT& content) const {
    const auto layout = extract_layout(content);
    append_focus_target(targets, FocusTargetKind::ExtractStart, layout.start);
    append_focus_target(targets, FocusTargetKind::ExtractDestination, layout.destination);
    append_focus_target(targets, FocusTargetKind::ExtractOverwrite, layout.overwrite_policy);
    append_focus_target(targets, FocusTargetKind::ExtractVerifyMetadata, layout.verify_metadata);
    append_focus_target(targets, FocusTargetKind::ExtractOpenDestination, layout.open_destination_after_extract);
    append_focus_target(targets, FocusTargetKind::ExtractIntegrityHash, layout.sha);
    append_focus_target(targets, FocusTargetKind::ExtractDefenderScan, layout.defender);
}

// Purpose: Append all Settings page keyboard-focus targets.
// Inputs: `targets` receives controls and `content` is the content rectangle.
// Outputs: Adds Settings buttons, toggles, and dropdowns in tab order.
void MainWindow::add_settings_focus_targets(std::vector<FocusTarget>& targets, const RECT& content) const {
    const auto layout = settings_layout(content);
    append_focus_target(targets, FocusTargetKind::SettingsRestoreDefaults, layout.restore_defaults);
    append_focus_target(targets, FocusTargetKind::SettingsApply, layout.apply);
    append_focus_target(targets, FocusTargetKind::SettingsOpenDestination, layout.open_destination_after_operation);
    append_focus_target(targets, FocusTargetKind::SettingsConfirmDelete, layout.confirm_before_deleting);
    append_focus_target(targets, FocusTargetKind::SettingsShowSummary, layout.show_operation_summary);
    append_focus_target(targets, FocusTargetKind::SettingsIntegrityHash, layout.sha);
    append_focus_target(targets, FocusTargetKind::SettingsDefenderScan, layout.defender);
    append_focus_target(targets, FocusTargetKind::SettingsGpuRequired, layout.gpu);
    append_focus_target(targets, FocusTargetKind::SettingsVerifyAfterWrite, layout.verify);
    append_focus_target(targets, FocusTargetKind::SettingsMemoryPolicy, layout.memory_policy);
    append_focus_target(targets, FocusTargetKind::SettingsLogLevel, layout.log_level);
    append_focus_target(targets, FocusTargetKind::SettingsLogRetention, layout.log_retention);
}

// Purpose: Normalize the current keyboard focus to a valid target index.
// Inputs: `targets` is the current page's focus target list.
// Outputs: Updates focus index and returns false if no target exists.
bool MainWindow::normalize_focus_index(const std::vector<FocusTarget>& targets) {
    if (targets.empty()) {
        keyboard_focus_index_ = 0;
        return false;
    }
    keyboard_focus_index_ =
        (keyboard_focus_index_ % static_cast<int>(targets.size()) + static_cast<int>(targets.size())) %
        static_cast<int>(targets.size());
    return true;
}

// Purpose: Move keyboard focus by a signed delta through the current page's focus list.
// Inputs: `delta` is normally +1 or -1.
// Outputs: Mutates focus index and queues a repaint.
bool MainWindow::move_keyboard_focus(int delta) {
    UiState state;
    {
        std::lock_guard lock(mutex_);
        state = state_;
    }
    const auto targets = focus_targets_for(content_rect(), state);
    if (!normalize_focus_index(targets)) {
        return false;
    }
    keyboard_focus_index_ += delta;
    normalize_focus_index(targets);
    request_repaint();
    return true;
}

// Purpose: Activate a focused control with Enter or Space.
// Inputs: `target` is the current focus target and `key` is the activating virtual key.
// Outputs: Executes the same command/toggle/dropdown path as mouse activation.
bool MainWindow::activate_focus_target(const FocusTarget& target, WPARAM key) {
    if (target.kind == FocusTargetKind::Navigation) {
        set_page(static_cast<Page>(std::clamp(target.index, 0, 7)));
        return true;
    }
    if (target.kind == FocusTargetKind::QueueRow && key == VK_SPACE) {
        return toggle_queue_item(static_cast<std::size_t>(std::max(0, target.index)));
    }
    const int x = (target.rect.left + target.rect.right) / 2;
    const int y = (target.rect.top + target.rect.bottom) / 2;
    return handle_content_click(x, y);
}

// Purpose: Move within open dropdown options or Queue rows using arrow keys.
// Inputs: `key` is one of the supported arrow/home/end virtual keys.
// Outputs: Updates selection or focus and returns true when consumed.
bool MainWindow::handle_navigation_key(WPARAM key) {
    DropdownId active = DropdownId::None;
    UiState state;
    {
        std::lock_guard lock(mutex_);
        active = state_.active_dropdown;
        state = state_;
    }
    if (active != DropdownId::None) {
        const auto options = dropdown_options(active);
        if (options.empty()) {
            return true;
        }
        int index = dropdown_keyboard_index_ >= 0 ? dropdown_keyboard_index_ : dropdown_selected_index(state, active);
        if (key == VK_HOME) {
            index = 0;
        } else if (key == VK_END) {
            index = static_cast<int>(options.size()) - 1;
        } else if (key == VK_UP || key == VK_LEFT) {
            --index;
        } else if (key == VK_DOWN || key == VK_RIGHT) {
            ++index;
        }
        dropdown_keyboard_index_ = (index % static_cast<int>(options.size()) + static_cast<int>(options.size())) %
                                   static_cast<int>(options.size());
        request_repaint();
        return true;
    }
    if (key == VK_HOME || key == VK_END) {
        const auto targets = focus_targets_for(content_rect(), state);
        if (!normalize_focus_index(targets)) {
            return false;
        }
        keyboard_focus_index_ = key == VK_HOME ? 0 : static_cast<int>(targets.size()) - 1;
        request_repaint();
        return true;
    }
    if (key == VK_LEFT || key == VK_UP) {
        return move_keyboard_focus(-1);
    }
    if (key == VK_RIGHT || key == VK_DOWN) {
        return move_keyboard_focus(1);
    }
    return false;
}

// Purpose: Handle keyboard traversal and activation with native Windows conventions.
// Inputs: `wparam` is a virtual key and `lparam` is the raw key message payload.
// Outputs: Moves focus, activates controls, updates dropdowns, or returns default processing.
LRESULT MainWindow::handle_key_down(WPARAM wparam, LPARAM) {
    if (wparam == VK_ESCAPE) {
        close_active_dropdown();
        return 0;
    }
    if (wparam == VK_TAB) {
        return move_keyboard_focus((GetKeyState(VK_SHIFT) & 0x8000) != 0 ? -1 : 1) ? 0 : 0;
    }
    if (wparam == VK_UP || wparam == VK_DOWN || wparam == VK_LEFT || wparam == VK_RIGHT || wparam == VK_HOME ||
        wparam == VK_END) {
        return handle_navigation_key(wparam) ? 0 : 0;
    }
    if (wparam == VK_RETURN || wparam == VK_SPACE) {
        DropdownId active = DropdownId::None;
        UiState state;
        {
            std::lock_guard lock(mutex_);
            active = state_.active_dropdown;
            state = state_;
        }
        if (active != DropdownId::None) {
            const int selected =
                dropdown_keyboard_index_ >= 0 ? dropdown_keyboard_index_ : dropdown_selected_index(state, active);
            select_dropdown_option(active, selected);
            return 0;
        }
        const auto targets = focus_targets_for(content_rect(), state);
        if (normalize_focus_index(targets)) {
            (void)activate_focus_target(targets[static_cast<std::size_t>(keyboard_focus_index_)], wparam);
        }
        return 0;
    }
    return DefWindowProcW(hwnd_, WM_KEYDOWN, wparam, 0);
}

// Purpose: Handle clicks inside the content area.
// Inputs: `x` and `y` are physical-pixel mouse coordinates relative to the client area.
// Outputs: Returns true when a setting was changed and a repaint was queued.
bool MainWindow::handle_content_click(int x, int y) {
    Page page;
    {
        std::lock_guard lock(mutex_);
        page = state_.page;
    }
    const RECT content = content_rect();
    switch (page) {
    case Page::Queue:
        return handle_queue_click(content, x, y);
    case Page::Compress:
        return handle_compress_click(content, x, y);
    case Page::Extract:
        return handle_extract_click(content, x, y);
    case Page::Security:
        return handle_security_click(content, x, y);
    case Page::History:
        return handle_history_click(content, x, y);
    case Page::Gpu:
        return handle_gpu_click(content, x, y);
    case Page::Settings:
        return handle_settings_click(content, x, y);
    case Page::About:
        return false;
    }
    return false;
}

// Purpose: Toggle a boolean UI-state member with the standard animated toggle feedback.
// Inputs: `member` selects the state field and `id` identifies the visual toggle to animate.
// Outputs: Mutates the state, starts the toggle animation, queues repaint, and returns true.
bool MainWindow::toggle_bool_setting(bool UiState::* member, ToggleId id) {
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
}

// Purpose: Toggle a checkbox-style boolean UI-state member without knob animation.
// Inputs: `member` selects the state field and `status` is the visible status-line text.
// Outputs: Mutates the state, writes status text, queues repaint, and returns true.
bool MainWindow::checkbox_bool_setting(bool UiState::* member, const char* status) {
    {
        std::lock_guard lock(mutex_);
        state_.*member = !(state_.*member);
        state_.status = status;
    }
    request_repaint();
    return true;
}

// Purpose: Keep queue enable flags one-for-one with queued paths while the UI mutex is held.
// Inputs: None; reads and mutates `state_`.
// Outputs: Normalizes enable flags and selected row bounds.
void MainWindow::normalize_queue_selection_locked() {
    if (state_.queued_enabled.size() < state_.queued_paths.size()) {
        state_.queued_enabled.resize(state_.queued_paths.size(), true);
    } else if (state_.queued_enabled.size() > state_.queued_paths.size()) {
        state_.queued_enabled.resize(state_.queued_paths.size());
    }
    if (state_.queued_paths.empty()) {
        state_.selected_queue_index = -1;
    } else if (state_.selected_queue_index < 0 ||
               state_.selected_queue_index >= static_cast<int>(state_.queued_paths.size())) {
        state_.selected_queue_index = 0;
    }
}

// Purpose: Toggle all queued items from the header checkbox.
// Inputs: None.
// Outputs: Mutates every row enable flag and queues a repaint.
bool MainWindow::toggle_all_queue_items() {
    {
        std::lock_guard lock(mutex_);
        normalize_queue_selection_locked();
        if (state_.queued_paths.empty()) {
            state_.status = "Queue is empty";
            request_repaint();
            return true;
        }
        const bool all_enabled = std::all_of(state_.queued_enabled.begin(), state_.queued_enabled.end(),
                                             [](bool enabled) { return enabled; });
        std::fill(state_.queued_enabled.begin(), state_.queued_enabled.end(), !all_enabled);
        state_.status = all_enabled ? "All queue items deselected" : "All queue items selected";
    }
    request_repaint();
    return true;
}

// Purpose: Toggle one queued item checkbox.
// Inputs: `index` is the queue row to update.
// Outputs: Mutates the row enable flag, focuses the row, and queues a repaint.
bool MainWindow::toggle_queue_item(std::size_t index) {
    {
        std::lock_guard lock(mutex_);
        normalize_queue_selection_locked();
        if (index >= state_.queued_enabled.size()) {
            return false;
        }
        state_.queued_enabled[index] = !state_.queued_enabled[index];
        state_.selected_queue_index = static_cast<int>(index);
        state_.status = state_.queued_enabled[index] ? "Queue item selected" : "Queue item deselected";
    }
    request_repaint();
    return true;
}

// Purpose: Start resizing adjacent queue data columns.
// Inputs: `separator` is the 0-2 data-column boundary and `x` is the initial mouse coordinate.
// Outputs: Stores resize baseline state.
void MainWindow::begin_queue_column_resize(int separator, int x) {
    queue_column_resize_separator_ = std::clamp(separator, 0, 2);
    queue_column_resize_start_x_ = x;
    queue_column_resize_start_ = queue_column_widths_;
    SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
}

// Purpose: Update queue data-column widths while preserving readable minimums.
// Inputs: `x` is the current mouse coordinate.
// Outputs: Resizes the two columns adjacent to the active separator.
void MainWindow::update_queue_column_resize(int x) {
    if (queue_column_resize_separator_ < 0) {
        return;
    }
    constexpr std::array<int, 4> minimums{90, 70, 70, 120};
    const int left_index = queue_column_resize_separator_;
    const int right_index = left_index + 1;
    const int delta =
        std::lround(static_cast<double>(x - queue_column_resize_start_x_) * 96.0 / static_cast<double>(dpi_));
    const int pair_total = queue_column_resize_start_[left_index] + queue_column_resize_start_[right_index];
    const int left = std::clamp(queue_column_resize_start_[left_index] + delta, minimums[left_index],
                                pair_total - minimums[right_index]);
    queue_column_widths_[left_index] = left;
    queue_column_widths_[right_index] = pair_total - left;
    request_repaint();
}

// Purpose: End an active queue data-column resize.
// Inputs: None.
// Outputs: Clears resize state and queues one final repaint.
void MainWindow::end_queue_column_resize() {
    if (queue_column_resize_separator_ < 0) {
        return;
    }
    queue_column_resize_separator_ = -1;
    request_repaint();
}

// Purpose: Handle Queue page hit-testing and commands.
// Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
// Outputs: Returns true when a queue control or row selection consumed the click.
bool MainWindow::handle_queue_click(const RECT& content, int x, int y) {
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
    if (y >= layout.table.top && y < header_bottom) {
        const RECT header_row{layout.table.left, layout.table.top, layout.table.right, header_bottom};
        const auto columns = queue_column_layout(layout.table, header_row);
        for (int separator = 0; separator < static_cast<int>(columns.resize_grips.size()); ++separator) {
            if (contains_point(columns.resize_grips[static_cast<std::size_t>(separator)], x, y)) {
                begin_queue_column_resize(separator, x);
                return true;
            }
        }
        if (contains_point(columns.header_checkbox, x, y)) {
            return toggle_all_queue_items();
        }
        return true;
    }
    if (y >= header_bottom && y < layout.table.bottom && row_height > 0) {
        const int index = (y - header_bottom) / row_height;
        const RECT row{layout.table.left, header_bottom + (index * row_height), layout.table.right,
                       header_bottom + ((index + 1) * row_height)};
        const auto columns = queue_column_layout(layout.table, row);
        if (contains_point(columns.checkbox, x, y)) {
            return toggle_queue_item(static_cast<std::size_t>(std::max(0, index)));
        }
        {
            std::lock_guard lock(mutex_);
            normalize_queue_selection_locked();
            if (index >= 0 && index < static_cast<int>(state_.queued_paths.size())) {
                state_.selected_queue_index = index;
                request_repaint();
                return true;
            }
        }
    }
    return false;
}

// Purpose: Handle Compress page hit-testing and commands.
// Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
// Outputs: Returns true when a compression control consumed the click.
bool MainWindow::handle_compress_click(const RECT& content, int x, int y) {
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
    if (contains_point(layout.format, x, y)) {
        open_dropdown(DropdownId::CompressFormat);
        return true;
    }
    if (contains_point(layout.compression_level, x, y)) {
        bool enabled = false;
        {
            std::lock_guard lock(mutex_);
            enabled = compression_format_uses_level(compression_format_value(state_.compression_format_index));
        }
        if (enabled) {
            open_dropdown(DropdownId::CompressLevel);
        }
        return true;
    }
    if (contains_point(layout.method, x, y)) {
        bool enabled = false;
        {
            std::lock_guard lock(mutex_);
            enabled = compression_format_uses_suzip_tuning(compression_format_value(state_.compression_format_index));
        }
        if (enabled) {
            open_dropdown(DropdownId::CompressMethod);
        }
        return true;
    }
    if (contains_point(layout.block_size, x, y)) {
        bool enabled = false;
        {
            std::lock_guard lock(mutex_);
            enabled = compression_format_uses_suzip_tuning(compression_format_value(state_.compression_format_index));
        }
        if (enabled) {
            open_dropdown(DropdownId::CompressBlockSize);
        }
        return true;
    }
    if (contains_point(layout.solid_archive, x, y)) {
        return toggle_bool_setting(&UiState::solid_archive, ToggleId::SolidArchive);
    }
    if (contains_point(layout.store_timestamps, x, y)) {
        return toggle_bool_setting(&UiState::store_timestamps, ToggleId::StoreTimestamps);
    }
    if (contains_point(layout.delete_after_compression, x, y)) {
        return toggle_bool_setting(&UiState::delete_after_compression, ToggleId::DeleteAfterCompression);
    }
    if (contains_point(layout.verify, x, y)) {
        return toggle_bool_setting(&UiState::verify_after_write_opt_in, ToggleId::VerifyAfterWrite);
    }
    if (contains_point(layout.sha, x, y)) {
        return toggle_bool_setting(&UiState::integrity_hash_opt_in, ToggleId::IntegrityHash);
    }
    if (contains_point(layout.defender, x, y)) {
        return toggle_bool_setting(&UiState::defender_scan_opt_in, ToggleId::DefenderScan);
    }
    return false;
}

// Purpose: Handle Extract page hit-testing and commands.
// Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
// Outputs: Returns true when an extraction control consumed the click.
bool MainWindow::handle_extract_click(const RECT& content, int x, int y) {
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
        return toggle_bool_setting(&UiState::verify_metadata_before_extract, ToggleId::VerifyMetadata);
    }
    if (contains_point(layout.open_destination_after_extract, x, y)) {
        return toggle_bool_setting(&UiState::open_destination_after_extract, ToggleId::OpenDestinationAfterExtract);
    }
    if (contains_point(layout.sha, x, y)) {
        return toggle_bool_setting(&UiState::integrity_hash_opt_in, ToggleId::IntegrityHash);
    }
    if (contains_point(layout.defender, x, y)) {
        return toggle_bool_setting(&UiState::defender_scan_opt_in, ToggleId::DefenderScan);
    }
    return false;
}

// Purpose: Handle History page hit-testing and commands.
// Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
// Outputs: Returns true when a history filter or command consumed the click.
bool MainWindow::handle_history_click(const RECT& content, int x, int y) {
    RECT area = inset_rect(content, scale(kPageInsetX), scale(kPageInsetY));
    const RECT operation{area.left, area.top + scale(48), area.left + scale(220), area.top + scale(92)};
    const RECT status{area.left + scale(238), area.top + scale(48), area.left + scale(458), area.top + scale(92)};
    const RECT clear = history_clear_button_rect(area);
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

// Purpose: Handle Security page hit-testing and commands.
// Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
// Outputs: Returns true when the security review command consumed the click.
bool MainWindow::handle_security_click(const RECT& content, int x, int y) {
    RECT area = inset_rect(content, scale(kPageInsetX), scale(kPageInsetY));
    const RECT verify_button = primary_action_rect(area);
    if (!contains_point(verify_button, x, y)) {
        return false;
    }
    start_security_verify();
    return true;
}

// Purpose: Start a real background security verification pass for queued inputs.
// Inputs: None; reads enabled queue rows and security opt-ins.
// Outputs: Launches a Verify job or reports that no queued item is selected.
void MainWindow::start_security_verify() {
    std::vector<std::filesystem::path> sources;
    bool integrity = false;
    bool defender = false;
    {
        std::lock_guard lock(mutex_);
        normalize_queue_selection_locked();
        for (std::size_t i = 0; i < state_.queued_paths.size(); ++i) {
            if (state_.queued_enabled[i]) {
                sources.push_back(state_.queued_paths[i]);
            }
        }
        integrity = state_.integrity_hash_opt_in;
        defender = state_.defender_scan_opt_in;
    }
    if (sources.empty()) {
        {
            std::lock_guard lock(mutex_);
            state_.status = "Select queue items before security verification";
        }
        request_repaint();
        return;
    }
    run_job(
        [this, sources, integrity, defender] {
            ProgressState progress;
            progress.start(OperationKind::Verify, sources.size(), sources.size());
            auto publish = [this, &progress] { publish_progress_snapshot(progress.snapshot()); };
            for (const auto& path : sources) {
                progress.set_current(path.filename().string());
                publish();
                std::error_code ec;
                if (!std::filesystem::exists(path, ec)) {
                    throw SecurityError("queued path does not exist: " + path.string());
                }
                if (integrity && std::filesystem::is_regular_file(path, ec)) {
                    const auto hash = hash_file(path, IntegrityMode::Sha256);
                    append_history_entry("Security", path.filename().string(), "SHA-256 " + hash.hex_digest, true);
                }
                if (defender) {
                    const auto scan = scan_with_windows_defender(path, DefenderScanMode::FullPath);
                    append_history_entry("Security", path.filename().string(),
                                         defender_history_status("Defender", scan), !scan.attempted || scan.clean);
                    if (scan.attempted && !scan.clean) {
                        throw SecurityError("Microsoft Defender did not report the path as clean: " + path.string());
                    }
                }
                progress.add_bytes(1);
                progress.finish_entry();
                publish();
            }
            append_history_entry("Security", "Security review",
                                 "Verified " + std::to_string(sources.size()) + " selected queue item(s)", true);
        },
        "Verifying");
}

// Purpose: Handle System page hit-testing and commands.
// Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
// Outputs: Returns true when the update-speed dropdown consumed the click.
bool MainWindow::handle_gpu_click(const RECT& content, int x, int y) {
    const RECT update_speed = dropdown_anchor_rect(DropdownId::GpuUpdateSpeed, content);
    if (handle_active_dropdown_click(x, y)) {
        return true;
    }
    if (contains_point(update_speed, x, y)) {
        open_dropdown(DropdownId::GpuUpdateSpeed);
        return true;
    }
    return false;
}

// Purpose: Handle Settings page hit-testing and commands.
// Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
// Outputs: Returns true when a preference control consumed the click.
bool MainWindow::handle_settings_click(const RECT& content, int x, int y) {
    const auto layout = settings_layout(content);
    if (handle_active_dropdown_click(x, y)) {
        return true;
    }
    if (contains_point(layout.restore_defaults, x, y)) {
        try {
            reset_settings_to_defaults();
        } catch (const std::exception& error) {
            {
                std::lock_guard lock(mutex_);
                state_.status = std::string("Settings reset failed: ") + error.what();
            }
            append_log_entry(LogSeverity::Warning, "Settings reset failed");
        }
        return true;
    }
    if (contains_point(layout.apply, x, y)) {
        try {
            apply_settings();
        } catch (const std::exception& error) {
            {
                std::lock_guard lock(mutex_);
                state_.status = std::string("Settings apply failed: ") + error.what();
            }
            append_log_entry(LogSeverity::Warning, "Settings apply failed");
        }
        return true;
    }
    if (contains_point(layout.sha, x, y)) {
        return toggle_bool_setting(&UiState::integrity_hash_opt_in, ToggleId::IntegrityHash);
    }
    if (contains_point(layout.defender, x, y)) {
        return toggle_bool_setting(&UiState::defender_scan_opt_in, ToggleId::DefenderScan);
    }
    if (contains_point(layout.gpu, x, y)) {
        return toggle_bool_setting(&UiState::gpu_required, ToggleId::GpuRequired);
    }
    if (contains_point(layout.verify, x, y)) {
        return toggle_bool_setting(&UiState::verify_after_write_opt_in, ToggleId::VerifyAfterWrite);
    }
    if (contains_point(layout.open_destination_after_operation, x, y)) {
        return toggle_bool_setting(&UiState::open_destination_after_operation, ToggleId::OpenDestinationAfterOperation);
    }
    if (contains_point(layout.confirm_before_deleting, x, y)) {
        return toggle_bool_setting(&UiState::confirm_before_deleting, ToggleId::ConfirmBeforeDeleting);
    }
    if (contains_point(layout.show_operation_summary, x, y)) {
        return toggle_bool_setting(&UiState::show_operation_summary, ToggleId::ShowOperationSummary);
    }
    if (contains_point(layout.memory_policy, x, y)) {
        open_dropdown(DropdownId::SettingsMemoryPolicy);
        return true;
    }
    if (contains_point(layout.log_level, x, y)) {
        open_dropdown(DropdownId::SettingsLogLevel);
        return true;
    }
    if (contains_point(layout.log_retention, x, y)) {
        open_dropdown(DropdownId::SettingsLogRetention);
        return true;
    }
    return false;
}

// Purpose: Handle a click while a dropdown menu is expanded.
// Inputs: `x` and `y` are physical-pixel mouse coordinates relative to the client area.
// Outputs: Returns true when the click was consumed by dropdown close or selection.
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
        const auto options = dropdown_options(active);
        const int row_height = scale(options.size() > 10U ? 28 : 32);
        const int option_index = row_height > 0 ? (y - menu.top - scale(1)) / row_height : -1;
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

// Purpose: Open one dropdown menu and seed keyboard selection to the current value.
// Inputs: `id` identifies the dropdown to open.
// Outputs: Updates UI state and queues a repaint.
void MainWindow::open_dropdown(DropdownId id) {
    {
        std::lock_guard lock(mutex_);
        state_.active_dropdown = id;
        state_.status = "Dropdown opened";
        dropdown_keyboard_index_ = dropdown_selected_index(state_, id);
    }
    request_repaint();
}

// Purpose: Close any expanded dropdown menu.
// Inputs: None.
// Outputs: Clears dropdown state and queues a repaint when needed.
void MainWindow::close_active_dropdown() {
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        changed = state_.active_dropdown != DropdownId::None;
        state_.active_dropdown = DropdownId::None;
        dropdown_keyboard_index_ = -1;
    }
    if (changed) {
        request_repaint();
    }
}

struct DropdownSelectionFeedback {
    int performance_seconds = 0;
    bool add_log = false;
    LogSeverity log_severity = LogSeverity::Information;
    std::string log_message;
};

// Purpose: Apply a non-Settings dropdown choice to synchronized UI state.
// Inputs: `state` is locked UI state, `id` is the dropdown, `option_index` is the selected row, and `feedback` receives
// side effects. Outputs: Mutates `state`; unsupported IDs are ignored.
void apply_primary_dropdown_selection(UiState& state, DropdownId id, int option_index,
                                      DropdownSelectionFeedback& feedback) {
    switch (id) {
    case DropdownId::CompressFormat:
        state.compression_format_index = std::clamp(option_index, 0, kCompressionFormatMaxIndex);
        state.status = "Archive format changed";
        break;
    case DropdownId::CompressLevel:
        state.compression_level_index = std::clamp(option_index, 0, 4);
        state.status = "Compression level changed";
        break;
    case DropdownId::CompressMethod:
        state.gpu_required = option_index == 0;
        state.status = "Compression method changed";
        break;
    case DropdownId::CompressBlockSize:
        state.compression_block_size_index = std::clamp(option_index, 0, kCompressionBlockSizeMaxIndex);
        state.status = "Block size changed";
        break;
    case DropdownId::ExtractOverwrite:
        state.overwrite = option_index == 1;
        state.status = "Overwrite policy changed";
        break;
    case DropdownId::HistoryOperation:
        state.history_operation_filter_index = std::clamp(option_index, 0, 3);
        state.status = "History operation filter changed";
        break;
    case DropdownId::HistoryStatus:
        state.history_status_filter_index = std::clamp(option_index, 0, 2);
        state.status = "History status filter changed";
        break;
    case DropdownId::GpuUpdateSpeed:
        state.performance_update_seconds = kPerformanceUpdateSecondsOptions[static_cast<std::size_t>(
            std::clamp(option_index, 0, static_cast<int>(kPerformanceUpdateSecondsOptions.size()) - 1))];
        feedback.performance_seconds = state.performance_update_seconds;
        state.status = "Refresh interval changed";
        break;
    default:
        break;
    }
}

// Purpose: Apply a Settings dropdown choice to synchronized UI state.
// Inputs: `state` is locked UI state, `id` is the dropdown, `option_index` is the selected row, and `feedback` receives
// log data. Outputs: Mutates `state`; unsupported IDs are ignored.
void apply_settings_dropdown_selection(UiState& state, DropdownId id, int option_index,
                                       DropdownSelectionFeedback& feedback) {
    switch (id) {
    case DropdownId::SettingsMemoryPolicy:
        state.memory_policy_index = std::clamp(option_index, 0, 2);
        state.status = "Memory policy changed";
        feedback.add_log = true;
        feedback.log_severity = LogSeverity::Debug;
        feedback.log_message = "Memory policy changed";
        break;
    case DropdownId::SettingsLogLevel:
        state.log_level_index = std::clamp(option_index, 0, 2);
        state.status = "Log level changed";
        feedback.add_log = true;
        if (state.log_level_index == 1) {
            feedback.log_severity = LogSeverity::Warning;
            feedback.log_message = "Log level changed to Warning";
        } else if (state.log_level_index == 2) {
            feedback.log_severity = LogSeverity::Debug;
            feedback.log_message = "Log level changed to Debug";
        } else {
            feedback.log_severity = LogSeverity::Information;
            feedback.log_message = "Log level changed to Information";
        }
        break;
    case DropdownId::SettingsLogRetention:
        state.log_retention_index = std::clamp(option_index, 0, 2);
        prune_log_entries(state.logs, std::chrono::system_clock::now(), state.log_retention_index, kMaxLogEntries);
        state.status = "Log retention changed";
        feedback.add_log = true;
        feedback.log_severity = LogSeverity::Debug;
        feedback.log_message = "Log retention changed to ";
        feedback.log_message += log_retention_log_text(state.log_retention_index);
        break;
    default:
        break;
    }
}

// Purpose: Apply one selected dropdown option to the matching UI state field.
// Inputs: `id` identifies the active dropdown and `option_index` is the zero-based row selected by the user.
// Outputs: Mutates UI state, closes the dropdown, updates status text, and requests repaint.
void MainWindow::select_dropdown_option(DropdownId id, int option_index) {
    int performance_seconds = 0;
    bool add_log = false;
    LogSeverity log_severity = LogSeverity::Information;
    std::string log_message;
    {
        std::lock_guard lock(mutex_);
        DropdownSelectionFeedback feedback;
        apply_primary_dropdown_selection(state_, id, option_index, feedback);
        apply_settings_dropdown_selection(state_, id, option_index, feedback);
        state_.active_dropdown = DropdownId::None;
        dropdown_keyboard_index_ = -1;
        performance_seconds = feedback.performance_seconds;
        add_log = feedback.add_log;
        log_severity = feedback.log_severity;
        log_message = std::move(feedback.log_message);
    }
    if (performance_seconds > 0) {
        reset_performance_timer(performance_seconds);
    }
    if (add_log) {
        append_log_entry(log_severity, std::move(log_message));
        return;
    }
    request_repaint();
}

// Purpose: Change the active application page and discard unapplied Settings drafts when leaving Settings.
// Inputs: `page` is the destination page.
// Outputs: Mutates page state and restores the last applied Settings snapshot if needed.
void MainWindow::set_page(Page page) {
    bool revert_unapplied_settings = false;
    {
        std::lock_guard lock(mutex_);
        const Page previous = state_.page;
        if (previous == page) {
            return;
        }
        state_.page = page;
        state_.active_dropdown = DropdownId::None;
        dropdown_keyboard_index_ = -1;
        keyboard_focus_index_ = 0;
        text_tooltip_cell_active_ = false;
        text_tooltip_visible_ = false;
        text_tooltip_text_.clear();
        KillTimer(hwnd_, kTextTooltipTimer);
        revert_unapplied_settings = previous == Page::Settings && page != Page::Settings;
    }
    if (revert_unapplied_settings) {
        revert_settings_draft();
    }
    request_repaint();
}

// Purpose: Open the Windows file picker and append selected files to the queue.
// Inputs: None; user selection comes from the common dialog or GUI smoke-test environment.
// Outputs: Mutates queued paths and queues a repaint when files are selected.
void MainWindow::add_files() {
    wchar_t smoke_paths[32768]{};
    constexpr DWORD smoke_paths_capacity = static_cast<DWORD>(sizeof(smoke_paths) / sizeof(smoke_paths[0]));
    const DWORD smoke_length =
        GetEnvironmentVariableW(L"SUPERZIP_GUI_SMOKE_FILE_SELECTION", smoke_paths, smoke_paths_capacity);
    if (smoke_length > 0 && smoke_length < smoke_paths_capacity) {
        std::lock_guard lock(mutex_);
        const bool was_empty = state_.queued_paths.empty();
        std::wstring_view paths(smoke_paths, smoke_length);
        std::size_t start = 0;
        while (start <= paths.size()) {
            const std::size_t end = paths.find(L';', start);
            const auto part =
                paths.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start);
            if (!part.empty()) {
                state_.queued_paths.emplace_back(std::wstring(part));
                state_.queued_enabled.push_back(true);
            }
            if (end == std::wstring_view::npos) {
                break;
            }
            start = end + 1;
        }
        if (was_empty && !state_.queued_paths.empty()) {
            state_.selected_queue_index = 0;
        }
        normalize_queue_selection_locked();
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
        state_.queued_enabled.push_back(true);
    } else {
        while (*cursor != L'\0') {
            std::filesystem::path name(cursor);
            state_.queued_paths.push_back(dir / name);
            state_.queued_enabled.push_back(true);
            cursor += name.wstring().size() + 1;
        }
    }
    if (was_empty && !state_.queued_paths.empty()) {
        state_.selected_queue_index = 0;
    }
    normalize_queue_selection_locked();
    request_repaint();
}

// Purpose: Open the Windows folder picker and append a selected folder to the queue.
// Inputs: None; user selection comes from the shell folder picker or GUI smoke-test environment.
// Outputs: Mutates queued paths and queues a repaint when a folder is selected.
void MainWindow::add_folder() {
    wchar_t smoke_path[32768]{};
    constexpr DWORD smoke_path_capacity = static_cast<DWORD>(sizeof(smoke_path) / sizeof(smoke_path[0]));
    const DWORD smoke_length =
        GetEnvironmentVariableW(L"SUPERZIP_GUI_SMOKE_FOLDER_SELECTION", smoke_path, smoke_path_capacity);
    if (smoke_length > 0 && smoke_length < smoke_path_capacity) {
        std::lock_guard lock(mutex_);
        const bool was_empty = state_.queued_paths.empty();
        state_.queued_paths.emplace_back(smoke_path);
        state_.queued_enabled.push_back(true);
        if (was_empty) {
            state_.selected_queue_index = 0;
        }
        normalize_queue_selection_locked();
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
        state_.queued_enabled.push_back(true);
        if (was_empty) {
            state_.selected_queue_index = 0;
        }
        normalize_queue_selection_locked();
    }
    request_repaint();
}

// Purpose: Remove every queued path and reset row selection state.
// Inputs: None.
// Outputs: Mutates queue state and queues a repaint.
void MainWindow::clear_queue() {
    {
        std::lock_guard lock(mutex_);
        state_.queued_paths.clear();
        state_.queued_enabled.clear();
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

// Purpose: Open the Windows folder picker for destination selection.
// Inputs: None; user selection comes from the shell dialog or smoke-test environment override.
// Outputs: Updates the destination directory and queues a repaint when selected.
void MainWindow::choose_destination() {
    wchar_t smoke_path[32768]{};
    constexpr DWORD smoke_path_capacity = static_cast<DWORD>(sizeof(smoke_path) / sizeof(smoke_path[0]));
    const DWORD smoke_length =
        GetEnvironmentVariableW(L"SUPERZIP_GUI_SMOKE_DESTINATION", smoke_path, smoke_path_capacity);
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

void MainWindow::cycle_compression_level() {
    {
        std::lock_guard lock(mutex_);
        state_.compression_level_index = (state_.compression_level_index + 1) % 5;
        state_.status = "Compression level changed";
    }
    request_repaint();
}

// Purpose: Start a background compression job from the current GUI queue.
// Inputs: None; reads queued paths and compression options from synchronized UI state.
// Outputs: Launches a worker job, updates progress/history/status, or requests repaint when the queue is empty.
void MainWindow::start_compress() {
    std::vector<std::filesystem::path> sources;
    bool gpu_required = true;
    bool integrity = false;
    bool defender = false;
    bool verify_after_write = false;
    std::uint32_t block_size = superzip::kDefaultArchiveBlockBytes;
    int compression_level = superzip::kDefaultCompressionLevel;
    ArchiveFormat archive_format = ArchiveFormat::SuperZip;
    std::filesystem::path output;
    {
        std::lock_guard lock(mutex_);
        normalize_queue_selection_locked();
        for (std::size_t i = 0; i < state_.queued_paths.size(); ++i) {
            if (state_.queued_enabled[i]) {
                sources.push_back(state_.queued_paths[i]);
            }
        }
        gpu_required = state_.gpu_required;
        integrity = state_.integrity_hash_opt_in;
        defender = state_.defender_scan_opt_in;
        verify_after_write = state_.verify_after_write_opt_in;
        block_size = compression_block_size_bytes(state_.compression_block_size_index);
        compression_level = compression_level_value(state_.compression_level_index);
        archive_format = compression_format_value(state_.compression_format_index);
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
    run_job(
        [this, sources, output, archive_format, gpu_required, integrity, defender, verify_after_write, block_size,
         compression_level] {
            auto progress_callback = [this](const ProgressSnapshot& snapshot) { publish_progress_snapshot(snapshot); };
            const auto stats = compress_gui_archive(sources, output, archive_format, gpu_required, verify_after_write,
                                                    block_size, compression_level, progress_callback);
            std::ostringstream line;
            line << "Compressed " << archive_format_info(archive_format).key << " to " << output.string() << " in "
                 << stats.seconds << "s";
            append_history_entry("Compress", output.filename().string(), line.str(), true);
            if (integrity) {
                const auto hash = hash_file(output, IntegrityMode::Sha256);
                append_history_entry("Security", output.filename().string(), "SHA-256 " + hash.hex_digest, true);
            }
            if (defender) {
                const auto scan = scan_with_windows_defender(output, DefenderScanMode::FullPath);
                append_history_entry("Security", output.filename().string(), defender_history_status("Defender", scan),
                                     !scan.attempted || scan.clean);
            }
        },
        "Compressing");
}

// Purpose: Start a background extraction job from the current GUI queue.
// Inputs: None; reads queued archive, destination, security, and GPU options from synchronized UI state.
// Outputs: Launches a worker job, updates progress/history/status, blocks unclean pre-scans, or requests repaint when
// no archive is selected.
void MainWindow::start_extract() {
    std::vector<std::filesystem::path> sources;
    bool gpu_required = true;
    bool overwrite = false;
    bool integrity = false;
    bool defender = false;
    std::filesystem::path output;
    {
        std::lock_guard lock(mutex_);
        normalize_queue_selection_locked();
        for (std::size_t i = 0; i < state_.queued_paths.size(); ++i) {
            if (state_.queued_enabled[i]) {
                sources.push_back(state_.queued_paths[i]);
            }
        }
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
    run_job(
        [this, archive = sources.front(), output, gpu_required, overwrite, integrity, defender] {
            if (integrity) {
                const auto hash = hash_file(archive, IntegrityMode::Sha256);
                append_history_entry("Security", archive.filename().string(), "SHA-256 " + hash.hex_digest, true);
            }
            if (defender) {
                const auto pre_scan = scan_with_windows_defender(archive, DefenderScanMode::FullPath);
                append_history_entry("Security", archive.filename().string(),
                                     defender_history_status("Defender archive", pre_scan),
                                     !pre_scan.attempted || pre_scan.clean);
                if (pre_scan.attempted && !pre_scan.clean) {
                    throw SecurityError("Microsoft Defender did not report the archive as clean: " + archive.string());
                }
            }
            auto progress_callback = [this](const ProgressSnapshot& snapshot) { publish_progress_snapshot(snapshot); };
            const auto archive_format = detect_archive_format(archive);
            const auto stats =
                extract_detected_archive(archive_format, archive, output, gpu_required, overwrite, progress_callback);
            std::ostringstream line;
            line << "Extracted " << archive_format_info(archive_format).key << " to " << output.string() << " in "
                 << stats.seconds << "s";
            append_history_entry("Extract", archive.filename().string(), line.str(), true);
            if (defender) {
                const auto post_scan = scan_with_windows_defender(output, DefenderScanMode::FullPath);
                append_history_entry("Security", output.filename().string(),
                                     defender_history_status("Defender output", post_scan),
                                     !post_scan.attempted || post_scan.clean);
            }
        },
        "Extracting");
}

// Purpose: Run one long operation on the background worker thread.
// Inputs: `job` performs the operation and `label` is the visible busy status.
// Outputs: Updates status, progress, history failure rows, and repaint state when the worker completes.
void MainWindow::run_job(std::function<void()> job, std::string label) {
    if (worker_running_.exchange(true)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    const std::string failure_operation = operation_for_job_label(label);
    {
        std::lock_guard lock(mutex_);
        state_.status = label;
        state_.progress = {};
        state_.progress_visible_until = {};
    }
    KillTimer(hwnd_, kProgressHoldTimer);
    worker_ = std::thread([this, job = std::move(job), failure_operation] {
        try {
            job();
            std::lock_guard lock(mutex_);
            state_.status = "Ready";
            retain_progress_after_stop_locked(true);
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex_);
            state_.status = std::string("Error: ") + error.what();
            retain_progress_after_stop_locked(false);
            state_.history.push_back(HistoryEntry{
                .operation = failure_operation,
                .subject = state_.status,
                .detail = error.what(),
                .success = false,
            });
        }
        worker_running_ = false;
        request_repaint();
    });
    request_repaint();
}

// Purpose: Publish one active operation progress snapshot to the UI.
// Inputs: `snapshot` is an immutable worker progress sample.
// Outputs: Replaces visible progress, cancels any completed-progress hold timer, and queues repaint.
void MainWindow::publish_progress_snapshot(const ProgressSnapshot& snapshot) {
    {
        std::lock_guard lock(mutex_);
        state_.progress = snapshot;
        state_.progress_visible_until = {};
    }
    KillTimer(hwnd_, kProgressHoldTimer);
    request_repaint();
}

// Purpose: Keep the last progress sample visible after an operation stops.
// Inputs: `mark_complete` fills known totals for successful work; caller must hold `mutex_`.
// Outputs: Arms the 15-second clear timer or clears idle progress.
void MainWindow::retain_progress_after_stop_locked(bool mark_complete) {
    if (state_.progress.operation == OperationKind::Idle) {
        state_.progress_visible_until = {};
        return;
    }
    if (mark_complete) {
        if (state_.progress.total_bytes > 0U) {
            state_.progress.processed_bytes = state_.progress.total_bytes;
        }
        if (state_.progress.total_entries > 0U) {
            state_.progress.completed_entries = state_.progress.total_entries;
        }
    }
    state_.progress_visible_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kProgressHoldMs);
    if (hwnd_ != nullptr) {
        SetTimer(hwnd_, kProgressHoldTimer, kProgressHoldMs, nullptr);
    }
}

// Purpose: Clear retained progress after its hold interval expires.
// Inputs: None; reads the synchronized progress hold timestamp.
// Outputs: Clears visible progress and queues repaint when the deadline has elapsed.
void MainWindow::clear_expired_progress() {
    bool should_kill_timer = false;
    {
        std::lock_guard lock(mutex_);
        if (state_.progress_visible_until == std::chrono::steady_clock::time_point{}) {
            should_kill_timer = true;
        } else if (std::chrono::steady_clock::now() < state_.progress_visible_until) {
            return;
        } else {
            state_.progress = {};
            state_.progress_visible_until = {};
            should_kill_timer = true;
        }
    }
    if (should_kill_timer) {
        KillTimer(hwnd_, kProgressHoldTimer);
        request_repaint();
    }
}

// Purpose: Append one bounded in-memory log entry.
// Inputs: `severity` is the visible category and `message` is safe session text.
// Outputs: Adds a timestamped row, prunes expired/over-capacity rows, and queues repaint.
void MainWindow::append_log_entry(LogSeverity severity, std::string message) {
    {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::system_clock::now();
        prune_log_entries(state_.logs, now, state_.log_retention_index, kMaxLogEntries);
        state_.logs.push_back(LogEntry{.severity = severity, .message = std::move(message), .timestamp = now});
        prune_log_entries(state_.logs, now, state_.log_retention_index, kMaxLogEntries);
    }
    request_repaint();
}

// Purpose: Convert a legacy history text line into a structured session row.
// Inputs: `line` is the existing caller text.
// Outputs: Appends a classified history entry.
void MainWindow::append_history(const std::string& line) {
    const bool success = !line.starts_with("Error");
    std::string operation = "Security";
    if (line.starts_with("Compressed")) {
        operation = "Compress";
    } else if (line.starts_with("Extracted")) {
        operation = "Extract";
    } else if (line.starts_with("Error")) {
        operation = "Failure";
    }
    append_history_entry(operation, line, line, success);
}

// Purpose: Add a structured operation result to session history.
// Inputs: `operation`, `subject`, `detail`, and `success` describe the visible history row.
// Outputs: Appends one row to in-memory UI history.
void MainWindow::append_history_entry(std::string operation, std::string subject, std::string detail, bool success) {
    std::lock_guard lock(mutex_);
    state_.history.push_back(HistoryEntry{
        .operation = std::move(operation),
        .subject = std::move(subject),
        .detail = std::move(detail),
        .success = success,
    });
}

void MainWindow::refresh_gpu_status() {
    const auto info = query_gpu_info();
    std::lock_guard lock(mutex_);
    state_.gpu_status = info.status;
    state_.gpu_runtime_name = info.runtime_name;
    state_.gpu_device_name = info.device_name;
    state_.gpu_arch = info.gcn_arch;
}

// Purpose: Initialize optional Windows performance counters for live monitoring.
// Inputs: None; uses the current process and Windows PDH provider.
// Outputs: Opens best-effort GPU engine counters without failing app startup.
void MainWindow::initialize_performance_monitor() {
    if (gpu_query_ != nullptr) {
        return;
    }
    if (PdhOpenQueryW(nullptr, 0, &gpu_query_) != ERROR_SUCCESS) {
        gpu_query_ = nullptr;
        gpu_counter_ = nullptr;
        return;
    }
    const PDH_STATUS status =
        PdhAddEnglishCounterW(gpu_query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpu_counter_);
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

// Purpose: Re-arm the live performance sampling timer at the selected interval.
// Inputs: `seconds` is normalized to one of the supported 1, 3, 5, or 10 second ranges.
// Outputs: Replaces the existing timer interval for the main window.
void MainWindow::reset_performance_timer(int seconds) {
    if (hwnd_ == nullptr) {
        return;
    }
    const auto interval = static_cast<UINT>(normalize_performance_update_seconds(seconds) * 1000);
    KillTimer(hwnd_, kPerformanceTimer);
    SetTimer(hwnd_, kPerformanceTimer, interval, nullptr);
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

// Purpose: Sample SuperZip process CPU use since the previous monitor tick.
// Inputs: `elapsed_seconds` is the interval from the previous sample.
// Outputs: Returns total-system-capacity CPU percentage and updates previous process FILETIME state.
double MainWindow::sample_process_cpu_percent(double elapsed_seconds) {
    double cpu_percent = 0.0;
    FILETIME creation_time{};
    FILETIME exit_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time)) {
        return cpu_percent;
    }
    if (elapsed_seconds > 0.0) {
        const std::uint64_t previous_ticks =
            filetime_ticks(last_process_kernel_time_) + filetime_ticks(last_process_user_time_);
        const std::uint64_t current_ticks = filetime_ticks(kernel_time) + filetime_ticks(user_time);
        const auto logical_processors = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        if (current_ticks >= previous_ticks) {
            cpu_percent = (static_cast<double>(current_ticks - previous_ticks) /
                           (elapsed_seconds * 10000000.0 * static_cast<double>(logical_processors))) *
                          100.0;
            cpu_percent = std::clamp(cpu_percent, 0.0, 100.0);
        }
    }
    last_process_kernel_time_ = kernel_time;
    last_process_user_time_ = user_time;
    return cpu_percent;
}

// Purpose: Sample total system CPU use since the previous monitor tick.
// Inputs: `elapsed_seconds` is the interval from the previous sample.
// Outputs: Returns logical-processor-normalized CPU percentage and updates previous system FILETIME state.
double MainWindow::sample_system_cpu_percent(double elapsed_seconds) {
    FILETIME idle_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        return 0.0;
    }
    double cpu_percent = 0.0;
    if (elapsed_seconds > 0.0) {
        const std::uint64_t previous_idle = filetime_ticks(last_system_idle_time_);
        const std::uint64_t previous_kernel = filetime_ticks(last_system_kernel_time_);
        const std::uint64_t previous_user = filetime_ticks(last_system_user_time_);
        const std::uint64_t current_idle = filetime_ticks(idle_time);
        const std::uint64_t current_kernel = filetime_ticks(kernel_time);
        const std::uint64_t current_user = filetime_ticks(user_time);
        const std::uint64_t previous_total = previous_kernel + previous_user;
        const std::uint64_t current_total = current_kernel + current_user;
        if (current_total >= previous_total && current_idle >= previous_idle) {
            const auto total_delta = current_total - previous_total;
            const auto idle_delta = current_idle - previous_idle;
            if (total_delta > 0U) {
                const auto busy_delta = total_delta > idle_delta ? total_delta - idle_delta : 0U;
                cpu_percent = (static_cast<double>(busy_delta) / static_cast<double>(total_delta)) * 100.0;
            }
        }
    }
    last_system_idle_time_ = idle_time;
    last_system_kernel_time_ = kernel_time;
    last_system_user_time_ = user_time;
    return std::clamp(cpu_percent, 0.0, 100.0);
}

// Purpose: Sample Windows GPU dedicated memory assigned to the SuperZip process.
// Inputs: None; uses current-process PDH GPU Process Memory counters.
// Outputs: Returns dedicated GPU memory bytes or zero when Windows does not expose the counter.
std::uint64_t MainWindow::sample_process_dedicated_vram_bytes() const {
    PDH_HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS || query == nullptr) {
        return 0U;
    }
    struct QueryGuard {
        PDH_HQUERY query = nullptr;
        ~QueryGuard() {
            if (query != nullptr) {
                PdhCloseQuery(query);
            }
        }
    } guard{query};

    PDH_HCOUNTER counter = nullptr;
    if (PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &counter) != ERROR_SUCCESS ||
        counter == nullptr) {
        return 0U;
    }
    if (PdhCollectQueryData(query) != ERROR_SUCCESS) {
        return 0U;
    }

    DWORD buffer_size = 0;
    DWORD item_count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &buffer_size, &item_count, nullptr);
    if (status != PDH_MORE_DATA || buffer_size == 0U || item_count == 0U) {
        return 0U;
    }
    std::vector<std::byte> buffer(buffer_size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &buffer_size, &item_count, items);
    if (status != ERROR_SUCCESS) {
        return 0U;
    }

    const std::wstring pid_marker = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    std::uint64_t total = 0U;
    for (DWORD index = 0; index < item_count; ++index) {
        const auto& item = items[index];
        if (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA || item.szName == nullptr ||
            std::wstring_view(item.szName).find(pid_marker) == std::wstring_view::npos) {
            continue;
        }
        if (item.FmtValue.largeValue > 0) {
            total += static_cast<std::uint64_t>(item.FmtValue.largeValue);
        }
    }
    return total;
}

// Purpose: Sample process read/write transfer rates since the previous monitor tick.
// Inputs: `elapsed_seconds` is the interval from the previous sample.
// Outputs: Returns non-negative byte-per-second rates and updates previous I/O counters.
MainWindow::ProcessIoRates MainWindow::sample_process_io_rates(double elapsed_seconds) {
    ProcessIoRates rates;
    IO_COUNTERS io_counters{};
    if (!GetProcessIoCounters(GetCurrentProcess(), &io_counters)) {
        return rates;
    }
    if (elapsed_seconds > 0.0) {
        if (io_counters.ReadTransferCount >= last_io_read_bytes_) {
            rates.read_bytes_per_second =
                static_cast<double>(io_counters.ReadTransferCount - last_io_read_bytes_) / elapsed_seconds;
        }
        if (io_counters.WriteTransferCount >= last_io_write_bytes_) {
            rates.write_bytes_per_second =
                static_cast<double>(io_counters.WriteTransferCount - last_io_write_bytes_) / elapsed_seconds;
        }
    }
    last_io_read_bytes_ = io_counters.ReadTransferCount;
    last_io_write_bytes_ = io_counters.WriteTransferCount;
    return rates;
}

// Purpose: Refresh cached HIP VRAM and visible GPU identity at a bounded cadence.
// Inputs: `now` is the current steady-clock timestamp.
// Outputs: Updates cached VRAM fields and GPU status text when the throttle permits a HIP query.
void MainWindow::refresh_gpu_memory_cache(std::chrono::steady_clock::time_point now) {
    if (last_gpu_memory_sample_time_ != std::chrono::steady_clock::time_point{} &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_gpu_memory_sample_time_).count() <
            kGpuMemorySampleMs) {
        return;
    }
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

// Purpose: Publish a completed performance sample into current UI state and graph history.
// Inputs: `sample` is the counter set and `now` is the sample timestamp.
// Outputs: Updates visible performance state, ring-buffer graph history, and last-sample time.
void MainWindow::publish_performance_sample(const PerformanceMonitorSample& sample,
                                            std::chrono::steady_clock::time_point now) {
    {
        std::lock_guard lock(mutex_);
        state_.performance = sample;
    }
    performance_history_[performance_history_next_] = sample;
    performance_history_next_ = (performance_history_next_ + 1U) % performance_history_.size();
    performance_history_count_ = std::min(performance_history_count_ + 1U, performance_history_.size());
    last_performance_sample_time_ = now;
}

// Purpose: Collect one live performance sample for the System page.
// Inputs: None; reads process, memory, I/O, optional PDH, and throttled HIP memory state.
// Outputs: Updates `state_.performance` with CPU, RAM, I/O, GPU utilization, and VRAM values.
void MainWindow::update_performance_sample() {
    const auto now = std::chrono::steady_clock::now();
    double elapsed_seconds = 0.0;
    if (last_performance_sample_time_ != std::chrono::steady_clock::time_point{}) {
        elapsed_seconds = std::chrono::duration<double>(now - last_performance_sample_time_).count();
    }

    const double cpu_percent = sample_system_cpu_percent(elapsed_seconds);
    const double process_cpu_percent = sample_process_cpu_percent(elapsed_seconds);
    const auto io_rates = sample_process_io_rates(elapsed_seconds);
    const auto private_bytes = sample_private_memory_bytes();
    const auto system_memory = sample_system_memory_usage();
    refresh_gpu_memory_cache(now);
    const double gpu_percent = sample_gpu_utilization();

    PerformanceMonitorSample sample;
    sample.live = true;
    sample.gpu_utilization_available = gpu_percent >= 0.0;
    sample.cpu_percent = cpu_percent;
    sample.process_cpu_percent = process_cpu_percent;
    sample.gpu_utilization_percent = sample.gpu_utilization_available ? gpu_percent : 0.0;
    sample.system_memory_percent = system_memory.percent;
    sample.io_read_bytes_per_second = std::max(0.0, io_rates.read_bytes_per_second);
    sample.io_write_bytes_per_second = std::max(0.0, io_rates.write_bytes_per_second);
    sample.private_bytes = private_bytes;
    sample.system_memory_total_bytes = system_memory.total_bytes;
    sample.system_memory_used_bytes = system_memory.used_bytes;
    sample.vram_total_bytes = cached_vram_total_bytes_;
    sample.vram_free_bytes = cached_vram_free_bytes_;
    sample.process_dedicated_vram_bytes = sample_process_dedicated_vram_bytes();
    publish_performance_sample(sample, now);
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

// Purpose: Start a bounded non-blocking command-button release animation.
// Inputs: `point` is the client-coordinate release position after a primary mouse click.
// Outputs: Records the release pulse origin and arms the animation timer.
void MainWindow::start_button_release_animation(POINT point) {
    button_release_point_ = point;
    button_release_start_ = std::chrono::steady_clock::now();
    SetTimer(hwnd_, kAnimationTimer, 8, nullptr);
}

double MainWindow::page_transition_progress() const {
    if (page_transition_start_ == std::chrono::steady_clock::time_point{}) {
        return 1.0;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - page_transition_start_)
            .count();
    return std::clamp(static_cast<double>(elapsed) / static_cast<double>(kPageTransitionMs), 0.0, 1.0);
}

// Purpose: Return a toggle's visual knob position.
// Inputs: `id` identifies the toggle and `enabled` is the final logical state.
// Outputs: Returns a normalized knob position from 0.0 to 1.0.
double MainWindow::toggle_visual_position(ToggleId id, bool enabled) const {
    const double final_position = enabled ? 1.0 : 0.0;
    if (id == ToggleId::None || id != transition_toggle_ ||
        toggle_transition_start_ == std::chrono::steady_clock::time_point{}) {
        return final_position;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                               toggle_transition_start_)
                             .count();
    const double progress =
        std::clamp(static_cast<double>(elapsed) / static_cast<double>(kToggleTransitionMs), 0.0, 1.0);
    if (progress >= 1.0) {
        return final_position;
    }
    const double start = transition_toggle_from_ ? 1.0 : 0.0;
    const double end = transition_toggle_to_ ? 1.0 : 0.0;
    return start + ((end - start) * ease_out(progress));
}

// Purpose: Return normalized command-button release animation progress.
// Inputs: `rect` is the button rectangle being rendered.
// Outputs: Returns 1.0 when no release pulse applies to this button.
double MainWindow::button_release_progress(const RECT& rect) const {
    if (button_release_start_ == std::chrono::steady_clock::time_point{} ||
        !contains_point(rect, button_release_point_.x, button_release_point_.y)) {
        return 1.0;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - button_release_start_)
            .count();
    return std::clamp(static_cast<double>(elapsed) / static_cast<double>(kButtonReleaseTransitionMs), 0.0, 1.0);
}

// Purpose: Advance active UI animations.
// Inputs: None.
// Outputs: Queues another repaint while animation is active or stops the timer.
void MainWindow::tick_animation() {
    const bool page_active = page_transition_progress() < 1.0;
    const bool toggle_active = transition_toggle_ != ToggleId::None &&
                               toggle_transition_start_ != std::chrono::steady_clock::time_point{} &&
                               std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                     toggle_transition_start_)
                                       .count() < kToggleTransitionMs;
    const bool button_release_active =
        button_release_start_ != std::chrono::steady_clock::time_point{} &&
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - button_release_start_)
                .count() < kButtonReleaseTransitionMs;
    if (!page_active) {
        page_transition_start_ = {};
    }
    if (!toggle_active) {
        transition_toggle_ = ToggleId::None;
        toggle_transition_start_ = {};
    }
    if (!button_release_active) {
        button_release_start_ = {};
        button_release_point_ = POINT{-1, -1};
    }
    if (page_active || toggle_active || button_release_active) {
        request_repaint();
        return;
    }
    KillTimer(hwnd_, kAnimationTimer);
    request_repaint();
}

// Purpose: Recreate native fonts at the current monitor DPI.
// Inputs: None; reads `dpi_`.
// Outputs: Replaces owned GDI font handles.
void MainWindow::rebuild_fonts() {
    for (HFONT* font : {&title_font_, &body_font_, &small_font_, &tiny_font_, &mono_font_}) {
        if (*font != nullptr) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
    title_font_ =
        CreateFontW(-MulDiv(22, static_cast<int>(dpi_), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    body_font_ =
        CreateFontW(-MulDiv(12, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    small_font_ =
        CreateFontW(-MulDiv(10, static_cast<int>(dpi_), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    tiny_font_ =
        CreateFontW(-MulDiv(9, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    mono_font_ = CreateFontW(-MulDiv(9, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
                             DEFAULT_PITCH, L"Cascadia Mono");
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
