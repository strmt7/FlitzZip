#pragma once

#include "app/input_limits.hpp"

#include <cstddef>
#include <climits>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>

namespace superzip::app {

namespace detail {

// Purpose: Represent the fixed Win32 DROPFILES wire header without depending on optional shell typedef visibility.
struct DropFilesHeader {
    DWORD pFiles = 0;
    POINT pt{};
    BOOL fNC = FALSE;
    BOOL fWide = FALSE;
};
static_assert(sizeof(DropFilesHeader) == 20U);

class GlobalLockView {
  public:
    // Purpose: Lock a movable Win32 global-memory handle for read-only parsing.
    // Inputs: `handle` is an HGLOBAL owned by the caller.
    // Outputs: Stores the locked pointer and unlocks it when the view is destroyed.
    explicit GlobalLockView(HGLOBAL handle) : handle_(handle), data_(GlobalLock(handle)) {}

    GlobalLockView(const GlobalLockView&) = delete;
    GlobalLockView& operator=(const GlobalLockView&) = delete;

    // Purpose: Release the locked Win32 memory view.
    // Inputs: None.
    // Outputs: Calls `GlobalUnlock` only when the constructor acquired a pointer.
    ~GlobalLockView() {
        if (data_ != nullptr) {
            GlobalUnlock(handle_);
        }
    }

    // Purpose: Expose the locked pointer for parser code.
    // Inputs: None.
    // Outputs: Returns the locked data pointer, or null when locking failed.
    [[nodiscard]] const void* data() const noexcept {
        return data_;
    }

