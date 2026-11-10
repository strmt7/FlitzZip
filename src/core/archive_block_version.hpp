#pragma once

#include "core/archive_index.hpp"
#include "core/byte_plane_block.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace flitzzip {

struct EncodedBlockVersionView {
    std::span<const std::byte> payload;
    std::uint32_t minimum_version;
};

inline constexpr std::size_t kMaxEncodedBlockVersionPrefixBytes =
    kGpuBytePlaneHeaderBytes + 8U * kGpuBytePlaneContextRecordBytes;

// Maximum prefix needed to inspect advertised child kinds; plain blocks need no payload read.
inline constexpr std::size_t encoded_block_version_prefix_bytes(BlockKind kind) noexcept {
    switch (kind) {
    case BlockKind::GpuCompound:
        return kGpuCompoundHeaderBytes;
    case BlockKind::GpuBytePlane:
        return kGpuBytePlaneHeaderBytes;
    case BlockKind::GpuBytePlaneContexts:
        return kMaxEncodedBlockVersionPrefixBytes;
    default:
        return 0U;
    }
}

// Inspect fixed headers against the descriptor's complete declared geometry.
// The prefix must contain every composition header/table byte; codec bodies and
// actual file containment require separate admission. No returned descriptor borrows bytes.
inline std::uint32_t minimum_archive_version_for_encoded_header(const BlockDescriptor& block,
                                                                std::span<const std::byte> prefix) {
    auto minimum = minimum_archive_version_for_block(block);
    if (minimum == 0U) {
        throw ArchiveError("archive block has unknown encoding kind");
    }
    const auto include_stage = [&](const BlockDescriptor& stage) {
        const auto required = minimum_archive_version_for_block(stage);
        if (required == 0U) {
            throw ArchiveError("archive composition has unknown encoding kind");
        }
        minimum = std::max(minimum, required);
    };
    if (block.kind == BlockKind::GpuCompound) {
        const auto stages = parse_gpu_compound_header(prefix, block);
        include_stage(stages.inner);
        include_stage(stages.original);
    } else if (block.kind == BlockKind::GpuBytePlane) {
        include_stage(parse_gpu_byte_plane_header(prefix, block).inner);
    } else if (block.kind == BlockKind::GpuBytePlaneContexts) {
        const auto stages = parse_gpu_byte_plane_context_header(prefix, block);
        for (std::size_t plane = 0; plane < stages.count; ++plane) {
            include_stage(stages.blocks[plane]);
        }
    }
    return minimum;
}

// Inspect one descriptor while its complete encoded owner window is available.
// Offsets are relative to that window; the returned view borrows its lifetime.
// Fixed-header/version admission does not establish codec-body or CRC validity.
inline EncodedBlockVersionView inspect_encoded_block_version(const BlockDescriptor& block,
                                                             std::span<const std::byte> payload) {
    if (block.encoded_offset > payload.size() || block.encoded_len > payload.size() - block.encoded_offset) {
        throw ArchiveError("archive block version inspection exceeds its payload window");
    }
    const auto encoded = block.encoded_len == 0U
                             ? std::span<const std::byte>{}
                             : payload.subspan(static_cast<std::size_t>(block.encoded_offset), block.encoded_len);
    return {.payload = encoded, .minimum_version = minimum_archive_version_for_encoded_header(block, encoded)};
}

}  // namespace flitzzip
