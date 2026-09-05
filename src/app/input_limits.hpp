#pragma once

#include <chrono>
#include <cstddef>

namespace superzip::app {

// Shell and Queue limits bound memory reserved from picker and drag/drop metadata.
inline constexpr std::size_t kMaxQueueItems = 4'096U;
inline constexpr std::size_t kMaxShellDropPayloadBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kMaxShellPathCharacters = 32'767U;
inline constexpr std::size_t kMaxShellDropPathCharacters = 4U * 1024U * 1024U;

// Folder-size limits keep optional Queue metadata work responsive and finite.
inline constexpr std::size_t kMaxFolderSizeEntries = 100'000U;
inline constexpr std::size_t kMaxFolderSizeDepth = 256U;
inline constexpr auto kMaxFolderSizeDuration = std::chrono::seconds(2);

}  // namespace superzip::app
