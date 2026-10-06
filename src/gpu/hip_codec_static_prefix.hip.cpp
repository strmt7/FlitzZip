#include "gpu/hip_kernel_api.hpp"

namespace superzip::hip_detail {
namespace {

using namespace hip_detail;

// Purpose: Return the static prefix-code bit width for one byte.
// Inputs: `value` is an uncompressed byte.
// Outputs: Returns the number of bits emitted by the GPU prefix codec.
__device__ std::uint32_t gpu_prefix_width(std::uint8_t value) {
    if (value <= 3U) {
        return 3U;
    }
    if (value <= 19U) {
        return 6U;
    }
    if (value <= 83U) {
        return 9U;
    }
    return 11U;
}

// Purpose: Return the static prefix-code bits for one byte in little-bit order.
// Inputs: `value` is an uncompressed byte and `width` receives the bit count.
// Outputs: Returns the code bits packed from least-significant to most-significant bit.
__device__ std::uint32_t gpu_prefix_code(std::uint8_t value, std::uint32_t& width) {
    width = gpu_prefix_width(value);
    if (value <= 3U) {
        return static_cast<std::uint32_t>(value) << 1U;
    }
    if (value <= 19U) {
        return 0x1U | ((static_cast<std::uint32_t>(value) - 4U) << 2U);
    }
    if (value <= 83U) {
        return 0x3U | ((static_cast<std::uint32_t>(value) - 20U) << 3U);
    }
    return 0x7U | ((static_cast<std::uint32_t>(value) - 84U) << 3U);
}

// Purpose: Compute one worker thread's prefix-code bit count for a contiguous byte range.
// Inputs: `input`, `block_start`, `start`, and `end` describe readable device input bytes.
// Outputs: Returns the number of encoded bits for the worker range.
__device__ std::uint32_t gpu_prefix_range_bit_count(const std::byte* input, std::size_t block_start, std::size_t start,
                                                    std::size_t end) {
    std::uint32_t bits = 0;
    for (auto pos = start; pos < end; ++pos) {
        bits += gpu_prefix_width(static_cast<std::uint8_t>(input[block_start + pos]));
    }
    return bits;
}

// Purpose: Compute encoded byte counts for a batched list of prefix-code segments.
// Inputs: `input` is a device chunk, `plans` maps each launched block to a source segment, and `segment_lengths`
// receives one byte count per segment. Outputs: Writes aligned compressed byte counts for the static prefix codec.
__global__ void prefix_segment_lengths_batch_kernel(const std::byte* input, const PrefixEncodeSegmentPlan* plans,
                                                    std::uint32_t* segment_lengths, std::uint32_t segment_count) {
    const auto segment = static_cast<std::uint32_t>(blockIdx.x);
    if (segment >= segment_count) {
        return;
    }
    const auto& plan = plans[segment];
    __shared__ std::uint32_t sums[kGpuPrefixSegmentThreads];
    const auto thread_id = static_cast<std::uint32_t>(threadIdx.x);
    std::size_t start = 0;
    std::size_t end = 0;
    gpu_prefix_thread_range(plan.block_len, plan.segment_index, thread_id, start, end);
    sums[thread_id] = gpu_prefix_range_bit_count(input, plan.block_start, start, end);
    __syncthreads();
    for (std::uint32_t offset = kGpuPrefixSegmentThreads / 2U; offset > 0U; offset >>= 1U) {
        if (thread_id < offset) {
            sums[thread_id] += sums[thread_id + offset];
        }
        __syncthreads();
    }
    if (thread_id == 0U) {
        const auto bytes = (sums[0] + 7U) / 8U;
        segment_lengths[segment] = (bytes + 3U) & ~3U;
    }
}

// Purpose: Pack a batched list of byte segments with SuperZip's static GPU prefix code.
// Inputs: `input` is a device chunk, `plans` maps launched blocks to source segments, `segment_offsets` contains
// 4-byte-aligned output offsets, and `encoded` is zeroed output storage. Outputs: Writes prefix bitstreams.
__global__ void prefix_pack_segments_batch_kernel(const std::byte* input, const PrefixEncodeSegmentPlan* plans,
                                                  const std::uint32_t* segment_offsets, std::byte* encoded,
                                                  std::uint32_t segment_count) {
    const auto segment = static_cast<std::uint32_t>(blockIdx.x);
    if (segment >= segment_count) {
        return;
    }
    const auto& plan = plans[segment];
    __shared__ std::uint32_t sums[kGpuPrefixSegmentThreads];
    const auto thread_id = static_cast<std::uint32_t>(threadIdx.x);
    std::size_t start = 0;
    std::size_t end = 0;
    gpu_prefix_thread_range(plan.block_len, plan.segment_index, thread_id, start, end);
    const auto local_bits = gpu_prefix_range_bit_count(input, plan.block_start, start, end);
    const auto thread_bit_base = gpu_prefix_exclusive_scan(sums, local_bits);
    if (local_bits == 0U) {
        return;
    }
    unsigned int scratch[kGpuPrefixMaxThreadWords] = {};
    auto local_bit_pos = thread_bit_base % 32U;
    for (auto pos = start; pos < end; ++pos) {
        std::uint32_t width = 0;
        const auto code = gpu_prefix_code(static_cast<std::uint8_t>(input[plan.block_start + pos]), width);
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
void bind_static_prefix_kernels(HipKernelApi& api) noexcept {
    api.prefix_segment_lengths_batch_kernel = prefix_segment_lengths_batch_kernel;
    api.prefix_pack_segments_batch_kernel = prefix_pack_segments_batch_kernel;
}

}  // namespace superzip::hip_detail
