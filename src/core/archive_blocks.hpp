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
    int compression_level = kDefaultCompressionLevel;
};

// Purpose: Describe one contiguous decode window without allocating or owning block metadata.
// Inputs: Populated by resolve_decode_block_window from bounded descriptors.
// Outputs: end is the exclusive block index; uncompressed_size is the exact decoded byte sum.
struct DecodeBlockWindow {
    std::size_t end = 0;
    std::uint64_t uncompressed_size = 0;
};

// Purpose: Group complete blocks under the decoded-byte window limit used by execution and worker admission.
// Inputs: blocks is borrowed metadata; first is an index through size; window_bytes is the positive byte limit.
// Outputs: Returns a nonempty window unless at end; throws ArchiveError on invalid indices, lengths, or limits.
DecodeBlockWindow resolve_decode_block_window(std::span<const BlockDescriptor> blocks, std::size_t first,
                                              std::uint64_t window_bytes);

// Purpose: Count actual complete-block windows rather than estimating from total decoded bytes.
// Inputs: blocks and window_bytes follow resolve_decode_block_window's borrowed metadata and byte-limit contract.
// Outputs: Returns the exact count, zero for no blocks; throws ArchiveError for invalid window metadata.
std::uint64_t count_decode_block_windows(std::span<const BlockDescriptor> blocks, std::uint64_t window_bytes);

// Purpose: Identify block kinds that carry bytes in the encoded payload stream.
// Inputs: `kind` is a native SUZIP block kind.
// Outputs: Returns true for raw, CPU-compressed, and GPU-compressed payload blocks.
bool block_kind_has_payload(BlockKind kind);

// Purpose: Encode a chunk with the bounded CPU archive codec.
// Inputs: `input` is uncompressed data and `options` controls block size, workers, and codec effort.
// Outputs: Returns encoded descriptors and payload; throws `ArchiveError` on invalid limits.
EncodedChunk encode_chunk_cpu(std::span<const std::byte> input, const ArchiveCodecOptions& options);

// Purpose: Detect whether a block table contains a CPU-only compression kind.
// Inputs: `blocks` is a validated or soon-to-be-validated native SUZIP block table.
// Outputs: Returns true when any block uses Deflate or Zstandard.
bool block_table_contains_cpu_only(std::span<const BlockDescriptor> blocks);

// Purpose: Decode only CPU-compressed blocks into an already allocated decoded chunk buffer.
// Inputs: `payload` and `blocks` describe encoded archive bytes, `output` is the decoded chunk buffer, and `options`
// supplies worker count. Outputs: Writes CPU-compressed block ranges into `output`; throws on malformed
// metadata or codec failure.
void decode_cpu_only_blocks_cpu(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                                std::span<std::byte> output, const ArchiveCodecOptions& options);

// Purpose: Decode a chunk entirely through the CPU archive codec.
// Inputs: `payload` and `blocks` describe encoded archive bytes, `output` is the exact decoded buffer, and `options`
// supplies worker count. Outputs: Writes decoded bytes into `output`; throws `ArchiveError` on malformed metadata or
// inflate failure.
void decode_chunk_cpu(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                      std::span<std::byte> output, const ArchiveCodecOptions& options);

}  // namespace superzip
