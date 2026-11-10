#pragma once

#include "core/archive_block_types.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <cstddef>
#include <span>

namespace flitzzip {

inline constexpr std::size_t kGpuCompoundHeaderBytes = 8U;

// Purpose: Define the closed payload-bearing GPU codec set allowed as a composition stage.
// Inputs: One native block kind, including possibly unknown serialized values.
// Outputs: True only for a nonrecursive GPU encoding with its own bounded decoder.
inline constexpr bool is_gpu_compound_stage(BlockKind kind) {
    return kind == BlockKind::Pattern || kind == BlockKind::GpuPrefix || kind == BlockKind::GpuAdaptivePrefix ||
           kind == BlockKind::GpuHuffman || kind == BlockKind::GpuDictionary || is_gpu_sparse_pattern_kind(kind);
}

struct GpuCompoundHeader {
    BlockDescriptor inner;
    BlockDescriptor original;
};

struct GpuCompoundStages {
    BlockDescriptor inner;
    BlockDescriptor original;
    std::span<const std::byte> inner_payload;
};

// Validate only fixed framing, using the descriptor's complete encoded extent.
// The prefix must contain eight header bytes; codec bodies are neither read nor admitted.
inline GpuCompoundHeader parse_gpu_compound_header(std::span<const std::byte> encoded, const BlockDescriptor& outer) {
    if (outer.kind != BlockKind::GpuCompound || outer.fill_value != 0U || outer.uncompressed_len == 0U ||
        outer.uncompressed_len > kMaxArchiveBlockBytes || outer.encoded_len < kGpuCompoundHeaderBytes ||
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
    if (intermediate_bytes >= outer.uncompressed_len || outer.encoded_len >= intermediate_bytes ||
        (inner_kind == BlockKind::Fill && outer.encoded_len != kGpuCompoundHeaderBytes)) {
        throw ArchiveError("GPU compound block does not strictly improve its original stage");
    }
    return {
        .inner = {.kind = inner_kind,
                  .fill_value = inner_fill,
                  .uncompressed_len = intermediate_bytes,
                  .encoded_len = static_cast<std::uint32_t>(outer.encoded_len - kGpuCompoundHeaderBytes)},
        .original = {.kind = original_kind,
                     .uncompressed_len = outer.uncompressed_len,
                     .encoded_len = intermediate_bytes},
    };
}

// Admit the complete payload extent as well as its nonrecursive fixed header.
// The returned payload borrows encoded; header admission alone does not validate codec bytes.
inline GpuCompoundStages parse_gpu_compound_block(std::span<const std::byte> encoded, const BlockDescriptor& outer) {
    if (encoded.size() != outer.encoded_len) {
        throw ArchiveError("GPU compound block framing is invalid");
    }
    const auto header = parse_gpu_compound_header(encoded, outer);
    return {
        .inner = header.inner, .original = header.original, .inner_payload = encoded.subspan(kGpuCompoundHeaderBytes)};
}

}  // namespace flitzzip
