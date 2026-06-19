#include "app/main_window_support.hpp"

#include "app/drop_payload.hpp"

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <system_error>

#include <knownfolders.h>
#include <shlobj.h>

namespace superzip::app {

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

// Purpose: Resolve the current user's Downloads known folder for archive output defaults.
// Inputs: None; asks Windows for the interactive user's Downloads folder.
// Outputs: Returns Downloads when available, `%USERPROFILE%\Downloads` as a secondary fallback, or the current path.
std::filesystem::path current_user_downloads_directory() {
    PWSTR downloads = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &downloads);
    if (SUCCEEDED(result) && downloads != nullptr && downloads[0] != L'\0') {
        std::filesystem::path path(downloads);
        CoTaskMemFree(downloads);
        return path;
    }
    if (downloads != nullptr) {
        CoTaskMemFree(downloads);
    }

    wchar_t profile[MAX_PATH]{};
    constexpr DWORD profile_capacity = static_cast<DWORD>(sizeof(profile) / sizeof(profile[0]));
    const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", profile, profile_capacity);
    if (length > 0 && length < profile_capacity) {
        return std::filesystem::path(profile) / L"Downloads";
    }
    return safe_current_path();
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

}  // namespace superzip::app