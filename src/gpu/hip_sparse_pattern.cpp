#include "gpu/hip_kernel_api.hpp"
#include "gpu/sparse_pattern_device.hpp"

#include "gpu/hip_codec_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace superzip::sparse_pattern {
namespace {

using namespace hip_detail;

// Purpose: Validate host candidate bounds before any device allocation or kernel submission.
// Inputs: `input_bytes` describes the borrowed device allocation and `candidates` are ordered source ranges.
// Outputs: Returns for disjoint, bounded candidates; otherwise throws `GpuError`.
void validate_sparse_candidates(std::uint32_t input_bytes, std::span<const SparseCandidate> candidates) {
    std::uint32_t previous_end = 0U;
    for (const auto& candidate : candidates) {
        if (candidate.source_offset < previous_end || candidate.source_offset > input_bytes ||
            candidate.input_bytes > input_bytes - candidate.source_offset ||
            candidate.input_bytes > kMaxArchiveBlockBytes || candidate.period < 2U ||
            candidate.period > kMaxGpuLongSparsePatternBytes || candidate.period >= candidate.input_bytes ||
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
// Ownership: One operation-owned event pair measures completed intervals without sharing handles.
std::vector<std::vector<std::uint32_t>> collect_positions_device_batch(const std::byte* device_input,
                                                                       std::uint32_t input_bytes,
                                                                       std::span<const SparseCandidate> candidates,
                                                                       GpuTelemetry* telemetry) {
    if (device_input == nullptr || input_bytes > kMaxArchiveChunkBytes ||
        candidates.size() > kMaxArchiveChunkBytes / kMinArchiveBlockBytes) {
        throw GpuError("sparse pattern HIP batch input bounds are invalid");
    }
    validate_sparse_candidates(input_bytes, candidates);
    std::vector<std::vector<std::uint32_t>> result(candidates.size());
    if (candidates.empty()) {
        return result;
    }
    std::uint32_t max_candidate_bytes = 0U;
    for (const auto& candidate : candidates) {
        max_candidate_bytes = std::max(max_candidate_bytes, candidate.input_bytes);
    }
    const dim3 grid((max_candidate_bytes + kSparseKernelTileBytes - 1U) / kSparseKernelTileBytes,
                    static_cast<unsigned int>(candidates.size()));

    const auto candidate_bytes =
        checked_multiply_bytes(candidates.size(), sizeof(SparseCandidate), "sparse candidates");
    const auto count_bytes = checked_multiply_bytes(candidates.size(), sizeof(std::uint32_t), "sparse counts");
    // The count phase reserves only compact candidate metadata, not a source-sized duplicate.
    HipDeviceMemoryReservation count_reservation(
        checked_add_bytes(candidate_bytes, count_bytes, "sparse count workspace"), "sparse count workspace");
    HipDeviceBuffer<SparseCandidate> device_candidates(candidate_bytes, "hipMalloc sparse candidates");
    HipDeviceBuffer<std::uint32_t> device_counts(count_bytes, "hipMalloc sparse counts");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(candidate_bytes + count_bytes));
    check_hip(copy_on_codec_stream(device_candidates.get(), candidates.data(), candidate_bytes, hipMemcpyHostToDevice),
              "hipMemcpy sparse candidates");
    record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(candidate_bytes));
    check_hip(hipMemsetAsync(device_counts.get(), 0, count_bytes, hipStreamPerThread), "hipMemset sparse counts");
    const auto events = make_hip_event_pair("create sparse collection events");
    launch_measured_kernel(hip_kernel_api().count_sparse_positions_kernel, grid, kSparseKernelThreads, 0,
                           hipStreamPerThread, events, "launch sparse count kernel", device_input,
                           device_candidates.get(), device_counts.get());
    finish_measured_kernel(telemetry, events, "synchronize sparse count kernel");

    std::vector<std::uint32_t> counts(candidates.size());
    check_hip(copy_on_codec_stream(counts.data(), device_counts.get(), count_bytes, hipMemcpyDeviceToHost),
              "hipMemcpy sparse counts");
    record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(count_bytes));
    // Admission uses exact completed counts and rejects overflow before forming positions.
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
    // Gather storage lives until readback completes; both reservations unwind on checked errors.
    HipDeviceMemoryReservation gather_reservation(
        checked_add_bytes(position_bytes, count_bytes, "sparse gather workspace"), "sparse gather workspace");
    HipDeviceBuffer<std::uint32_t> device_positions(position_bytes, "hipMalloc sparse positions");
    HipDeviceBuffer<std::uint32_t> device_cursors(count_bytes, "hipMalloc sparse cursors");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(position_bytes + count_bytes));
    check_hip(copy_on_codec_stream(device_candidates.get(), admitted.data(), candidate_bytes, hipMemcpyHostToDevice),
              "hipMemcpy admitted sparse candidates");
    record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(candidate_bytes));
    check_hip(hipMemsetAsync(device_cursors.get(), 0, count_bytes, hipStreamPerThread), "hipMemset sparse cursors");
    launch_measured_kernel(hip_kernel_api().gather_sparse_positions_kernel, grid, kSparseKernelThreads, 0,
                           hipStreamPerThread, events, "launch sparse gather kernel", device_input,
                           device_candidates.get(), device_cursors.get(), device_positions.get());
    finish_measured_kernel(telemetry, events, "synchronize sparse gather kernel");

    std::vector<std::uint32_t> cursors(candidates.size());
    check_hip(copy_on_codec_stream(cursors.data(), device_cursors.get(), count_bytes, hipMemcpyDeviceToHost),
              "hipMemcpy sparse cursors");
    record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(count_bytes));
    for (std::size_t index = 0U; index < admitted.size(); ++index) {
        if (admitted[index].patch_count != 0U && cursors[index] != admitted[index].patch_count) {
            throw GpuError("sparse pattern HIP gather count changed between passes");
        }
    }
    std::vector<std::uint32_t> positions(static_cast<std::size_t>(total_positions));
    check_hip(copy_on_codec_stream(positions.data(), device_positions.get(), position_bytes, hipMemcpyDeviceToHost),
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
