#pragma once

#include "app/input_limits.hpp"
#include "core/archive_format.hpp"
#include "core/result.hpp"

#include <Windows.h>

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flitzzip::app {
namespace extraction_detail {

struct FolderNameLess {
    // Purpose: Give batch destinations the same case-insensitive identity used by Windows path admission.
    // Inputs: Nonempty, NUL-free folder components produced by the planner.
    // Outputs: A strict ordinal ordering; failed platform comparison aborts planning before extraction.
    bool operator()(const std::wstring& left, const std::wstring& right) const {
        const int compared = CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE);
        if (compared == 0) {
            throw ArchiveError("extraction folder names cannot be compared safely on Windows");
        }
        return compared == CSTR_LESS_THAN;
    }
};

// Purpose: Remove forbidden characters and trailing dots/spaces from a derived extraction component.
// Inputs: A filename stem from a selected archive; no filesystem objects are created or opened.
// Outputs: A nonempty component, with the stable name archive when no visible stem remains.
inline std::wstring sanitize_folder_component(std::wstring value) {
    for (wchar_t& character : value) {
        if (character < 32 || std::wstring_view(L"<>:\"/\\|?*").find(character) != std::wstring_view::npos) {
            character = L'_';
        }
    }
    while (!value.empty() && (value.back() == L'.' || value.back() == L' ')) {
        value.pop_back();
    }
    return value.empty() ? L"archive" : value;
}

// Purpose: Fold registered ASCII extension bytes without altering Unicode filename characters.
// Inputs: A filename or registered extension. Outputs: Only ASCII uppercase letters are lowercased.
inline std::wstring ascii_lower(std::wstring value) {
    for (wchar_t& character : value) {
        if (character >= L'A' && character <= L'Z') {
            character = static_cast<wchar_t>(character - L'A' + L'a');
        }
    }
    return value;
}

// Purpose: Remove the complete registered extension when deriving a batch extraction directory.
// Inputs: One archive path; registered extensions come from the canonical format registry.
// Outputs: A sanitized nonempty base; unknown extensions retain the existing final-stem behavior.
inline std::wstring folder_base(const std::filesystem::path& archive) {
    auto filename = archive.filename().wstring();
    const auto format = detect_archive_format_by_extension(archive);
    const std::string_view extension(archive_format_extension_info_for_path(format, archive).extension);
    const std::wstring wide_extension(extension.begin(), extension.end());
    const auto folded = ascii_lower(filename);
    if (!extension.empty() && filename.size() > extension.size() && folded.ends_with(ascii_lower(wide_extension))) {
        filename.resize(filename.size() - extension.size());
    } else {
        filename = archive.stem().wstring();
    }
    return sanitize_folder_component(std::move(filename));
}

// Purpose: Reserve suffix space within a Windows filename component without splitting a UTF-16 pair.
// Inputs: A sanitized base and a limit of at least seven UTF-16 code units.
// Outputs: A nonempty component within the requested limit, with no new trailing dots/spaces.
inline std::wstring fit_folder_component(std::wstring value, std::size_t limit) {
    if (value.size() > limit) {
        value.resize(limit);
        if (!value.empty() && value.back() >= 0xd800 && value.back() <= 0xdbff) {
            value.pop_back();
        }
    }
    return sanitize_folder_component(std::move(value));
}

}  // namespace extraction_detail

// Purpose: Allocate distinct Windows folder names for every archive before a batch starts writing.
// Inputs: At most the admitted Queue capacity of archives and the captured common destination.
// Outputs: One path per archive; a single archive preserves the root, and an empty batch has no outputs.
inline std::vector<std::filesystem::path>
extraction_outputs_for_archives(std::span<const std::filesystem::path> archives, const std::filesystem::path& root) {
    if (archives.size() > kMaxQueueItems) {
        throw ArchiveError("extraction batch exceeds the Queue item limit");
    }
    if (archives.empty()) {
        return {};
    }
    if (archives.size() == 1U) {
        return {root};
    }
    constexpr std::size_t component_limit = 255U;
    std::vector<std::filesystem::path> outputs;
    outputs.reserve(archives.size());
    std::map<std::wstring, std::size_t, extraction_detail::FolderNameLess> assigned;
    for (const auto& archive : archives) {
        const auto base = extraction_detail::folder_base(archive);
        auto candidate = extraction_detail::fit_folder_component(base, component_limit);
        const auto [position, inserted] = assigned.try_emplace(candidate, 2U);
        if (!inserted) {
            do {
                const auto tail = L" (" + std::to_wstring(position->second++) + L")";
                candidate = extraction_detail::fit_folder_component(base, component_limit - tail.size()) + tail;
            } while (!assigned.try_emplace(candidate, 2U).second);
        }
        outputs.emplace_back(root / candidate);
    }
    return outputs;
}

}  // namespace flitzzip::app
