#include "gpu/hip_kernel_api.hpp"

namespace superzip::sparse_pattern {
namespace {

using namespace hip_detail;

// Purpose: Count mismatches for every admitted block in one HIP submission.
// Inputs: Resident input and validated candidate extents; `counts` is zeroed device memory.
// Outputs: Stores one exact mismatch count per candidate without retaining source-sized buffers.
__global__ void count_sparse_positions_kernel(const std::byte* input, const SparseCandidate* candidates,
                                              std::uint32_t* counts) {
    const auto index = static_cast<std::uint32_t>(blockIdx.y);
    const auto candidate = candidates[index];
    const auto tile_start = static_cast<std::uint32_t>(blockIdx.x) * kSparseKernelTileBytes;
    if (tile_start >= candidate.input_bytes) {
        return;
    }
    const auto tile_end = min(tile_start + kSparseKernelTileBytes, candidate.input_bytes);
    const auto* block = input + candidate.source_offset;
    const auto stride = blockDim.x % candidate.period;
    auto motif_offset = (tile_start + threadIdx.x) % candidate.period;
    for (std::uint32_t position = tile_start + threadIdx.x; position < tile_end; position += blockDim.x) {
        if (position >= candidate.period && block[position] != block[motif_offset]) {
            atomicAdd(counts + index, 1U);
        }
        motif_offset += stride;
        if (motif_offset >= candidate.period) {
            motif_offset -= candidate.period;
        }
    }
}

// Purpose: Gather only byte-saving candidate patches into exact, disjoint output ranges.
// Inputs: `candidates` carries host-admitted patch counts and offsets; `cursors` is zeroed device memory.
// Outputs: Writes at most the admitted patch count per candidate for host canonicalization.
__global__ void gather_sparse_positions_kernel(const std::byte* input, const SparseCandidate* candidates,
                                               std::uint32_t* cursors, std::uint32_t* positions) {
    const auto index = static_cast<std::uint32_t>(blockIdx.y);
    const auto candidate = candidates[index];
    if (candidate.patch_count == 0U) {
        return;
    }
    const auto tile_start = static_cast<std::uint32_t>(blockIdx.x) * kSparseKernelTileBytes;
    if (tile_start >= candidate.input_bytes) {
        return;
    }
    const auto tile_end = min(tile_start + kSparseKernelTileBytes, candidate.input_bytes);
    const auto* block = input + candidate.source_offset;
    const auto stride = blockDim.x % candidate.period;
    auto motif_offset = (tile_start + threadIdx.x) % candidate.period;
    for (std::uint32_t position = tile_start + threadIdx.x; position < tile_end; position += blockDim.x) {
        if (position >= candidate.period && block[position] != block[motif_offset]) {
            const auto slot = atomicAdd(cursors + index, 1U);
            if (slot < candidate.patch_count) {
                positions[candidate.positions_offset + slot] = position;
            }
        }
        motif_offset += stride;
        if (motif_offset >= candidate.period) {
            motif_offset -= candidate.period;
        }
    }
}

}  // namespace

// Purpose: Bind this translation unit's registered kernels to the private POD dispatch table.
// Inputs: Module-owned table under construction after DLL registration.
// Outputs: Writes only this component's typed entrypoints; allocates no storage.
void bind_sparse_kernels(HipKernelApi& api) noexcept {
    api.count_sparse_positions_kernel = count_sparse_positions_kernel;
    api.gather_sparse_positions_kernel = gather_sparse_positions_kernel;
}

}  // namespace superzip::sparse_pattern
