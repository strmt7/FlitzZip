#pragma once

#include <algorithm>
#include <cstdint>

namespace superzip {

// Purpose: Compute the existing per-window CPU worker share without arithmetic overflow.
// Inputs: workers/inflight are admitted counts; windows is the number of available work windows.
// Outputs: Returns the ceiling share with one worker minimum; pipeline admission separately controls active windows.
inline std::uint32_t resolve_codec_worker_count(std::uint32_t workers, std::uint32_t inflight,
                                                std::uint64_t windows) noexcept {
    workers = std::max(1U, workers);
    const auto active = std::max<std::uint64_t>(1U, std::min<std::uint64_t>(inflight, windows));
    const auto share = (static_cast<std::uint64_t>(workers) + active - 1U) / active;
    return static_cast<std::uint32_t>(std::max<std::uint64_t>(1U, std::min<std::uint64_t>(workers, share)));
}

}  // namespace superzip
