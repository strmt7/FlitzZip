#include "gpu/hip_kernel_api.hpp"

namespace flitzzip::hip_detail {
namespace {

constexpr std::uint32_t kEncodeLower = 1U << 23U;
constexpr std::uint32_t kEncodeLimit = 1U << 31U;

__device__ bool rans_source_shape(std::uint32_t bytes, std::uint32_t shift) {
    return bytes != 0U && bytes <= kRansCountMaxBytes && shift >= 8U && shift <= 12U;
}

// A floor reciprocal underestimates the quotient by at most one for the
// admitted 31-bit state. Validate that bound before correcting; a corrupted
// reciprocal cannot silently supply another quotient. Frequency one uses its
// exact identity, avoiding a narrowed representation of 2^32.
__device__ bool rans_quotient(std::uint32_t state, std::uint32_t frequency, std::uint32_t reciprocal,
                              std::uint32_t& quotient, std::uint32_t& remainder) {
    if (frequency == 1U) {
        quotient = state;
        remainder = 0U;
        return true;
    }
    quotient = static_cast<std::uint32_t>((std::uint64_t{state} * reciprocal) >> 32U);
    const auto consumed = std::uint64_t{quotient} * frequency;
    if (consumed > state) {
        return false;
    }
    const auto rest = std::uint64_t{state} - consumed;
    if (rest >= 2ULL * frequency) {
        return false;
    }
    remainder = static_cast<std::uint32_t>(rest);
    if (remainder >= frequency) {
        ++quotient;
        remainder -= frequency;
    }
    return true;
}

// Each lane owns one bounded segment and its uniquely derived output slot.
// Reverse traversal writes renormalization bytes backwards, then prepends the
// final little-endian state. No cross-segment dependency or extra reversal.
__device__ RansEncodeResult encode_rans_segment(const std::byte* source, std::uint32_t bytes,
                                                const std::uint32_t* models, std::uint32_t model_words,
                                                const std::uint32_t* reciprocals, std::uint32_t precision,
                                                std::uint32_t bits, std::uint32_t shift, std::uint32_t segment,
                                                std::byte* output) {
    const auto begin = segment << shift;
    const auto length = min(1U << shift, bytes - begin);
    const auto slot_begin = 2U * begin + 4U * segment;
    const auto slot_end = slot_begin + 2U * length + 4U;
    auto cursor = slot_end;
    std::uint32_t state = kEncodeLower;
    const auto contexts = 1U << bits;
    const auto mass = 1U << precision;
    for (auto remaining = length; remaining != 0U; --remaining) {
        const auto index = begin + remaining - 1U;
        const auto previous = remaining == 1U ? 0U : static_cast<std::uint32_t>(source[index - 1U]);
        const auto symbol = static_cast<std::uint32_t>(source[index]);
        const auto context = previous >> (8U - bits);
        const auto row = models[context];
        if (row < contexts || row > model_words - 257U) {
            return {0U, 0U, RansModelStatus::invalid_model};
        }
        const auto* cdf = models + row;
        if (cdf[0] != 0U || cdf[256] != mass || cdf[symbol] >= cdf[symbol + 1U] || cdf[symbol + 1U] > mass) {
            return {0U, 0U, RansModelStatus::invalid_model};
        }
        const auto frequency = cdf[symbol + 1U] - cdf[symbol];
        const auto threshold = ((kEncodeLower >> precision) << 8U) * frequency;
        for (unsigned int emitted = 0U; emitted < 2U && state >= threshold; ++emitted) {
            if (cursor <= slot_begin + 4U) {
                return {0U, 0U, RansModelStatus::invalid_model};
            }
            output[--cursor] = static_cast<std::byte>(state & 255U);
            state >>= 8U;
        }
        std::uint32_t quotient = 0U;
        std::uint32_t remainder = 0U;
        if (state >= threshold ||
            !rans_quotient(state, frequency, reciprocals[context * 256U + symbol], quotient, remainder)) {
            return {0U, 0U, RansModelStatus::invalid_model};
        }
        const auto next = (std::uint64_t{quotient} << precision) + remainder + cdf[symbol];
        if (next < kEncodeLower || next >= kEncodeLimit) {
            return {0U, 0U, RansModelStatus::invalid_model};
        }
        state = static_cast<std::uint32_t>(next);
    }
    cursor -= 4U;
    for (unsigned int i = 0U; i < 4U; ++i) {
        output[cursor + i] = static_cast<std::byte>((state >> (8U * i)) & 255U);
    }
    return {cursor, slot_end - cursor, RansModelStatus::success};
}

__global__ void encode_rans_segments(const std::byte* source, std::uint32_t bytes, const std::uint32_t* models,
                                     std::uint32_t model_words, const std::uint32_t* reciprocals,
                                     std::uint32_t reciprocal_words, std::uint32_t precision, std::uint32_t bits,
                                     std::uint32_t shift, std::uint32_t first, std::uint32_t count, std::byte* output,
                                     std::uint32_t output_bytes, RansEncodeResult* results,
                                     std::uint32_t result_count) {
    const auto id = blockIdx.x * blockDim.x + threadIdx.x;
    if (blockDim.y != 1U || blockDim.z != 1U || gridDim.y != 1U || gridDim.z != 1U || id >= count ||
        results == nullptr || first >= result_count || id >= result_count - first) {
        return;
    }
    results[first + id] = {0U, 0U, RansModelStatus::invalid_plan};
    if (source == nullptr || models == nullptr || reciprocals == nullptr || output == nullptr ||
        !rans_source_shape(bytes, shift) || precision < 8U || precision > 16U || bits > 8U || count == 0U ||
        count > kRansEncodeBatchSegments) {
        return;
    }
    const auto segments = ((bytes - 1U) >> shift) + 1U;
    const auto contexts = 1U << bits;
    if (result_count != segments || count > segments - first || model_words != contexts * 258U ||
        reciprocal_words != contexts * 256U || output_bytes != 2U * bytes + 4U * segments) {
        return;
    }
    results[first + id] = encode_rans_segment(source, bytes, models, model_words, reciprocals, precision, bits, shift,
                                              first + id, output);
}

// Establish both source-slot containment and the exact adjacent destination
// interval before any pointer arithmetic. Checked ordering precedes subtraction.
__device__ bool rans_stream_range_is_valid(const RansEncodeResult& result, std::uint32_t slot_begin,
                                           std::uint32_t slot_end, std::uint32_t start, std::uint32_t end,
                                           std::uint32_t output_bytes) {
    return result.status == RansModelStatus::success && result.stream_bytes >= 4U &&
           result.stream_offset >= slot_begin && result.stream_offset <= slot_end &&
           result.stream_bytes == slot_end - result.stream_offset && start <= end && end <= output_bytes &&
           end - start == result.stream_bytes;
}

// Validate each fixed source slot and its adjacent dense offsets before copying.
// Admission is block-uniform; successful status follows the copy's final barrier.
// The owner admits all batches before retaining the complete compacted stream.
__global__ void compact_rans_segments(const std::byte* slots, std::uint32_t slot_bytes, std::uint32_t bytes,
                                      std::uint32_t shift, const RansEncodeResult* results, std::uint32_t result_count,
                                      const std::uint32_t* offsets, std::uint32_t offset_count, std::uint32_t first,
                                      std::uint32_t count, std::byte* output, std::uint32_t output_bytes,
                                      RansModelStatus* statuses, std::uint32_t status_count) {
    const auto id = blockIdx.x;
    if (blockDim.x != 256U || blockDim.y != 1U || blockDim.z != 1U || gridDim.y != 1U || gridDim.z != 1U ||
        statuses == nullptr || id >= status_count) {
        return;
    }
    if (threadIdx.x == 0U) {
        statuses[id] = RansModelStatus::invalid_plan;
    }
    if (slots == nullptr || results == nullptr || offsets == nullptr || output == nullptr ||
        !rans_source_shape(bytes, shift) || count == 0U || count > kRansEncodeBatchSegments || status_count != count) {
        return;
    }
    const auto segments = ((bytes - 1U) >> shift) + 1U;
    if (result_count != segments || offset_count != segments + 1U || first >= segments || count > segments - first ||
        slot_bytes != 2U * bytes + 4U * segments || output_bytes < 4U * segments || output_bytes > slot_bytes) {
        return;
    }
    const auto segment = first + id;
    const auto begin = segment << shift;
    const auto length = min(1U << shift, bytes - begin);
    const auto slot_begin = 2U * begin + 4U * segment;
    const auto slot_end = slot_begin + 2U * length + 4U;
    const auto result = results[segment];
    const auto start = offsets[segment];
    const auto end = offsets[segment + 1U];
    if (offsets[0] != 0U || offsets[segments] != output_bytes ||
        !rans_stream_range_is_valid(result, slot_begin, slot_end, start, end, output_bytes)) {
        if (threadIdx.x == 0U) {
            statuses[id] = RansModelStatus::invalid_model;
        }
        return;
    }
    for (auto i = threadIdx.x; i < result.stream_bytes; i += blockDim.x) {
        output[start + i] = slots[result.stream_offset + i];
    }
    __syncthreads();
    if (threadIdx.x == 0U) {
        statuses[id] = RansModelStatus::success;
    }
}

}  // namespace

void bind_rans_encode_kernels(HipKernelApi& api) noexcept {
    api.encode_rans_segments = encode_rans_segments;
    api.compact_rans_segments = compact_rans_segments;
}

}  // namespace flitzzip::hip_detail
