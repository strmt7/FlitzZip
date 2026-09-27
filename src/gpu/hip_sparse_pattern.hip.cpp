#include "gpu/sparse_pattern_device.hpp"

#include "gpu/hip_codec_support.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace superzip::sparse_pattern {
namespace {

using namespace hip_detail;

constexpr std::uint32_t kThreads = 256U;

// Purpose: Count mismatches for every admitted block in one HIP submission.
// Inputs: Resident input and validated candidate extents; `counts` is zeroed device memory.
// Outputs: Stores one exact mismatch count per candidate without retaining source-sized buffers.
__global__ void count_sparse_positions_kernel(const std::byte* input, const SparseCandidate* candidates,
                                              std::uint32_t* counts) {
    const auto index = static_cast<std::uint32_t>(blockIdx.x);
    const auto candidate = candidates[index];
    const auto* block = input + candidate.source_offset;
    for (std::uint32_t position = candidate.period + threadIdx.x; position < candidate.input_bytes;
         position += blockDim.x) {
        if (block[position] != block[position % candidate.period]) {
            atomicAdd(counts + index, 1U);
        }
    }
}

// Purpose: Gather only byte-saving candidate patches into exact, disjoint output ranges.
// Inputs: `candidates` carries host-admitted patch counts and offsets; `cursors` is zeroed device memory.
// Outputs: Writes at most the admitted patch count per candidate for host canonicalization.
__global__ void gather_sparse_positions_kernel(const std::byte* input, const SparseCandidate* candidates,
                                               std::uint32_t* cursors, std::uint32_t* positions) {
    const auto index = static_cast<std::uint32_t>(blockIdx.x);
    const auto candidate = candidates[index];
    if (candidate.patch_count == 0U) {
        return;
    }
    const auto* block = input + candidate.source_offset;
    for (std::uint32_t position = candidate.period + threadIdx.x; position < candidate.input_bytes;
         position += blockDim.x) {
        if (block[position] == block[position % candidate.period]) {
            continue;
        }
        const auto slot = atomicAdd(cursors + index, 1U);
        if (slot < candidate.patch_count) {
            positions[candidate.positions_offset + slot] = position;
        }
    }
}

// Purpose: Validate host candidate bounds before any device allocation or kernel submission.
// Inputs: `input_bytes` describes the borrowed device allocation and `candidates` are ordered source ranges.
// Outputs: Returns for disjoint, bounded candidates; otherwise throws `GpuError`.
void validate_sparse_candidates(std::uint32_t input_bytes, std::span<const SparseCandidate> candidates) {
    std::uint32_t previous_end = 0U;
    for (const auto& candidate : candidates) {
        if (candidate.source_offset < previous_end || candidate.source_offset > input_bytes ||
            candidate.input_bytes > input_bytes - candidate.source_offset ||
            candidate.input_bytes > kMaxArchiveBlockBytes || candidate.period < 2U ||
            candidate.period > kMaxGpuPatternBytes || candidate.period >= candidate.input_bytes ||
            candidate.max_patches == 0U || candidate.max_patches > candidate.input_bytes - candidate.period ||
            candidate.positions_offset != 0U || candidate.patch_count != 0U) {
            throw GpuError("sparse pattern HIP batch candidate bounds are invalid");
        }
        previous_end = candidate.source_offset + candidate.input_bytes;
    }
}

}  // namespace

