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

// Inspect one descriptor while its complete encoded owner window is available.
// Offsets are relative to that window; the returned view borrows its lifetime.
// Reject unknown kinds, invalid extents and noncanonical composition headers.
// Include both compound stages and every independent plane in the version floor.
// This does not replace decoded-size, codec-body, CRC or archive-layout admission.
inline EncodedBlockVersionView inspect_encoded_block_version(const BlockDescriptor& block,
                                                             std::span<const std::byte> payload) {
    auto minimum = minimum_archive_version_for_block(block);
    if (minimum == 0U) {
        throw ArchiveError("archive block has unknown encoding kind");
    }
    if (block.encoded_offset > payload.size() || block.encoded_len > payload.size() - block.encoded_offset) {
        throw ArchiveError("archive block version inspection exceeds its payload window");
    }
    const auto encoded = block.encoded_len == 0U
                             ? std::span<const std::byte>{}
                             : payload.subspan(static_cast<std::size_t>(block.encoded_offset), block.encoded_len);
    const auto include_stage = [&](const BlockDescriptor& stage) {
        const auto required = minimum_archive_version_for_block(stage);
        if (required == 0U) {
            throw ArchiveError("archive composition has unknown encoding kind");
        }
        minimum = std::max(minimum, required);
    };
    if (block.kind == BlockKind::GpuCompound) {
        const auto stages = parse_gpu_compound_block(encoded, block);
        include_stage(stages.inner);
        include_stage(stages.original);
    } else if (is_gpu_byte_plane_kind(block.kind)) {
        const auto stages = parse_gpu_byte_plane_stages(encoded, block);
        for (std::size_t plane = 0; plane < stages.count; ++plane) {
            include_stage(stages.blocks[plane]);
        }
    }
    return {.payload = encoded, .minimum_version = minimum};
}

}  // namespace flitzzip
