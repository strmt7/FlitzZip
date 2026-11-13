#include "gpu/hip_kernel_api.hpp"

namespace flitzzip::hip_detail {
namespace {

// Block-uniform admission precedes every barrier. Each block owns one source
// tile/context bucket and exactly one status; inactive blocks touch nothing.
__global__ void count_rans_transitions(const std::byte* input, std::uint32_t bytes, std::uint32_t first_tile,
                                       std::uint32_t tile_count, std::uint32_t* counts, std::uint32_t words,
                                       RansCountStatus* statuses, std::uint32_t status_count) {
    const auto id = blockIdx.x;
    if (blockDim.y != 1U || blockDim.z != 1U || gridDim.y != 1U || gridDim.z != 1U || statuses == nullptr ||
        id >= status_count) {
        return;
    }
    if (threadIdx.x == 0U) {
        statuses[id] = RansCountStatus::invalid_plan;
    }
    if (input == nullptr || counts == nullptr || bytes == 0U || bytes > kRansCountMaxBytes ||
        words != kRansCountWords || tile_count == 0U || tile_count > kRansCountTilesPerDispatch ||
        status_count != tile_count * kRansCountBucketCount) {
        return;
    }
    const auto tiles = (bytes - 1U) / kRansCountTileBytes + 1U;
    if (first_tile >= tiles || tile_count > tiles - first_tile) {
        return;
    }
    __shared__ std::uint32_t bins[4096];
    for (auto i = threadIdx.x; i < 4096U; i += blockDim.x) {
        bins[i] = 0U;
    }
    __syncthreads();
    const auto bucket = id % kRansCountBucketCount;
    const auto begin = (first_tile + id / kRansCountBucketCount) * kRansCountTileBytes;
    const auto tile_bytes = min(bytes - begin, kRansCountTileBytes);
    for (auto local = threadIdx.x; local < tile_bytes; local += blockDim.x) {
        const auto i = begin + local;
        const auto previous = i == 0U ? 0U : static_cast<std::uint32_t>(input[i - 1U]);
        if ((previous >> 4U) == bucket) {
            const auto symbol = static_cast<std::uint32_t>(input[i]);
            atomicAdd(&bins[((previous & 15U) << 8U) | symbol], 1U);
        }
    }
    __syncthreads();
    for (auto i = threadIdx.x; i < 4096U; i += blockDim.x) {
        if (bins[i] != 0U) {
            atomicAdd(&counts[(bucket << 12U) | i], bins[i]);
        }
    }
    __syncthreads();
    if (threadIdx.x == 0U) {
        statuses[id] = RansCountStatus::success;
    }
}

// Record the actual predecessor of each later segment start. The first source
// byte already uses predecessor zero and is never corrected a second time.
__global__ void count_rans_boundaries(const std::byte* input, std::uint32_t bytes, std::uint32_t shift,
                                      std::uint32_t first, std::uint32_t count, std::uint32_t* removed,
                                      std::uint32_t words, RansCountStatus* statuses, std::uint32_t status_count) {
    const auto id = blockIdx.x * blockDim.x + threadIdx.x;
    if (blockDim.y != 1U || blockDim.z != 1U || gridDim.y != 1U || gridDim.z != 1U || statuses == nullptr ||
        id >= status_count) {
        return;
    }
    statuses[id] = RansCountStatus::invalid_plan;
    if (input == nullptr || removed == nullptr || bytes == 0U || bytes > kRansCountMaxBytes || shift < 8U ||
        shift > 12U || count == 0U || count > kRansCorrectionBatch || words != kRansCountWords ||
        status_count != count || first == 0U) {
        return;
    }
    const auto last = (bytes - 1U) >> shift;
    if (first > last || count > last - first + 1U) {
        return;
    }
    const auto offset = (first + id) << shift;
    const auto previous = static_cast<std::uint32_t>(input[offset - 1U]);
    const auto symbol = static_cast<std::uint32_t>(input[offset]);
    atomicAdd(&removed[(previous << 8U) | symbol], 1U);
    statuses[id] = RansCountStatus::success;
}

// Context zero keeps transitions already within its group and gains every
// boundary sample outside it. Other groups lose only their own boundary samples.
// Validate every subtraction and bounded sum before publishing a single count.
__device__ bool project_rans_count(const std::uint32_t* base, const std::uint32_t* removed, std::uint32_t bytes,
                                   std::uint32_t bits, std::uint32_t entry, std::uint32_t& value) {
    const auto context = entry >> 8U;
    const auto symbol = entry & 255U;
    const auto width = 1U << (8U - bits);
    const auto begin = context * width;
    const auto end = context == 0U ? 256U : begin + width;
    value = 0U;
    for (auto previous = begin; previous < end; ++previous) {
        const auto index = (previous << 8U) | symbol;
        const auto original = base[index];
        const auto correction = removed[index];
        if (original > bytes || correction > original) {
            return false;
        }
        const auto next = context == 0U ? (previous < width ? original : correction) : original - correction;
        if (next > bytes - value) {
            return false;
        }
        value += next;
    }
    return true;
}

// Independent output lanes need no barrier, polling or global dependency.
// Host batches are bounded even for the full 256-context distribution.
__global__ void project_rans_counts(const std::uint32_t* base, std::uint32_t base_words, const std::uint32_t* removed,
                                    std::uint32_t removed_words, std::uint32_t bytes, std::uint32_t bits,
                                    std::uint32_t first, std::uint32_t count, std::uint32_t* output,
                                    std::uint32_t output_words, RansCountStatus* statuses, std::uint32_t status_count) {
    const auto id = blockIdx.x * blockDim.x + threadIdx.x;
    if (blockDim.y != 1U || blockDim.z != 1U || gridDim.y != 1U || gridDim.z != 1U || statuses == nullptr ||
        id >= status_count) {
        return;
    }
    statuses[id] = RansCountStatus::invalid_plan;
    if (base == nullptr || removed == nullptr || output == nullptr || bytes == 0U || bytes > kRansCountMaxBytes ||
        bits > 8U || base_words != kRansCountWords || removed_words != kRansCountWords || count == 0U ||
        count > kRansProjectionBatch || status_count != count) {
        return;
    }
    const auto entries = 256U << bits;
    if (output_words != entries || first > entries || count > entries - first) {
        return;
    }
    std::uint32_t value = 0U;
    if (!project_rans_count(base, removed, bytes, bits, first + id, value)) {
        statuses[id] = RansCountStatus::invalid_counts;
        return;
    }
    output[first + id] = value;
    statuses[id] = RansCountStatus::success;
}

}  // namespace

void bind_rans_count_kernels(HipKernelApi& api) noexcept {
    api.count_rans_transitions = count_rans_transitions;
    api.count_rans_boundaries = count_rans_boundaries;
    api.project_rans_counts = project_rans_counts;
}

}  // namespace flitzzip::hip_detail
