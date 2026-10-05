#pragma once

#include "core/archive_block_types.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <cstddef>
#include <span>

namespace superzip {

inline constexpr std::size_t kGpuCompoundHeaderBytes = 8U;

// Purpose: Define the closed payload-bearing GPU codec set allowed as a composition stage.
// Inputs: One native block kind, including possibly unknown serialized values.
// Outputs: True only for a nonrecursive GPU encoding with its own bounded decoder.
inline constexpr bool is_gpu_compound_stage(BlockKind kind) {
    return kind == BlockKind::Pattern || kind == BlockKind::GpuPrefix || kind == BlockKind::GpuAdaptivePrefix ||
           kind == BlockKind::GpuHuffman || kind == BlockKind::GpuDictionary || is_gpu_sparse_pattern_kind(kind);
}

struct GpuCompoundStages {
    BlockDescriptor inner;
    BlockDescriptor original;
    std::span<const std::byte> inner_payload;
};

// Purpose: Validate the closed two-stage layout before allocation or device dispatch.
// Inputs: The exact compound block payload and its outer descriptor, with bounded decoded length.
// Outputs: Returns nonrecursive stage descriptors or throws for malformed or nonimproving framing.
inline GpuCompoundStages parse_gpu_compound_block(std::span<const std::byte> encoded, const BlockDescriptor& outer) {
    if (outer.kind != BlockKind::GpuCompound || outer.fill_value != 0U || outer.uncompressed_len == 0U ||
        outer.uncompressed_len > kMaxArchiveBlockBytes || encoded.size() != outer.encoded_len ||
        encoded.size() < kGpuCompoundHeaderBytes) {
        throw ArchiveError("GPU compound block framing is invalid");
    }
    const auto inner_kind = static_cast<BlockKind>(encoded[0]);
    const auto original_kind = static_cast<BlockKind>(encoded[1]);
    const auto inner_fill = static_cast<std::uint8_t>(encoded[2]);
    if ((!is_gpu_compound_stage(inner_kind) && inner_kind != BlockKind::Fill) ||
        !is_gpu_compound_stage(original_kind) || encoded[3] != std::byte{0} ||
        (inner_kind != BlockKind::Fill && inner_fill != 0U)) {
        throw ArchiveError("GPU compound block has an unsupported or noncanonical stage");
    }
    std::uint32_t intermediate_bytes = 0U;
    for (std::size_t byte = 0U; byte < sizeof(intermediate_bytes); ++byte) {
        intermediate_bytes |= static_cast<std::uint32_t>(encoded[4U + byte]) << (byte * 8U);
    }
    if (intermediate_bytes >= outer.uncompressed_len || encoded.size() >= intermediate_bytes ||
        (inner_kind == BlockKind::Fill && encoded.size() != kGpuCompoundHeaderBytes)) {
        throw ArchiveError("GPU compound block does not strictly improve its original stage");
    }
    return {
        .inner = {.kind = inner_kind,
                  .fill_value = inner_fill,
                  .uncompressed_len = intermediate_bytes,
                  .encoded_len = static_cast<std::uint32_t>(encoded.size() - kGpuCompoundHeaderBytes)},
        .original = {.kind = original_kind,
                     .uncompressed_len = outer.uncompressed_len,
                     .encoded_len = intermediate_bytes},
        .inner_payload = encoded.subspan(kGpuCompoundHeaderBytes),
    };
}

}  // namespace superzip
