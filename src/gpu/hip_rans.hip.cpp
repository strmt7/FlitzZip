#include "gpu/hip_kernel_api.hpp"

namespace flitzzip::hip_detail {
namespace {

constexpr std::uint32_t kLower = 1U << 23U;
constexpr std::uint32_t kStateLimit = 1U << 31U;
constexpr std::uint32_t kCdfWords = 257U;

// Bound metadata before shifts, arithmetic or pointer formation. Containment
// and disjoint allocations/plans remain the owning host dispatcher's contract.
__device__ bool valid_rans_plan(const RansDecodeSegment& plan, std::uint32_t input_bytes, std::uint32_t model_words,
                                std::uint32_t output_bytes, std::uint32_t precision, std::uint32_t context_bits) {
    return precision >= 8U && precision <= 16U && context_bits <= 8U &&
           model_words >= (1U << context_bits) + kCdfWords && model_words <= 66048U && plan.decoded_bytes != 0U &&
           plan.decoded_bytes <= 4096U && plan.stream_offset <= input_bytes &&
           plan.stream_bytes <= input_bytes - plan.stream_offset && plan.stream_bytes >= 4U &&
           plan.stream_bytes <= 2U * plan.decoded_bytes + 4U && plan.output_offset <= output_bytes &&
           plan.decoded_bytes <= output_bytes - plan.output_offset;
}

// Binary search has at most nine iterations, independent of symbol frequency.
// Validate the selected positive interval before the full-width state update.
__device__ bool rans_symbol(const std::uint32_t* cdf, std::uint32_t slot, std::uint32_t mass, std::uint32_t& symbol) {
    if (cdf[0] != 0U || cdf[256] != mass) {
        return false;
    }
    std::uint32_t lo = 0U;
    std::uint32_t hi = kCdfWords;
    for (unsigned int step = 0U; step < 9U && lo < hi; ++step) {
        const auto mid = lo + (hi - lo) / 2U;
        if (cdf[mid] <= slot) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    if (lo != hi || lo == 0U || lo > 256U) {
        return false;
    }
    symbol = lo - 1U;
    return cdf[symbol] <= slot && slot < cdf[symbol + 1U] && cdf[symbol + 1U] <= mass;
}

// Each lane owns one independent admitted segment (at most 4096 symbols).
// No barrier, polling or cross-lane dependency exists. At most two bounded
// byte reads restore each state. Partial output is never a publishable result.
__device__ RansDecodeStatus decode_rans_segment(const std::byte* input, const RansDecodeSegment& plan,
                                                const std::uint32_t* models, std::uint32_t model_words,
                                                std::uint32_t precision, std::uint32_t context_bits,
                                                std::byte* output) {
    const auto* stream = input + plan.stream_offset;
    std::uint32_t state = 0U;
    for (unsigned int i = 0U; i < 4U; ++i) {
        state |= static_cast<std::uint32_t>(stream[i]) << (8U * i);
    }
    if (state < kLower || state >= kStateLimit) {
        return RansDecodeStatus::invalid_state;
    }
    std::uint32_t cursor = 4U;
    std::uint32_t previous = 0U;
    const auto mass = 1U << precision;
    for (std::uint32_t i = 0U; i < plan.decoded_bytes; ++i) {
        const auto row = models[previous >> (8U - context_bits)];
        if (row < (1U << context_bits) || row > model_words - kCdfWords) {
            return RansDecodeStatus::missing_model;
        }
        const auto* cdf = models + row;
        const auto slot = state & (mass - 1U);
        std::uint32_t symbol = 0U;
        if (!rans_symbol(cdf, slot, mass, symbol)) {
            return RansDecodeStatus::missing_model;
        }
        const auto frequency = cdf[symbol + 1U] - cdf[symbol];
        state = frequency * (state >> precision) + slot - cdf[symbol];
        for (unsigned int n = 0U; n < 2U && state < kLower; ++n) {
            if (cursor == plan.stream_bytes) {
                return RansDecodeStatus::truncated_stream;
            }
            state = (state << 8U) | static_cast<std::uint32_t>(stream[cursor++]);
        }
        if (state < kLower || state >= kStateLimit) {
            return RansDecodeStatus::invalid_state;
        }
        output[plan.output_offset + i] = static_cast<std::byte>(symbol);
        previous = symbol;
    }
    return cursor == plan.stream_bytes && state == kLower ? RansDecodeStatus::success
                                                          : RansDecodeStatus::invalid_final_state;
}

// Counts describe live allocations, not trusted segment metadata. The host
// bounds each dispatch and keeps all allocations immutable through completion.
// Inactive lanes touch nothing, and every active lane initializes its status.
__global__ void decode_rans_segments(const std::byte* input, std::uint32_t input_bytes, const std::uint32_t* models,
                                     std::uint32_t model_words, const RansDecodeSegment* segments, std::uint32_t count,
                                     std::uint32_t precision, std::uint32_t context_bits, std::byte* output,
                                     std::uint32_t output_bytes, RansDecodeStatus* statuses) {
    const auto id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= count) {
        return;
    }
    statuses[id] = RansDecodeStatus::invalid_plan;
    const auto plan = segments[id];
    if (input == nullptr || models == nullptr || output == nullptr ||
        !valid_rans_plan(plan, input_bytes, model_words, output_bytes, precision, context_bits)) {
        return;
    }
    statuses[id] = decode_rans_segment(input, plan, models, model_words, precision, context_bits, output);
}

}  // namespace

void bind_rans_kernels(HipKernelApi& api) noexcept {
    api.decode_rans_segments = decode_rans_segments;
}

}  // namespace flitzzip::hip_detail
