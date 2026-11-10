#pragma once

#include "core/archive_blocks.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace flitzzip::hip_detail {

struct NeutronBlockReplacement {
    BlockKind kind = BlockKind::Raw;
    std::vector<std::byte> payload;
};

// Selectors borrow the current block until they return an owned, strictly smaller
// frame, or an empty payload to keep it. Dense input offsets keep the write cursor
// behind unread blocks. The original chunk allocation and its metadata stay owned
// by this operation until all selections and extent checks have succeeded.
template <typename Select> EncodedChunk compact_neutron_payload(EncodedChunk chunk, Select&& select) {
    std::size_t read_offset = 0U;
    std::size_t write_offset = 0U;
    for (auto& block : chunk.blocks) {
        const auto original_bytes = block.encoded_len;
        if (block.encoded_offset != read_offset || original_bytes > chunk.payload.size() - read_offset) {
            throw GpuError("Neutron baseline does not contain dense block payloads");
        }

        const auto original = std::span<const std::byte>(chunk.payload).subspan(read_offset, original_bytes);
        const auto replacement = select(std::as_const(block), original);
        const auto improved = !replacement.payload.empty();
        if (improved && replacement.payload.size() >= original_bytes) {
            throw GpuError("Neutron replacement must be smaller than its baseline");
        }

        const auto selected = improved ? std::span<const std::byte>(replacement.payload) : original;
        if (!selected.empty() && (improved || write_offset != read_offset)) {
            // A retained range only moves left. Its destination starts before
            // the source, so forward copy preserves overlapping bytes.
            std::ranges::copy(selected, chunk.payload.begin() + static_cast<std::ptrdiff_t>(write_offset));
        }
        if (improved) {
            block.kind = replacement.kind;
            block.fill_value = 0U;
            block.encoded_len = static_cast<std::uint32_t>(selected.size());
        }
        block.encoded_offset = write_offset;
        read_offset += original_bytes;
        write_offset += selected.size();
    }
    if (read_offset != chunk.payload.size()) {
        throw GpuError("Neutron baseline has unowned payload bytes");
    }
    chunk.payload.resize(write_offset);
    return chunk;
}

}  // namespace flitzzip::hip_detail
