#include "gpu/hip_kernel_api.hpp"

namespace superzip::hip_detail {
namespace {

using namespace hip_detail;

// Purpose: Count encoded bits for one worker range using a per-block adaptive prefix table.
// Inputs: `input`, `table`, `block_start`, `start`, and `end` describe readable bytes and adaptive widths.
// Outputs: Returns the number of encoded bits for the worker range.
__device__ std::uint32_t gpu_adaptive_prefix_range_bit_count(const std::byte* input, const AdaptiveEncodeTable* table,
                                                             std::size_t block_start, std::size_t start,
                                                             std::size_t end) {
    std::uint32_t bits = 0;
    for (auto pos = start; pos < end; ++pos) {
        bits += table->width[static_cast<std::uint8_t>(input[block_start + pos])];
    }
    return bits;
}

// Purpose: Measure every admitted entropy table while loading each source segment only once.
// Inputs: Bounded plans, contiguous candidate tables, output counts, and Capacity (2/4/8/16) bound shared scratch.
// Outputs: Writes exact, word-aligned segment lengths for every candidate without packing any payload.
template <std::uint32_t Capacity>
__global__ void entropy_segment_lengths_batch_kernel(const std::byte* input, const EntropyLengthSegmentPlan* plans,
                                                     const AdaptiveEncodeTable* tables, std::uint32_t* segment_lengths,
                                                     std::uint32_t segment_count) {
    const auto segment = static_cast<std::uint32_t>(blockIdx.x);
    if (segment >= segment_count) {
        return;
    }
    const auto& plan = plans[segment];
    __shared__ std::uint8_t widths[Capacity][256];
    __shared__ std::uint32_t sums[Capacity][kGpuPrefixSegmentThreads];
    const auto thread_id = static_cast<std::uint32_t>(threadIdx.x);
#pragma unroll
    for (std::uint32_t candidate = 0U; candidate < Capacity; ++candidate) {
        if (candidate < plan.candidate_count) {
            for (auto symbol = thread_id; symbol < 256U; symbol += kGpuPrefixSegmentThreads) {
                widths[candidate][symbol] = tables[plan.table_index + candidate].width[symbol];
            }
        }
    }
    __syncthreads();
    std::uint32_t local_bits[Capacity] = {};
    const auto start = static_cast<std::size_t>(plan.segment_index) * kGpuPrefixSegmentBytes;
    const auto end = min(static_cast<std::size_t>(plan.block_len), start + kGpuPrefixSegmentBytes);
    for (auto pos = start + thread_id; pos < end; pos += kGpuPrefixSegmentThreads) {
        const auto value = static_cast<std::uint8_t>(input[static_cast<std::size_t>(plan.block_start) + pos]);
#pragma unroll
        for (std::uint32_t candidate = 0U; candidate < Capacity; ++candidate) {
            if (candidate < plan.candidate_count) {
                local_bits[candidate] += widths[candidate][value];
            }
        }
    }
#pragma unroll
    for (std::uint32_t candidate = 0U; candidate < Capacity; ++candidate) {
        if (candidate < plan.candidate_count) {
            sums[candidate][thread_id] = local_bits[candidate];
        }
    }
    __syncthreads();
    for (std::uint32_t offset = kGpuPrefixSegmentThreads / 2U; offset > 0U; offset >>= 1U) {
        if (thread_id < offset) {
#pragma unroll
            for (std::uint32_t candidate = 0U; candidate < Capacity; ++candidate) {
                if (candidate < plan.candidate_count) {
                    sums[candidate][thread_id] += sums[candidate][thread_id + offset];
                }
            }
        }
        __syncthreads();
    }
    if (thread_id == 0U) {
        for (std::uint32_t candidate = 0U; candidate < plan.candidate_count; ++candidate) {
            const auto bytes = (sums[candidate][0] + 7U) / 8U;
            segment_lengths[plan.length_offset + candidate] = (bytes + 3U) & ~3U;
        }
    }
}

// Purpose: Pack a batched list of byte segments with adaptive GPU prefix code tables.
// Inputs: `input`, `plans`, `tables`, `segment_offsets`, and `encoded` describe the selected output spans.
// Outputs: Writes adaptive prefix bitstreams with atomic word updates.
__global__ void adaptive_prefix_pack_segments_batch_kernel(const std::byte* input,
                                                           const AdaptiveEncodeSegmentPlan* plans,
                                                           const AdaptiveEncodeTable* tables,
                                                           const std::uint32_t* segment_offsets, std::byte* encoded,
                                                           std::uint32_t segment_count) {
    const auto segment = static_cast<std::uint32_t>(blockIdx.x);
    if (segment >= segment_count) {
        return;
    }
    const auto& plan = plans[segment];
    const auto* table = tables + plan.table_index;
    __shared__ std::uint32_t sums[kGpuPrefixSegmentThreads];
    const auto thread_id = static_cast<std::uint32_t>(threadIdx.x);
    std::size_t start = 0;
    std::size_t end = 0;
    gpu_prefix_thread_range(plan.block_len, plan.segment_index, thread_id, start, end);
    const auto local_bits =
        gpu_adaptive_prefix_range_bit_count(input, table, static_cast<std::size_t>(plan.block_start), start, end);
    const auto thread_bit_base = gpu_prefix_exclusive_scan(sums, local_bits);
    if (local_bits == 0U) {
        return;
    }
    unsigned int scratch[kGpuPrefixMaxThreadWords] = {};
    auto local_bit_pos = thread_bit_base % 32U;
    for (auto pos = start; pos < end; ++pos) {
        const auto value = static_cast<std::uint8_t>(input[static_cast<std::size_t>(plan.block_start) + pos]);
        const auto width = static_cast<std::uint32_t>(table->width[value]);
        const auto code = static_cast<std::uint32_t>(table->code[value]);
        for (std::uint32_t bit = 0; bit < width; ++bit) {
            if (((code >> bit) & 1U) != 0U) {
                scratch[local_bit_pos / 32U] |= 1U << (local_bit_pos % 32U);
            }
            ++local_bit_pos;
        }
    }
    auto* segment_words = reinterpret_cast<unsigned int*>(encoded + segment_offsets[segment]);
    const auto word_base = thread_bit_base / 32U;
    const auto word_count = ((thread_bit_base % 32U) + local_bits + 31U) / 32U;
    for (std::uint32_t word = 0; word < word_count; ++word) {
        if (scratch[word] != 0U) {
            atomicOr(segment_words + word_base + word, scratch[word]);
        }
    }
}

}  // namespace

// Purpose: Bind this translation unit's registered kernels to the private POD dispatch table.
// Inputs: Module-owned table under construction after DLL registration.
// Outputs: Writes only this component's typed entrypoints; allocates no storage.
void bind_adaptive_prefix_kernels(HipKernelApi& api) noexcept {
    api.entropy_lengths_2 = entropy_segment_lengths_batch_kernel<2U>;
    api.entropy_lengths_4 = entropy_segment_lengths_batch_kernel<4U>;
    api.entropy_lengths_8 = entropy_segment_lengths_batch_kernel<8U>;
    api.entropy_lengths_16 = entropy_segment_lengths_batch_kernel<16U>;
    api.adaptive_prefix_pack_segments_batch_kernel = adaptive_prefix_pack_segments_batch_kernel;
}

}  // namespace superzip::hip_detail
