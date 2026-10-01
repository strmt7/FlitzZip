#include "core/host_memory_budget.hpp"

#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace superzip {

// Purpose: Query physical RAM without inventing capacity when the OS counter fails.
// Inputs: None.
// Outputs: Returns the current snapshot or throws ArchiveError with the OS failure code.
HostMemorySnapshot query_host_memory_snapshot() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) == 0) {
        const auto error = GetLastError();
        throw ArchiveError("cannot query physical memory (Windows error " + std::to_string(error) + ")");
    }
    return HostMemorySnapshot{
        .total_bytes = static_cast<std::uint64_t>(status.ullTotalPhys),
        .available_bytes = static_cast<std::uint64_t>(status.ullAvailPhys),
    };
#else
    throw ArchiveError("physical memory admission is unavailable on this platform");
#endif
}

// Purpose: Compute additional physical RAM permitted by the host usage target without overflow.
// Inputs: memory contains total and available physical bytes, with available no greater than total.
// Outputs: Returns the remaining target allowance or throws ArchiveError for invalid counters.
std::uint64_t safe_host_memory_growth_bytes(HostMemorySnapshot memory) {
    if (memory.total_bytes == 0 || memory.available_bytes > memory.total_bytes) {
        throw ArchiveError("physical memory counters are invalid; cannot admit archive processing");
    }
    const auto used = memory.total_bytes - memory.available_bytes;
    const auto target = (memory.total_bytes / 100U) * kHostMemoryTargetUsagePercent +
                        ((memory.total_bytes % 100U) * kHostMemoryTargetUsagePercent) / 100U;
    return used >= target ? 0 : target - used;
}

// Purpose: Apply shared three-buffer native window admission without a forced one-window bypass.
// Inputs: memory is a snapshot, chunk_size is 1..kMaxArchiveChunkBytes, reserve_bytes is caller overhead.
// Outputs: Returns 1..kMaxInflightArchiveChunks; throws ArchiveError when a window cannot be admitted.
std::uint32_t resolve_host_pipeline_inflight_limit(HostMemorySnapshot memory, std::uint64_t chunk_size,
                                                   std::uint64_t reserve_bytes) {
    if (chunk_size == 0 || chunk_size > kMaxArchiveChunkBytes) {
        throw ArchiveError("pipeline chunk size is outside SuperZip resource limits");
    }
    const auto growth = safe_host_memory_growth_bytes(memory);
    const auto remaining = growth > reserve_bytes ? std::min(growth - reserve_bytes, kMaxPipelineMemoryBytes) : 0;
    const auto per_chunk = chunk_size * 3U;
    const auto limit = std::min<std::uint64_t>(remaining / per_chunk, kMaxInflightArchiveChunks);
    if (limit == 0) {
        throw ArchiveError("insufficient physical memory for one archive window within the 80% host RAM target; "
                           "retry when more memory is available");
    }
    return static_cast<std::uint32_t>(limit);
}

}  // namespace superzip