  private:
    HGLOBAL handle_ = nullptr;
    void* data_ = nullptr;
};

// Purpose: Parse a bounded wide null-terminated path list from a DROPFILES payload.
// Inputs: `list` and `limit` delimit trusted copied payload memory; `paths` receives complete paths only.
// Outputs: Returns true only for a terminated list within all path-count and character budgets.
inline bool append_wide_drop_paths(const wchar_t* list, const wchar_t* limit,
                                   std::vector<std::filesystem::path>& paths) {
    const wchar_t* cursor = list;
    std::size_t aggregate_characters = 0U;
    while (cursor < limit) {
        if (*cursor == L'\0') {
            return !paths.empty() || (cursor + 1 < limit && cursor[1] == L'\0');
        }
        const wchar_t* end = cursor;
        while (end < limit && *end != L'\0') {
            ++end;
        }
        if (end >= limit) {
            return false;
        }
        const auto length = static_cast<std::size_t>(end - cursor);
        if (length == 0U || length > kMaxShellPathCharacters || paths.size() >= kMaxQueueItems ||
            length > kMaxShellDropPathCharacters - aggregate_characters) {
            return false;
        }
        paths.emplace_back(std::wstring(cursor, end));
        aggregate_characters += length;
        cursor = end + 1;
    }
    return false;
}

// Purpose: Convert a system-code-page byte string from a DROPFILES payload.
// Inputs: `text` is the byte string and `length` excludes the null terminator.
// Outputs: Returns the converted UTF-16 path, or an empty string if Windows rejects the bytes.
inline std::wstring ansi_drop_path_to_wide(const char* text, std::size_t length) {
    if (text == nullptr || length == 0U || length > static_cast<std::size_t>(INT_MAX)) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, text, static_cast<int>(length), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring converted(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, text, static_cast<int>(length), converted.data(), needed);
    return converted;
}

// Purpose: Parse a bounded ANSI null-terminated path list from a DROPFILES payload.
// Inputs: `list` and `limit` delimit trusted copied payload memory; `paths` receives complete converted paths only.
// Outputs: Returns true only for a terminated list within all path-count and character budgets.
inline bool append_ansi_drop_paths(const char* list, const char* limit, std::vector<std::filesystem::path>& paths) {
    const char* cursor = list;
    std::size_t aggregate_characters = 0U;
    while (cursor < limit) {
        if (*cursor == '\0') {
            return !paths.empty() || (cursor + 1 < limit && cursor[1] == '\0');
        }
        const char* end = cursor;
        while (end < limit && *end != '\0') {
            ++end;
        }
        if (end >= limit) {
            return false;
        }
        const auto byte_length = static_cast<std::size_t>(end - cursor);
        if (byte_length == 0U || byte_length > kMaxShellPathCharacters || paths.size() >= kMaxQueueItems) {
            return false;
        }
        auto path = ansi_drop_path_to_wide(cursor, byte_length);
        if (path.empty() || path.size() > kMaxShellPathCharacters ||
            path.size() > kMaxShellDropPathCharacters - aggregate_characters) {
            return false;
        }
        aggregate_characters += path.size();
        paths.emplace_back(std::move(path));
        cursor = end + 1;
    }
    return false;
}

}  // namespace detail

// Purpose: Parse a Win32 DROPFILES global-memory block into filesystem paths.
// Inputs: `global` is an HGLOBAL carrying a DROPFILES header and double-null path list; ownership stays with caller.
// Outputs: Returns every complete nonempty path, or an empty vector for malformed or unreadable payloads.
inline std::vector<std::filesystem::path> paths_from_dropfiles_global(HGLOBAL global) {
    std::vector<std::filesystem::path> paths;
    if (global == nullptr) {
        return paths;
    }
    const SIZE_T byte_size = GlobalSize(global);
    if (byte_size < sizeof(detail::DropFilesHeader) || byte_size > kMaxShellDropPayloadBytes) {
        return paths;
    }
    std::vector<unsigned char> payload(byte_size);
    {
        const detail::GlobalLockView view(global);
        const auto* bytes = static_cast<const unsigned char*>(view.data());
        if (bytes == nullptr) {
            return paths;
        }
        std::memcpy(payload.data(), bytes, byte_size);
    }
    detail::DropFilesHeader header{};
    std::memcpy(&header, payload.data(), sizeof(header));
    if (header.pFiles < sizeof(detail::DropFilesHeader) || static_cast<SIZE_T>(header.pFiles) >= byte_size) {
        return paths;
    }
    const auto* list = payload.data() + header.pFiles;
    const auto* end = payload.data() + payload.size();
    bool valid = false;
    if (header.fWide) {
        const auto remaining = static_cast<SIZE_T>(end - list);
        if ((header.pFiles % alignof(wchar_t)) != 0U || remaining < (2U * sizeof(wchar_t)) ||
            (remaining % sizeof(wchar_t)) != 0U) {
            return paths;
        }
        valid = detail::append_wide_drop_paths(reinterpret_cast<const wchar_t*>(list),
                                               reinterpret_cast<const wchar_t*>(end), paths);
    } else {
        if (end - list < 2) {
            return paths;
        }
        valid = detail::append_ansi_drop_paths(reinterpret_cast<const char*>(list), reinterpret_cast<const char*>(end),
                                               paths);
    }
    if (!valid) {
        paths.clear();
    }
    return paths;
}

// Purpose: Reject shell-drop paths that could trigger remote authentication or device-path interpretation.
// Inputs: `path` is untrusted lexical shell metadata; this function performs no file or directory probe.
// Outputs: Returns true only for bounded absolute DOS paths rooted on a local Windows drive.
inline bool is_supported_local_drop_path(const std::filesystem::path& path) {
    const auto& value = path.native();
    const auto is_separator = [](wchar_t character) { return character == L'\\' || character == L'/'; };
    const auto is_drive_letter = [](wchar_t character) {
        return (character >= L'A' && character <= L'Z') || (character >= L'a' && character <= L'z');
    };
    if (value.size() < 3U || value.size() > kMaxShellPathCharacters || value.find(L'\0') != std::wstring::npos ||
        !is_drive_letter(value[0]) || value[1] != L':' || !is_separator(value[2]) ||
        value.find(L':', 2U) != std::wstring::npos) {
        return false;
    }
    for (const auto& component : path) {
        if (component == L"." || component == L"..") {
            return false;
        }
    }
    const wchar_t root[] = {value[0], L':', L'\\', L'\0'};
    switch (GetDriveTypeW(root)) {
    case DRIVE_FIXED:
    case DRIVE_REMOVABLE:
    case DRIVE_CDROM:
    case DRIVE_RAMDISK:
        return true;
    default:
        return false;
    }
}

}  // namespace superzip::app