// Purpose: Collect exact, size-admitted mismatch lists for all sparse candidates in a chunk.
// Inputs: A resident HIP chunk, its byte extent, ordered candidates, and optional telemetry.
// Outputs: Returns aligned unordered patch lists with two launches at most; no candidate is silently truncated.
std::vector<std::vector<std::uint32_t>> collect_positions_device_batch(const std::byte* device_input,
                                                                       std::uint32_t input_bytes,
                                                                       std::span<const SparseCandidate> candidates,
                                                                       GpuTelemetry* telemetry) {
    if (device_input == nullptr || input_bytes > kMaxArchiveChunkBytes ||
        candidates.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw GpuError("sparse pattern HIP batch input bounds are invalid");
    }
    validate_sparse_candidates(input_bytes, candidates);
    std::vector<std::vector<std::uint32_t>> result(candidates.size());
    if (candidates.empty()) {
        return result;
    }

    const auto candidate_bytes =
        checked_multiply_bytes(candidates.size(), sizeof(SparseCandidate), "sparse candidates");
    const auto count_bytes = checked_multiply_bytes(candidates.size(), sizeof(std::uint32_t), "sparse counts");
    HipDeviceMemoryReservation count_reservation(
        checked_add_bytes(candidate_bytes, count_bytes, "sparse count workspace"), "sparse count workspace");
    HipDeviceBuffer<SparseCandidate> device_candidates(candidate_bytes, "hipMalloc sparse candidates");
    HipDeviceBuffer<std::uint32_t> device_counts(count_bytes, "hipMalloc sparse counts");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(candidate_bytes + count_bytes));
    check_hip(hipMemcpy(device_candidates.get(), candidates.data(), candidate_bytes, hipMemcpyHostToDevice),
              "hipMemcpy sparse candidates");
    record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(candidate_bytes));
    check_hip(hipMemset(device_counts.get(), 0, count_bytes), "hipMemset sparse counts");
    auto events = make_hip_event_pair("create sparse count events");
    launch_measured_kernel(count_sparse_positions_kernel, static_cast<unsigned int>(candidates.size()), kThreads, 0,
                           hipStreamPerThread, events, "launch sparse count kernel", device_input,
                           device_candidates.get(), device_counts.get());
    finish_measured_kernel(telemetry, events, "synchronize sparse count kernel");

    std::vector<std::uint32_t> counts(candidates.size());
    check_hip(hipMemcpy(counts.data(), device_counts.get(), count_bytes, hipMemcpyDeviceToHost),
              "hipMemcpy sparse counts");
    record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(count_bytes));
    std::vector<SparseCandidate> admitted(candidates.begin(), candidates.end());
    std::uint64_t total_positions = 0U;
    for (std::size_t index = 0U; index < admitted.size(); ++index) {
        auto& candidate = admitted[index];
        candidate.positions_offset = 0U;
        candidate.patch_count = 0U;
        if (counts[index] == 0U || counts[index] > candidate.max_patches) {
            continue;
        }
        if (total_positions > std::numeric_limits<std::uint32_t>::max() - counts[index]) {
            throw GpuError("sparse pattern HIP patch table exceeds 32-bit layout");
        }
        candidate.positions_offset = static_cast<std::uint32_t>(total_positions);
        candidate.patch_count = counts[index];
        total_positions += counts[index];
    }
    if (total_positions == 0U) {
        return result;
    }

    const auto position_bytes =
        checked_multiply_bytes(static_cast<std::size_t>(total_positions), sizeof(std::uint32_t), "sparse positions");
    HipDeviceMemoryReservation gather_reservation(
        checked_add_bytes(position_bytes, count_bytes, "sparse gather workspace"), "sparse gather workspace");
    HipDeviceBuffer<std::uint32_t> device_positions(position_bytes, "hipMalloc sparse positions");
    HipDeviceBuffer<std::uint32_t> device_cursors(count_bytes, "hipMalloc sparse cursors");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(position_bytes + count_bytes));
    check_hip(hipMemcpy(device_candidates.get(), admitted.data(), candidate_bytes, hipMemcpyHostToDevice),
              "hipMemcpy admitted sparse candidates");
    record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(candidate_bytes));
    check_hip(hipMemset(device_cursors.get(), 0, count_bytes), "hipMemset sparse cursors");
    events = make_hip_event_pair("create sparse gather events");
    launch_measured_kernel(gather_sparse_positions_kernel, static_cast<unsigned int>(candidates.size()), kThreads, 0,
                           hipStreamPerThread, events, "launch sparse gather kernel", device_input,
                           device_candidates.get(), device_cursors.get(), device_positions.get());
    finish_measured_kernel(telemetry, events, "synchronize sparse gather kernel");

    std::vector<std::uint32_t> cursors(candidates.size());
    check_hip(hipMemcpy(cursors.data(), device_cursors.get(), count_bytes, hipMemcpyDeviceToHost),
              "hipMemcpy sparse cursors");
    record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(count_bytes));
    for (std::size_t index = 0U; index < admitted.size(); ++index) {
        if (admitted[index].patch_count != 0U && cursors[index] != admitted[index].patch_count) {
            throw GpuError("sparse pattern HIP gather count changed between passes");
        }
    }
    std::vector<std::uint32_t> positions(static_cast<std::size_t>(total_positions));
    check_hip(hipMemcpy(positions.data(), device_positions.get(), position_bytes, hipMemcpyDeviceToHost),
              "hipMemcpy sparse positions");
    record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(position_bytes));
    for (std::size_t index = 0U; index < admitted.size(); ++index) {
        const auto& candidate = admitted[index];
        if (candidate.patch_count == 0U) {
            continue;
        }
        const auto begin = positions.begin() + candidate.positions_offset;
        result[index].assign(begin, begin + candidate.patch_count);
    }
    device_cursors.reset_checked("hipFree sparse cursors");
    device_positions.reset_checked("hipFree sparse positions");
    device_counts.reset_checked("hipFree sparse counts");
    device_candidates.reset_checked("hipFree sparse candidates");
    return result;
}

}  // namespace superzip::sparse_pattern
