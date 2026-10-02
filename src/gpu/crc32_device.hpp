#pragma once

#include "core/crc32_operators.hpp"
#include "gpu/hip_codec_support.hpp"

namespace superzip::hip_detail {

constexpr unsigned int kCrcSegmentThreads = 256U;
constexpr std::size_t kDeviceCrcLengthBits = 16U;
static_assert(kLargeCrcSegmentBytes < (1U << kDeviceCrcLengthBits));

__device__ __constant__ crc_detail::ByteOperators<kDeviceCrcLengthBits> kDeviceCrcByteOperators =
    crc_detail::make_byte_operators<kDeviceCrcLengthBits>();

// Purpose: Concatenate finalized CRCs in source order within one bounded device segment.
// Inputs: first/second are independent CRCs; second_length is at most kLargeCrcSegmentBytes.
// Outputs: Returns the CRC of the concatenation; an empty second range preserves first.
__device__ __forceinline__ std::uint32_t combine_device_crc32(std::uint32_t first, std::uint32_t second,
                                                              std::uint32_t second_length) {
    if (second_length == 0U) {
        return first;
    }
    for (std::size_t power = 0U; second_length != 0U; ++power) {
        if ((second_length & 1U) != 0U) {
            auto vector = first;
            std::uint32_t result = 0U;
            for (std::size_t bit = 0U; vector != 0U; ++bit) {
                if ((vector & 1U) != 0U) {
                    result ^= kDeviceCrcByteOperators.powers[power].rows[bit];
                }
                vector >>= 1U;
            }
            first = result;
        }
        second_length >>= 1U;
    }
    return first ^ second;
}

// Purpose: Reduce adjacent partial CRCs without changing byte order or host result geometry.
// Inputs: All kCrcSegmentThreads threads call with their contiguous CRC/length, including empty tails.
// Outputs: Thread zero writes one exact segment CRC and length; uses 2 KiB shared storage and uniform barriers.
__device__ __forceinline__ void publish_cooperative_crc32(std::uint32_t crc, std::uint32_t length,
                                                          DeviceCrcSegment* segments, std::uint32_t segment_index) {
    __shared__ std::uint32_t checksums[kCrcSegmentThreads];
    __shared__ std::uint32_t lengths[kCrcSegmentThreads];
    const auto thread = static_cast<unsigned int>(threadIdx.x);
    checksums[thread] = crc;
    lengths[thread] = length;
    __syncthreads();
    for (unsigned int stride = 1U; stride < kCrcSegmentThreads; stride *= 2U) {
        const auto first = thread * (stride * 2U);
        if (first < kCrcSegmentThreads) {
            const auto second = first + stride;
            checksums[first] = combine_device_crc32(checksums[first], checksums[second], lengths[second]);
            lengths[first] += lengths[second];
        }
        __syncthreads();
    }
    if (thread == 0U) {
        segments[segment_index] = DeviceCrcSegment{.crc32 = checksums[0], .length = lengths[0]};
    }
}

}  // namespace superzip::hip_detail
