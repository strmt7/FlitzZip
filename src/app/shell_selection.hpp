#pragma once

#include "app/input_limits.hpp"
#include "core/com_task_string.hpp"

#include <cwchar>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>
#include <shobjidl.h>
#include <wrl/client.h>

namespace flitzzip::app {

// Purpose: Preserve the failed shell operation and native status in an exception.
// Inputs: COM status and a static operation description. Outputs: Throws only for a failed HRESULT.
inline void require_shell_succeeded(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream message;
        message << operation << " failed (0x" << std::hex << static_cast<unsigned long>(result) << ')';
        throw std::runtime_error(message.str());
    }
}

// Purpose: Copy one bounded filesystem path while retaining ownership of every COM string output.
// Inputs: Borrowed shell item. Outputs: Complete nonempty path; malformed or failed results throw.
inline std::filesystem::path shell_item_filesystem_path(IShellItem* item) {
    if (item == nullptr) {
        throw std::runtime_error("Shell selection returned no item");
    }
    CoTaskMemWideString text;
    require_shell_succeeded(item->GetDisplayName(SIGDN_FILESYSPATH, text.put()), "Shell item path lookup");
    if (text.get() == nullptr) {
        throw std::runtime_error("Shell selection returned no filesystem path");
    }
    const auto length = wcsnlen_s(text.get(), kMaxShellPathCharacters + 1U);
    if (length == 0U || length > kMaxShellPathCharacters) {
        throw std::runtime_error("Shell selection returned an empty or oversized filesystem path");
    }
    return std::filesystem::path(std::wstring_view(text.get(), length));
}

// Purpose: Admit the complete shell selection before the caller changes its queue.
// Inputs: Borrowed shell array with COM-owned item results. Outputs: Complete bounded paths or an exception.
inline std::vector<std::filesystem::path> shell_selection_paths(IShellItemArray* items) {
    if (items == nullptr) {
        throw std::runtime_error("File picker returned no selection array");
    }
    DWORD count = 0;
    require_shell_succeeded(items->GetCount(&count), "File picker result count");
    if (count > kMaxQueueItems) {
        throw std::runtime_error("Select at most 4,096 files at a time; no files were added");
    }
    std::vector<std::filesystem::path> paths;
    paths.reserve(count);
    std::size_t characters = 0;
    for (DWORD index = 0; index < count; ++index) {
        Microsoft::WRL::ComPtr<IShellItem> item;
        require_shell_succeeded(items->GetItemAt(index, item.GetAddressOf()), "File picker item lookup");
        auto path = shell_item_filesystem_path(item.Get());
        if (path.native().size() > kMaxShellDropPathCharacters - characters) {
            throw std::runtime_error("Selected filesystem paths exceed the text budget; no files were added");
        }
        characters += path.native().size();
        paths.emplace_back(std::move(path));
    }
    return paths;
}

}  // namespace flitzzip::app
