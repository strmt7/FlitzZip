#pragma once

#include "core/compound_block.hpp"

#include <array>

namespace superzip {

inline constexpr std::size_t kGpuBytePlaneHeaderBytes = 4U;
inline constexpr std::size_t kGpuBytePlaneContextRecordBytes = 6U;

// Purpose: Admit only the documented exact byte permutations, independent of file type or alignment.
// Inputs: A serialized number of byte planes.
// Outputs: True for widths two, four or eight; false for every other value.
inline constexpr bool is_gpu_byte_plane_width(std::uint8_t width) {
    return width == 2U || width == 4U || width == 8U;
}

// Purpose: Identify the versioned byte-plane family for shared validation and dispatch.
// Inputs: A possibly unknown native block kind.
// Outputs: True only for the whole-block and independent-context representations.
inline constexpr bool is_gpu_byte_plane_kind(BlockKind kind) {
    return kind == BlockKind::GpuBytePlane || kind == BlockKind::GpuBytePlaneContexts;
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

struct GpuBytePlaneContextStages {
    std::uint8_t width;
    std::uint8_t count;
    std::array<BlockDescriptor, 8> blocks{};
    std::span<const std::byte> payload;
};

// Purpose: Admit a closed, dense table of independently encoded byte planes before allocation or dispatch.
// Inputs: Exact frame bytes and a bounded outer descriptor; decoded plane lengths are derived from its extent.
// Outputs: Returns nonrecursive plain-stage descriptors or rejects noncanonical kinds, lengths and trailing data.
inline GpuBytePlaneContextStages parse_gpu_byte_plane_contexts(std::span<const std::byte> encoded,
                                                               const BlockDescriptor& outer) {
    if (outer.kind != BlockKind::GpuBytePlaneContexts || outer.fill_value != 0U || outer.uncompressed_len == 0U ||
        outer.uncompressed_len > kMaxArchiveBlockBytes || encoded.size() != outer.encoded_len ||
        encoded.size() < kGpuBytePlaneHeaderBytes || encoded.size() >= outer.uncompressed_len) {
        throw ArchiveError("GPU byte-plane context framing is invalid");
    }
    const auto width = static_cast<std::uint8_t>(encoded[0]);
    if (!is_gpu_byte_plane_width(width) || outer.uncompressed_len < width || encoded[1] != std::byte{0} ||
        encoded[2] != std::byte{0} || encoded[3] != std::byte{0}) {
        throw ArchiveError("GPU byte-plane context header is noncanonical");
    }
    const auto header_bytes = kGpuBytePlaneHeaderBytes + width * kGpuBytePlaneContextRecordBytes;
    if (encoded.size() < header_bytes) {
        throw ArchiveError("GPU byte-plane context table is truncated");
    }
    GpuBytePlaneContextStages stages{.width = width, .count = width, .payload = encoded.subspan(header_bytes)};
    std::size_t cursor = 0U;
    const auto records = outer.uncompressed_len / width;
    for (std::size_t plane = 0U; plane < width; ++plane) {
        const auto offset = kGpuBytePlaneHeaderBytes + plane * kGpuBytePlaneContextRecordBytes;
        const auto kind = static_cast<BlockKind>(encoded[offset]);
        const auto fill = static_cast<std::uint8_t>(encoded[offset + 1U]);
        std::uint32_t bytes = 0U;
        for (std::size_t byte = 0U; byte < sizeof(bytes); ++byte) {
            bytes |= static_cast<std::uint32_t>(encoded[offset + 2U + byte]) << (byte * 8U);
        }
        const auto decoded = records + (plane + 1U == width ? outer.uncompressed_len % width : 0U);
        if ((!is_gpu_compound_stage(kind) && kind != BlockKind::Raw && kind != BlockKind::Fill) ||
            (kind != BlockKind::Fill && fill != 0U) || (kind == BlockKind::Fill && bytes != 0U) ||
            (kind == BlockKind::Raw && bytes != decoded) ||
            (kind != BlockKind::Raw && kind != BlockKind::Fill && (bytes == 0U || bytes >= decoded)) ||
            cursor > stages.payload.size() || bytes > stages.payload.size() - cursor) {
            throw ArchiveError("GPU byte-plane context stage is unsupported or noncanonical");
        }
        stages.blocks[plane] = {.kind = kind,
                                .fill_value = fill,
                                .uncompressed_len = decoded,
                                .encoded_offset = cursor,
                                .encoded_len = bytes};
        cursor += bytes;
    }
    if (cursor != stages.payload.size()) {
        throw ArchiveError("GPU byte-plane context frame contains trailing bytes");
    }
    return stages;
}

// Purpose: Resolve both byte-plane versions into one nonrecursive stage table for their common readers.
// Inputs: Exact untrusted frame bytes and their version-admitted outer descriptor.
// Outputs: Returns the existing single-stage or independent-context layout; all framing checks remain mandatory.
inline GpuBytePlaneContextStages parse_gpu_byte_plane_stages(std::span<const std::byte> encoded,
                                                             const BlockDescriptor& outer) {
    if (outer.kind == BlockKind::GpuBytePlane) {
        const auto stage = parse_gpu_byte_plane_block(encoded, outer);
        return {.width = stage.width, .count = 1U, .blocks = {stage.inner}, .payload = stage.payload};
    }
    return parse_gpu_byte_plane_contexts(encoded, outer);
}

}  // namespace superzip
