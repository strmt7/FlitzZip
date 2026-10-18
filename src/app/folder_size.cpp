#include "app/folder_size.hpp"

#include "app/drop_payload.hpp"

#include <limits>

namespace flitzzip::app {

// Purpose: Produce a complete bounded folder total, never an apparently valid partial total.
// Inputs: Local directory, independent shutdown/cancellation signals, and display-work ceilings.
// Outputs: Ordinary-file bytes or unavailable; reparse targets are excluded, access errors invalidate the result.
std::optional<std::uintmax_t> measure_folder_size(const std::filesystem::path& path, const std::atomic_bool& stopped,
                                                  const std::atomic_bool& cancelled, const FolderSizeLimits& limits) {
    if (stopped.load() || cancelled.load() || !is_supported_local_drop_path(path)) {
        return std::nullopt;
    }
    const DWORD root_attributes = GetFileAttributesW(path.c_str());
    if (root_attributes == INVALID_FILE_ATTRIBUTES || (root_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return std::nullopt;
    }
    std::error_code ec;
    std::uintmax_t total = 0;
    std::size_t entry_count = 0U;
    const auto started = std::chrono::steady_clock::now();
    // Permission failures invalidate the whole display total; silently skipping a subtree undercounts it.
    std::filesystem::recursive_directory_iterator iterator(path, std::filesystem::directory_options::none, ec);
    const std::filesystem::recursive_directory_iterator end;
    if (ec) {
        return std::nullopt;
    }
    std::uint32_t entries_since_yield = 0;
    while (iterator != end) {
        if (stopped.load() || cancelled.load() || std::chrono::steady_clock::now() - started >= limits.duration ||
            entry_count >= limits.entries || static_cast<std::size_t>(iterator.depth()) >= limits.depth) {
            return std::nullopt;
        }
        ++entry_count;
        const DWORD attributes = GetFileAttributesW(iterator->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            return std::nullopt;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
                iterator.disable_recursion_pending();
            }
        } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
            const auto size = iterator->file_size(ec);
            if (ec || size > std::numeric_limits<std::uintmax_t>::max() - total) {
                return std::nullopt;
            }
            total += size;
        }
        ec.clear();
        iterator.increment(ec);
        if (ec) {
            return std::nullopt;
        }
        ++entries_since_yield;
        if (entries_since_yield >= 512U) {
            entries_since_yield = 0;
            Sleep(1);
        }
    }
    return total;
}

}  // namespace flitzzip::app
