#pragma once

#include <cstdint>

namespace superzip {

// Purpose: Carry one volatile physical-memory snapshot, not a reservation against other processes.
// Inputs: Total and currently available physical bytes reported by the OS.
// Outputs: A value passed to the shared admission policy without additional OS queries.
struct HostMemorySnapshot {
    std::uint64_t total_bytes = 0;
    std::uint64_t available_bytes = 0;
};

// Purpose: Describe additional native encode storage beyond three payload buffers per window.
// Inputs: Per-window metadata and per-worker codec bytes with bounded concurrency ceilings.
// Outputs: An allocation-free admission estimate; zero values preserve buffer-only admission.
struct HostPipelineWorkspace {
    std::uint64_t per_window_bytes = 0;
    std::uint64_t per_worker_bytes = 0;
    std::uint32_t aggregate_workers = 0;
    std::uint32_t workers_per_window = 0;
};

// Purpose: Query physical RAM without inventing capacity when the OS counter fails.
// Inputs: None.
// Outputs: Returns the current snapshot or throws ArchiveError when it cannot be obtained.
HostMemorySnapshot query_host_memory_snapshot();

// Purpose: Compute additional physical RAM permitted by the host usage target.
// Inputs: memory is an OS snapshot; zero total or available greater than total is invalid.
// Outputs: Returns zero at/above the target, or throws ArchiveError for invalid counters.
std::uint64_t safe_host_memory_growth_bytes(HostMemorySnapshot memory);

// Purpose: Admit native pipeline buffers and supplied workspace in production and RAM validation.
// Inputs: memory is a snapshot; chunk_size, caller reserve, and workspace bound additional storage.
// Outputs: Returns a capped nonzero depth or throws ArchiveError if one complete estimate cannot fit.
// This snapshot-based estimate does not reserve memory against unrelated processes.
std::uint32_t resolve_host_pipeline_inflight_limit(HostMemorySnapshot memory, std::uint64_t chunk_size,
                                                   std::uint64_t reserve_bytes = 0,
                                                   HostPipelineWorkspace workspace = {});

}  // namespace superzip
