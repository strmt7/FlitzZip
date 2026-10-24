#include "app/main_window_support.hpp"

#include "app/drop_payload.hpp"
#include "core/com_task_string.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cwchar>
#include <system_error>

#include <knownfolders.h>
#include <shlobj.h>

namespace flitzzip::app {
namespace {

// Purpose: Resolve a Windows known folder without fixed path buffers.
// Inputs: `folder_id` names the known folder and `flags` are passed to SHGetKnownFolderPath.
// Outputs: Returns the resolved path, or an empty path when Windows cannot provide one.
std::filesystem::path known_folder_or_empty(REFKNOWNFOLDERID folder_id, DWORD flags) {
    CoTaskMemWideString value;
    const HRESULT result = SHGetKnownFolderPath(folder_id, flags, nullptr, value.put());
    if (FAILED(result) || value.get() == nullptr || value.get()[0] == L'\0') {
        return {};
    }
    return std::filesystem::path(value.get());
}

}  // namespace

// Purpose: Return the fixed release window style.
// Inputs: None.
// Outputs: Returns a non-resizable Win32 overlapped-window style.
DWORD window_style() {
    return WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
}

// Purpose: Read the optional GUI-smoke auto-close timeout.
// Inputs: Uses `FLITZZIP_GUI_SMOKE_AUTO_CLOSE_MS` from the process environment.
// Outputs: Returns a bounded timer interval in milliseconds, or zero when disabled.
UINT smoke_auto_close_ms() {
    wchar_t buffer[32]{};
    constexpr DWORD capacity = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    const DWORD length = GetEnvironmentVariableW(L"FLITZZIP_GUI_SMOKE_AUTO_CLOSE_MS", buffer, capacity);
    if (length == 0 || length >= capacity) {
        return 0;
    }
    if (!std::all_of(buffer, buffer + length, [](wchar_t value) { return value >= L'0' && value <= L'9'; })) {
        return 0;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(buffer, &end, 10);
    if (errno == ERANGE || end != buffer + length || parsed == 0) {
        return 0;
    }
    return static_cast<UINT>(std::clamp<unsigned long>(parsed, 5000UL, USER_TIMER_MAXIMUM));
}

// Purpose: Read the optional GUI-smoke close-marker file path.
// Inputs: Uses `FLITZZIP_GUI_SMOKE_CLOSE_FILE` from the process environment.
// Outputs: Returns the marker path, or an empty path when smoke close polling is disabled.
std::filesystem::path smoke_close_marker_path() {
    wchar_t buffer[32768]{};
    constexpr DWORD capacity = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    const DWORD length = GetEnvironmentVariableW(L"FLITZZIP_GUI_SMOKE_CLOSE_FILE", buffer, capacity);
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

// Purpose: Resolve the current user's Downloads known folder for archive output defaults.
// Inputs: None; asks Windows for the interactive user's Downloads folder.
// Outputs: Returns a Downloads-like known folder path; never falls back to the process current directory.
std::filesystem::path current_user_downloads_directory() {
    auto downloads = known_folder_or_empty(FOLDERID_Downloads, KF_FLAG_DEFAULT);
    if (!downloads.empty()) {
        return downloads;
    }

    auto profile = known_folder_or_empty(FOLDERID_Profile, KF_FLAG_DEFAULT);
    if (!profile.empty()) {
        return profile / L"Downloads";
    }

    auto public_downloads = known_folder_or_empty(FOLDERID_PublicDownloads, KF_FLAG_DEFAULT);
    if (!public_downloads.empty()) {
        return public_downloads;
    }

    auto local_app_data = known_folder_or_empty(FOLDERID_LocalAppData, KF_FLAG_CREATE);
    if (!local_app_data.empty()) {
        return local_app_data / L"FlitzZip" / L"Downloads";
    }

    std::error_code ec;
    auto temp = std::filesystem::temp_directory_path(ec);
    if (!ec && !temp.empty()) {
        return temp / L"FlitzZip" / L"Downloads";
    }

    return std::filesystem::path(L"C:\\Users\\Public\\Downloads");
}

// Purpose: Convert a shell HDROP payload into filesystem paths.
// Inputs: `drop` is a valid HDROP handle owned by the caller.
// Outputs: Returns only a complete, bounded DROPFILES list; malformed payloads fail closed.
std::vector<std::filesystem::path> paths_from_hdrop(HDROP drop) {
    if (drop == nullptr) {
        return {};
    }
    return paths_from_dropfiles_global(reinterpret_cast<HGLOBAL>(drop));
}

}  // namespace flitzzip::app
