#pragma once

#include <cstddef>
#include <climits>
#include <filesystem>
#include <string>
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

// Purpose: Append a wide null-terminated path list from a DROPFILES payload.
// Inputs: `list` points to the first UTF-16 path string and `limit` is the byte limit of the global block.
// Outputs: Appends nonempty filesystem paths until the terminating empty string or limit is reached.
inline void append_wide_drop_paths(const wchar_t* list, const wchar_t* limit,
                                   std::vector<std::filesystem::path>& paths) {
    const wchar_t* cursor = list;
    while (cursor < limit && *cursor != L'\0') {
        const wchar_t* end = cursor;
        while (end < limit && *end != L'\0') {
            ++end;
        }
        if (end >= limit) {
            return;
        }
        if (end != cursor) {
            paths.emplace_back(std::wstring(cursor, end));
        }
        cursor = end + 1;
    }
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

// Purpose: Append an ANSI null-terminated path list from a DROPFILES payload.
// Inputs: `list` points to the first ANSI path string and `limit` is the byte limit of the global block.
// Outputs: Appends converted nonempty filesystem paths until the terminating empty string or limit is reached.
inline void append_ansi_drop_paths(const char* list, const char* limit, std::vector<std::filesystem::path>& paths) {
    const char* cursor = list;
    while (cursor < limit && *cursor != '\0') {
        const char* end = cursor;
        while (end < limit && *end != '\0') {
            ++end;
        }
        if (end >= limit) {
            return;
        }
        auto path = ansi_drop_path_to_wide(cursor, static_cast<std::size_t>(end - cursor));
        if (!path.empty()) {
            paths.emplace_back(std::move(path));
        }
        cursor = end + 1;
    }
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
    if (byte_size < sizeof(detail::DropFilesHeader)) {
        return paths;
    }
    const detail::GlobalLockView view(global);
    const auto* bytes = static_cast<const unsigned char*>(view.data());
    if (bytes == nullptr) {
        return paths;
    }
    const auto* header = reinterpret_cast<const detail::DropFilesHeader*>(bytes);
    if (header->pFiles < sizeof(detail::DropFilesHeader) || static_cast<SIZE_T>(header->pFiles) >= byte_size) {
        return paths;
    }
    const auto* list = bytes + header->pFiles;
    const auto* end = bytes + byte_size;
    if (header->fWide) {
        const auto remaining = static_cast<SIZE_T>(end - list);
        if (remaining < sizeof(wchar_t) || (remaining % sizeof(wchar_t)) != 0U) {
            return paths;
        }
        detail::append_wide_drop_paths(reinterpret_cast<const wchar_t*>(list), reinterpret_cast<const wchar_t*>(end),
                                       paths);
    } else {
        detail::append_ansi_drop_paths(reinterpret_cast<const char*>(list), reinterpret_cast<const char*>(end), paths);
    }
    return paths;
}

}  // namespace superzip::app
