#pragma once

#include "core/archive_block_types.hpp"
#include "core/resource_limits.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace superzip {

struct EncodedChunk {
    std::vector<BlockDescriptor> blocks;
    std::vector<std::byte> payload;
    std::uint32_t source_crc32 = 0;
    bool gpu_used = false;
    bool source_crc32_available = false;
};

struct ArchiveCodecOptions {
    std::uint32_t block_size = kDefaultArchiveBlockBytes;
    std::uint32_t worker_count = 1;
    int compression_level = 1;
};

// Purpose: Identify block kinds that carry bytes in the encoded payload stream.
// Inputs: `kind` is a native SUZIP block kind.
// Outputs: Returns true for raw, deflate, and GPU pattern payload blocks.
bool block_kind_has_payload(BlockKind kind);

// Purpose: Encode a chunk with the bounded CPU archive codec.
// Inputs: `input` is uncompressed data and `options` controls block size, workers, and miniz deflate level.
// Outputs: Returns encoded descriptors and payload; throws `ArchiveError` on invalid limits.
EncodedChunk encode_chunk_cpu(std::span<const std::byte> input, const ArchiveCodecOptions& options);

// Purpose: Detect whether a block table requires CPU deflate inflation after GPU materialization.
// Inputs: `blocks` is a validated or soon-to-be-validated native SUZIP block table.
// Outputs: Returns true when any block is deflate-compressed.
bool block_table_contains_deflate(std::span<const BlockDescriptor> blocks);

// Purpose: Inflate only deflate blocks into an already allocated decoded chunk buffer.
// Inputs: `payload` and `blocks` describe encoded archive bytes, `output` is the decoded chunk buffer, and `options` supplies worker count.
// Outputs: Writes deflated block ranges into `output`; throws `ArchiveError` on malformed metadata or inflate failure.
void decode_deflate_blocks_cpu(
    std::span<const std::byte> payload,
    std::span<const BlockDescriptor> blocks,
    std::span<std::byte> output,
    const ArchiveCodecOptions& options);

// Purpose: Decode a chunk entirely through the CPU archive codec.
// Inputs: `payload` and `blocks` describe encoded archive bytes, `output` is the exact decoded buffer, and `options` supplies worker count.
// Outputs: Writes decoded bytes into `output`; throws `ArchiveError` on malformed metadata or inflate failure.
void decode_chunk_cpu(
    std::span<const std::byte> payload,
    std::span<const BlockDescriptor> blocks,
    std::span<std::byte> output,
    const ArchiveCodecOptions& options);

}  // namespace superzip
