#pragma once

#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace superzip {

constexpr std::size_t kSparsePatternHeaderBytes = 2U * sizeof(std::uint32_t);
constexpr std::size_t kSparsePatternPatchBytes = sizeof(std::uint32_t) + sizeof(std::byte);

// Borrowed views of one validated sparse payload; the encoded bytes remain caller-owned.
struct SparsePatternLayout {
    std::span<const std::byte> motif;
    std::span<const std::byte> patches;
    std::uint32_t patch_count = 0;
};

// Purpose: Read one bounded little-endian field from a version-five sparse block.
// Inputs: `bytes` is an untrusted payload and `offset` is the field's byte position.
// Outputs: Returns the field or throws `ArchiveError` before reading a truncated span.
inline std::uint32_t read_sparse_u32(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
        throw ArchiveError("sparse pattern block field is truncated");
    }
    std::uint32_t value = 0U;
    for (std::size_t byte = 0U; byte < sizeof(std::uint32_t); ++byte) {
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + byte])) << (byte * 8U);
    }
    return value;
}

// Purpose: Admit a canonical sparse repeated-pattern block before CPU or HIP materialization.
// Inputs: `payload` is untrusted encoded data that must outlive the returned spans; `decoded_size` and
// `max_period` are exact versioned bounds.
// Outputs: Returns bounded motif/patch spans or throws `ArchiveError` on size, order, range, or canonicality failure.
inline SparsePatternLayout parse_sparse_pattern_block(std::span<const std::byte> payload, std::uint32_t decoded_size,
                                                      std::uint32_t max_period = kMaxGpuPatternBytes) {
    if ((max_period != kMaxGpuPatternBytes && max_period != kMaxGpuLongSparsePatternBytes) || decoded_size == 0U ||
        decoded_size > kMaxArchiveBlockBytes || payload.size() < kSparsePatternHeaderBytes ||
        payload.size() >= decoded_size) {
        throw ArchiveError("sparse pattern block size is invalid");
    }
    const auto period = read_sparse_u32(payload, 0U);
    const auto patch_count = read_sparse_u32(payload, sizeof(std::uint32_t));
    const auto min_period = max_period == kMaxGpuLongSparsePatternBytes ? kMaxGpuPatternBytes + 1U : 2U;
    if (period < min_period || period > max_period || period >= decoded_size || patch_count == 0U ||
        period > payload.size() - kSparsePatternHeaderBytes) {
        throw ArchiveError("sparse pattern block header is invalid");
    }
    const auto patch_bytes = payload.size() - kSparsePatternHeaderBytes - period;
    if (patch_bytes % kSparsePatternPatchBytes != 0U || patch_count != patch_bytes / kSparsePatternPatchBytes ||
        patch_count > decoded_size - period) {
        throw ArchiveError("sparse pattern patch table size is invalid");
    }
    const auto motif = payload.subspan(kSparsePatternHeaderBytes, period);
    const auto patches = payload.subspan(kSparsePatternHeaderBytes + period);
    std::uint32_t previous = period - 1U;
    for (std::size_t index = 0U; index < patch_count; ++index) {
        const auto offset = index * kSparsePatternPatchBytes;
        const auto position = read_sparse_u32(patches, offset);
        if (position <= previous || position >= decoded_size ||
            patches[offset + sizeof(std::uint32_t)] == motif[position % period]) {
            throw ArchiveError("sparse pattern patch position or value is invalid");
        }
        previous = position;
    }
    return SparsePatternLayout{.motif = motif, .patches = patches, .patch_count = patch_count};
}

}  // namespace superzip
