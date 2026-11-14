#pragma once

#include "core/archive_block_types.hpp"
#include "core/host_memory_budget.hpp"
#include "core/resource_limits.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace flitzzip {

struct EncodedChunk {
    std::vector<BlockDescriptor> blocks;
    std::vector<std::byte> payload;
    std::uint32_t source_crc32 = 0;
    bool gpu_used = false;
    bool source_crc32_available = false;
};

inline constexpr std::size_t kMaxEncodeBatchBlocks = 256;

// One descriptor and finalized source CRC per independent block, in input order.
// Payload offsets refer to encoded.payload; the combined source CRC covers all input.
struct EncodedBlockBatch {
    EncodedChunk encoded;
    std::vector<std::uint32_t> block_crc32;
};

struct ArchiveCodecOptions {
    std::uint32_t block_size = kDefaultArchiveBlockBytes;
    std::uint32_t worker_count = 1;
    int compression_level = kDefaultCompressionLevel;
};

// Purpose: Enforce the shared native codec block-size interval at every direct CPU/HIP boundary.
// Inputs: `block_size` is the requested decoded block extent. Outputs: Returns for the inclusive supported range;
// throws `ArchiveError` otherwise.
void validate_codec_block_size(std::uint32_t block_size);

// Purpose: Enforce the shared native codec chunk limit without requiring a caller to allocate the rejected extent.
// Inputs: `size` is a proposed span length and `label` identifies it in diagnostics. Outputs: Returns while bounded;
// throws `ArchiveError` above the resource limit.
void validate_codec_span_size(std::size_t size, const char* label);

// Validate exact positive coverage by at most kMaxEncodeBatchBlocks independent
// blocks, each within block_size, before CPU work, HIP dispatch or allocation.
// Empty input requires an empty table. Invalid codec or layout bounds throw ArchiveError.
void validate_encode_batch(std::size_t input_size, std::span<const std::uint32_t> lengths, std::uint32_t block_size);

// Purpose: Bound native CPU encode workspace before allocating queued input windows.
// Inputs: A nonzero bounded chunk, validated codec options, and a 1..64 aggregate worker budget.
// Outputs: Returns metadata/context/trial overhead beyond three payload buffers; throws on invalid bounds.
HostPipelineWorkspace cpu_encode_workspace_estimate(std::uint64_t chunk_size, const ArchiveCodecOptions& options,
                                                    std::uint32_t aggregate_workers);

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
// Inputs: `kind` is a native FLZIP block kind.
// Outputs: Returns true for raw, CPU-compressed, and GPU-compressed payload blocks.
bool block_kind_has_payload(BlockKind kind);

// Purpose: Encode a chunk with the bounded CPU archive codec.
// Inputs: `input` is uncompressed data and `options` controls block size, workers, and codec effort.
// Outputs: Returns encoded descriptors and payload; throws `ArchiveError` on invalid limits.
// Workers finish before payload publication. A sole compact compressed frame
// transfers its allocation, including among payload-free fills; spare search
// capacity is released through dense assembly instead of retained in the result.
EncodedChunk encode_chunk_cpu(std::span<const std::byte> input, const ArchiveCodecOptions& options);

// Encode owned CPU input with the same block policy as encode_chunk_cpu and a complete source CRC.
// Transfers the input allocation into an all-raw result; other representations release it on return.
// No borrowed input survives the call. Invalid options or codec failures throw before publication.
EncodedChunk encode_owned_chunk_cpu(std::vector<std::byte> input, const ArchiveCodecOptions& options);

// Encode validated independent CPU frames with one serial, reusable codec workspace.
// Validates options and exact block coverage before work. Returns individual and
// combined source CRCs, computed before any original bytes are overwritten.
// Completed blocks compact into input storage without touching unread blocks;
// nonempty payloads transfer that allocation. All-fill batches retain no payload
// storage. Exceptions publish no partial result. No CPU worker fan-out.
EncodedBlockBatch encode_owned_block_batch_cpu(std::vector<std::byte> input,
                                               std::span<const std::uint32_t> block_lengths,
                                               const ArchiveCodecOptions& options);

// Purpose: Detect whether a block table contains a CPU-only compression kind.
// Inputs: `blocks` is a validated or soon-to-be-validated native FLZIP block table.
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

}  // namespace flitzzip
