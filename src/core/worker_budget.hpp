#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace superzip {

// Purpose: Bound active codec windows by the aggregate worker budget and available work.
// Inputs: workers/inflight are upper bounds; windows is available work, with zero normalized to one for empty entries.
// Outputs: Returns a positive queue limit no greater than normalized workers, inflight, or windows.
inline std::uint32_t
resolve_worker_inflight_limit(std::uint32_t workers, std::uint32_t inflight,
                              std::uint64_t windows = std::numeric_limits<std::uint64_t>::max()) noexcept {
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>({std::max(1U, workers), std::max(1U, inflight), std::max<std::uint64_t>(1U, windows)}));
}

// Purpose: Divide the aggregate CPU budget without oversubscribing concurrently active codec windows.
// Inputs: workers/inflight are upper bounds; windows is available work. Callers must also apply the shared queue limit.
// Outputs: Returns a positive floor share; share times admitted active windows never exceeds normalized workers.
inline std::uint32_t resolve_codec_worker_count(std::uint32_t workers, std::uint32_t inflight,
                                                std::uint64_t windows) noexcept {
    workers = std::max(1U, workers);
    return workers / resolve_worker_inflight_limit(workers, inflight, windows);
}

}  // namespace superzip
