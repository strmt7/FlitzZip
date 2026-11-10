#pragma once

#include "core/compound_block.hpp"

#include <array>

namespace flitzzip {

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

struct GpuBytePlaneHeader {
    std::uint8_t width;
    BlockDescriptor inner;
};

// Inspect the four-byte header against the complete declared extent without reading codec bytes.
inline GpuBytePlaneHeader parse_gpu_byte_plane_header(std::span<const std::byte> encoded,
                                                      const BlockDescriptor& outer) {
    if (outer.kind != BlockKind::GpuBytePlane || outer.fill_value != 0U || outer.uncompressed_len == 0U ||
        outer.uncompressed_len > kMaxArchiveBlockBytes || outer.encoded_len < kGpuBytePlaneHeaderBytes ||
        encoded.size() < kGpuBytePlaneHeaderBytes || outer.encoded_len >= outer.uncompressed_len) {
        throw ArchiveError("GPU byte-plane framing is invalid");
    }
    const auto width = static_cast<std::uint8_t>(encoded[0]);
    const auto kind = static_cast<BlockKind>(encoded[1]);
    const auto fill = static_cast<std::uint8_t>(encoded[2]);
    if (!is_gpu_byte_plane_width(width) || outer.uncompressed_len < width ||
        (!is_gpu_compound_stage(kind) && kind != BlockKind::Fill) || encoded[3] != std::byte{0} ||
        (kind != BlockKind::Fill && fill != 0U) ||
        (kind == BlockKind::Fill && outer.encoded_len != kGpuBytePlaneHeaderBytes)) {
        throw ArchiveError("GPU byte-plane stage is unsupported or noncanonical");
    }
    return {.width = width,
            .inner = {.kind = kind,
                      .fill_value = fill,
                      .uncompressed_len = outer.uncompressed_len,
                      .encoded_len = static_cast<std::uint32_t>(outer.encoded_len - kGpuBytePlaneHeaderBytes)}};
}

// Admit the complete frame extent; returned payload borrows encoded and still needs codec validation.
inline GpuBytePlaneStage parse_gpu_byte_plane_block(std::span<const std::byte> encoded, const BlockDescriptor& outer) {
    if (encoded.size() != outer.encoded_len) {
        throw ArchiveError("GPU byte-plane framing is invalid");
    }
    const auto header = parse_gpu_byte_plane_header(encoded, outer);
    return {.width = header.width, .inner = header.inner, .payload = encoded.subspan(kGpuBytePlaneHeaderBytes)};
}

struct GpuBytePlaneContextStages {
    std::uint8_t width;
    std::uint8_t count;
    std::array<BlockDescriptor, 8> blocks{};
    std::span<const std::byte> payload;
};

struct GpuBytePlaneContextHeader {
    std::uint8_t width;
    std::uint8_t count;
    std::array<BlockDescriptor, 8> blocks{};
    std::size_t header_bytes;
};

// Purpose: Classify one nonempty plane's canonical codec metadata independently from payload containment.
// Inputs: An untrusted child kind/fill/encoded extent and its nonzero derived decoded length.
// Outputs: True only for exact raw/fill extents or a strictly smaller nonrecursive plain GPU encoding.
inline bool is_canonical_gpu_byte_plane_context_stage(const BlockDescriptor& stage) {
    if (stage.uncompressed_len == 0U) {
        return false;
    }
    if (stage.kind == BlockKind::Fill) {
        return stage.encoded_len == 0U;
    }
    if (stage.fill_value != 0U) {
        return false;
    }
    if (stage.kind == BlockKind::Raw) {
        return stage.encoded_len == stage.uncompressed_len;
    }
    return is_gpu_compound_stage(stage.kind) && stage.encoded_len != 0U && stage.encoded_len < stage.uncompressed_len;
}

// Admit the complete fixed table and declared payload geometry, without reading codec bodies.
// At most 52 prefix bytes are needed; all child descriptors are owned values.
inline GpuBytePlaneContextHeader parse_gpu_byte_plane_context_header(std::span<const std::byte> encoded,
                                                                     const BlockDescriptor& outer) {
    if (outer.kind != BlockKind::GpuBytePlaneContexts || outer.fill_value != 0U || outer.uncompressed_len == 0U ||
        outer.uncompressed_len > kMaxArchiveBlockBytes || outer.encoded_len < kGpuBytePlaneHeaderBytes ||
        encoded.size() < kGpuBytePlaneHeaderBytes || outer.encoded_len >= outer.uncompressed_len) {
        throw ArchiveError("GPU byte-plane context framing is invalid");
    }
    const auto width = static_cast<std::uint8_t>(encoded[0]);
    if (!is_gpu_byte_plane_width(width) || outer.uncompressed_len < width || encoded[1] != std::byte{0} ||
        encoded[2] != std::byte{0} || encoded[3] != std::byte{0}) {
        throw ArchiveError("GPU byte-plane context header is noncanonical");
    }
    const auto header_bytes = kGpuBytePlaneHeaderBytes + width * kGpuBytePlaneContextRecordBytes;
    if (encoded.size() < header_bytes || outer.encoded_len < header_bytes) {
        throw ArchiveError("GPU byte-plane context table is truncated");
    }
    GpuBytePlaneContextHeader stages{.width = width, .count = width, .header_bytes = header_bytes};
    const auto payload_bytes = outer.encoded_len - header_bytes;
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
        const BlockDescriptor stage{.kind = kind,
                                    .fill_value = fill,
                                    .uncompressed_len = decoded,
                                    .encoded_offset = cursor,
                                    .encoded_len = bytes};
        if (!is_canonical_gpu_byte_plane_context_stage(stage)) {
            throw ArchiveError("GPU byte-plane context stage is unsupported or noncanonical");
        }
        if (cursor > payload_bytes || bytes > payload_bytes - cursor) {
            throw ArchiveError("GPU byte-plane context stage is unsupported or noncanonical");
        }
        stages.blocks[plane] = stage;
        cursor += bytes;
    }
    if (cursor != payload_bytes) {
        throw ArchiveError("GPU byte-plane context frame contains trailing bytes");
    }
    return stages;
}

// Admit exact frame containment as well as its table. The returned payload borrows encoded.
inline GpuBytePlaneContextStages parse_gpu_byte_plane_contexts(std::span<const std::byte> encoded,
                                                               const BlockDescriptor& outer) {
    if (encoded.size() != outer.encoded_len) {
        throw ArchiveError("GPU byte-plane context framing is invalid");
    }
    const auto header = parse_gpu_byte_plane_context_header(encoded, outer);
    return {.width = header.width,
            .count = header.count,
            .blocks = header.blocks,
            .payload = encoded.subspan(header.header_bytes)};
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

}  // namespace flitzzip
