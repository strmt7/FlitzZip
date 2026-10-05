#pragma once

#include "core/compound_block.hpp"

namespace superzip {

inline constexpr std::size_t kGpuBytePlaneHeaderBytes = 4U;

// Purpose: Admit only the documented exact byte permutations, independent of file type or alignment.
// Inputs: A serialized number of byte planes.
// Outputs: True for widths two, four or eight; false for every other value.
inline constexpr bool is_gpu_byte_plane_width(std::uint8_t width) {
    return width == 2U || width == 4U || width == 8U;
}

struct GpuBytePlaneStage {
    std::uint8_t width;
    BlockDescriptor inner;
    std::span<const std::byte> payload;
};

// Purpose: Validate a nonrecursive byte-plane frame before allocation or device dispatch.
// Inputs: Exact encoded bytes and a bounded outer descriptor; inner codecs are the closed plain GPU set.
// Outputs: Returns an equally sized transformed-stage descriptor or throws for noncanonical framing.
inline GpuBytePlaneStage parse_gpu_byte_plane_block(std::span<const std::byte> encoded, const BlockDescriptor& outer) {
    if (outer.kind != BlockKind::GpuBytePlane || outer.fill_value != 0U || outer.uncompressed_len == 0U ||
        outer.uncompressed_len > kMaxArchiveBlockBytes || encoded.size() != outer.encoded_len ||
        encoded.size() < kGpuBytePlaneHeaderBytes || encoded.size() >= outer.uncompressed_len) {
        throw ArchiveError("GPU byte-plane framing is invalid");
    }
    const auto width = static_cast<std::uint8_t>(encoded[0]);
    const auto kind = static_cast<BlockKind>(encoded[1]);
    const auto fill = static_cast<std::uint8_t>(encoded[2]);
    if (!is_gpu_byte_plane_width(width) || outer.uncompressed_len < width ||
        (!is_gpu_compound_stage(kind) && kind != BlockKind::Fill) || encoded[3] != std::byte{0} ||
        (kind != BlockKind::Fill && fill != 0U) ||
        (kind == BlockKind::Fill && encoded.size() != kGpuBytePlaneHeaderBytes)) {
        throw ArchiveError("GPU byte-plane stage is unsupported or noncanonical");
    }
    return {.width = width,
            .inner = {.kind = kind,
                      .fill_value = fill,
                      .uncompressed_len = outer.uncompressed_len,
                      .encoded_len = static_cast<std::uint32_t>(encoded.size() - kGpuBytePlaneHeaderBytes)},
            .payload = encoded.subspan(kGpuBytePlaneHeaderBytes)};
}

}  // namespace superzip
