#pragma once

#include "app/input_limits.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace flitzzip::app {

// Purpose: Bound optional display metadata work independently of archive jobs.
// Inputs: Entry/depth ceilings and a steady-clock duration; defaults preserve Queue limits.
// Outputs: A traversal budget; reaching a limit makes the size unavailable, never partial.
struct FolderSizeLimits {
    std::size_t entries = kMaxFolderSizeEntries;
    std::size_t depth = kMaxFolderSizeDepth;
    std::chrono::steady_clock::duration duration = kMaxFolderSizeDuration;
};

// Purpose: Measure local folder metadata without following reparse points or skipping access failures.
// Inputs: Borrowed path and lifetime-safe stop/cancel flags, plus the optional-work budget.
// Outputs: Complete ordinary-file bytes, or empty on cancellation, inaccessible input or exhausted budget.
// Allocation failures propagate to the owning worker; a partial size is never returned as complete.
[[nodiscard]] std::optional<std::uintmax_t> measure_folder_size(const std::filesystem::path& path,
                                                                const std::atomic_bool& stopped,
                                                                const std::atomic_bool& cancelled,
                                                                const FolderSizeLimits& limits = {});

}  // namespace flitzzip::app
