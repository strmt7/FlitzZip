#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace superzip {

struct GpuTelemetry;

namespace sparse_pattern {

struct SparseCandidate {
    std::uint32_t source_offset = 0;
    std::uint32_t input_bytes = 0;
    std::uint32_t period = 0;
    std::uint32_t max_patches = 0;
    std::uint32_t positions_offset = 0;
    std::uint32_t patch_count = 0;
};

// Purpose: Collect exact motif mismatches for many source blocks with two bounded HIP launches.
// Inputs: `device_input` is a live `input_bytes` allocation; candidates have validated nonoverlapping source extents.
// Outputs: Returns one unordered list per candidate, empty when no smaller sparse payload is possible.
std::vector<std::vector<std::uint32_t>> collect_positions_device_batch(const std::byte* device_input,
                                                                       std::uint32_t input_bytes,
                                                                       std::span<const SparseCandidate> candidates,
                                                                       GpuTelemetry* telemetry);

}  // namespace sparse_pattern
}  // namespace superzip
