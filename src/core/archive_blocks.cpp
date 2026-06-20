#include "core/archive_blocks.hpp"

#include "core/result.hpp"

#include "miniz.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <future>
#include <string>
#include <utility>

namespace superzip {
namespace {

struct EncodedBlockWork {
    BlockDescriptor descriptor;
    std::vector<std::byte> payload;
};

// Purpose: Validate CPU archive codec options before block work begins.
// Inputs: `options` contains block size, worker count, and deflate level.
// Outputs: Returns normally for bounded settings; throws `ArchiveError` otherwise.
void validate_encode_options(const ArchiveCodecOptions& options) {
    if (options.block_size < kMinArchiveBlockBytes || options.block_size > kMaxArchiveBlockBytes) {
        throw ArchiveError("codec block size is outside SuperZip resource limits");
    }
    if (options.compression_level < kMinCompressionLevel || options.compression_level > kMaxCompressionLevel) {
        throw ArchiveError("codec compression level must be between 1 and 9");
    }
}

// Purpose: Validate CPU decode options before materialization begins.
// Inputs: `options` contains the caller-selected block size and worker count.
// Outputs: Returns normally for bounded settings; throws `ArchiveError` otherwise.
void validate_decode_options(const ArchiveCodecOptions& options) {
    if (options.block_size < kMinArchiveBlockBytes || options.block_size > kMaxArchiveBlockBytes) {
        throw ArchiveError("codec block size is outside SuperZip resource limits");
    }
}

// Purpose: Reject direct codec spans that exceed the bounded archive chunk contract.
// Inputs: `size` is a caller-provided span length and `label` names the span.
// Outputs: Returns normally while bounded; throws `ArchiveError` before large allocations.
void reject_oversized_codec_span(std::size_t size, const char* label) {
    if (size > kMaxArchiveChunkBytes) {
        throw ArchiveError(std::string(label) + " exceeds SuperZip codec resource limit");
    }
}

// Purpose: Run a bounded parallel loop over contiguous index ranges.
// Inputs: `count` is the number of work items, `worker_count` is the requested worker budget, and `fn` processes
// `[begin,end)`. Outputs: Blocks until all ranges finish; propagates worker exceptions.
template <typename Fn> void run_parallel_ranges(std::size_t count, std::uint32_t worker_count, Fn fn) {
    if (count == 0) {
        return;
    }
    const auto workers = std::min<std::size_t>(std::max<std::uint32_t>(1U, worker_count), count);
    if (workers == 1) {
        fn(0, count);
        return;
    }
    const auto items_per_worker = (count + workers - 1U) / workers;
    std::vector<std::future<void>> futures;
    futures.reserve(workers - 1U);
    std::size_t begin = 0;
    for (std::size_t worker = 1; worker < workers; ++worker) {
        const auto end = std::min<std::size_t>(count, begin + items_per_worker);
        futures.push_back(std::async(std::launch::async, [begin, end, &fn] { fn(begin, end); }));
        begin = end;
    }
    fn(begin, count);
    for (auto& future : futures) {
        future.get();
    }
}

// Purpose: Detect whether a block is a repeated byte value.
// Inputs: `bytes` is the block to inspect and `value` receives the repeated byte when true.
// Outputs: Returns true for empty or uniform blocks; does not allocate.
bool block_is_fill(std::span<const std::byte> bytes, std::uint8_t& value) {
    if (bytes.empty()) {
        value = 0;
        return true;
    }
    value = static_cast<std::uint8_t>(bytes.front());
    return std::ranges::all_of(bytes, [&](std::byte byte) { return static_cast<std::uint8_t>(byte) == value; });
}

// Purpose: Estimate whether a block is worth sending through deflate.
// Inputs: `bytes` is a non-fill block.
// Outputs: Returns true for blocks with repeated sampled bytes that are likely compressible.
bool block_is_likely_compressible(std::span<const std::byte> bytes) {
    if (bytes.size() < 512U) {
        return false;
    }
    std::array<bool, 256> seen{};
    std::size_t unique = 0;
    const auto stride = std::max<std::size_t>(1U, bytes.size() / 512U);
    std::size_t samples = 0;
    for (std::size_t i = 0; i < bytes.size() && samples < 512U; i += stride, ++samples) {
        const auto value = static_cast<std::uint8_t>(bytes[i]);
        if (!seen[value]) {
            seen[value] = true;
            ++unique;
        }
    }
    return samples > 0 && unique * 100U < samples * 85U;
}

// Purpose: Try miniz deflate for one bounded block.
// Inputs: `block` is a non-fill block to compress and `compression_level` selects the miniz effort.
// Outputs: Returns compressed bytes when smaller than raw, otherwise an empty vector.
std::vector<std::byte> try_deflate_block(std::span<const std::byte> block, int compression_level) {
    if (!block_is_likely_compressible(block)) {
        return {};
    }
    mz_ulong bound = compressBound(static_cast<mz_ulong>(block.size()));
    std::vector<std::byte> compressed(static_cast<std::size_t>(bound));
    const auto status = compress2(reinterpret_cast<unsigned char*>(compressed.data()), &bound,
                                  reinterpret_cast<const unsigned char*>(block.data()),
                                  static_cast<mz_ulong>(block.size()), compression_level);
    if (status != MZ_OK || bound >= block.size()) {
        return {};
    }
    compressed.resize(static_cast<std::size_t>(bound));
    return compressed;
}

// Purpose: Inflate one miniz deflate payload into the caller-owned output span.
// Inputs: `payload`, `encoded_offset`, `encoded_len`, and `output` describe one block.
// Outputs: Writes exactly `output.size()` bytes or throws `ArchiveError`.
void inflate_deflate_block(std::span<const std::byte> payload, std::uint64_t encoded_offset, std::uint32_t encoded_len,
                           std::span<std::byte> output) {
    mz_ulong output_len = static_cast<mz_ulong>(output.size());
    const auto status =
        uncompress(reinterpret_cast<unsigned char*>(output.data()), &output_len,
                   reinterpret_cast<const unsigned char*>(payload.data() + static_cast<std::size_t>(encoded_offset)),
                   static_cast<mz_ulong>(encoded_len));
    if (status != MZ_OK || output_len != output.size()) {
        throw ArchiveError("deflate block failed to decompress");
    }
}

// Purpose: Materialize one repeated pattern into an output block.
// Inputs: `pattern` is the compact pattern payload and `output` is the expanded destination span.
// Outputs: Writes `output.size()` bytes by repeating the compact pattern.
void materialize_pattern_cpu(std::span<const std::byte> pattern, std::span<std::byte> output) {
    if (pattern.size() < 2 || pattern.size() > kMaxGpuPatternBytes || pattern.size() >= output.size()) {
        throw ArchiveError("GPU pattern block metadata is invalid");
    }
    for (std::size_t i = 0; i < output.size(); ++i) {
        output[i] = pattern[i % pattern.size()];
    }
}

// Purpose: Read one little-endian GPU prefix table entry.
// Inputs: `payload` is the block payload and `offset` is the table byte offset.
// Outputs: Returns the decoded unsigned offset; throws when the table is truncated.
std::uint32_t read_prefix_u32(std::span<const std::byte> payload, std::size_t offset) {
    if (offset > payload.size() || payload.size() - offset < sizeof(std::uint32_t)) {
        throw ArchiveError("GPU prefix block table is truncated");
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i) {
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload[offset + i])) << (i * 8U);
    }
    return value;
}

// Purpose: Read one bit from a byte-aligned GPU prefix segment.
// Inputs: `stream`, `bit_pos`, and `limit_bits` describe one encoded segment.
// Outputs: Returns the bit and advances `bit_pos`; throws when the segment ends early.
std::uint32_t read_prefix_bit(std::span<const std::byte> stream, std::size_t& bit_pos, std::size_t limit_bits) {
    if (bit_pos >= limit_bits) {
        throw ArchiveError("GPU prefix block ended before decoded bytes were complete");
    }
    const auto byte_index = bit_pos / 8U;
    const auto bit_index = bit_pos % 8U;
    ++bit_pos;
    return (static_cast<std::uint8_t>(stream[byte_index]) >> bit_index) & 1U;
}

// Purpose: Read a small little-endian bit field from a GPU prefix segment.
// Inputs: `stream`, `bit_pos`, `limit_bits`, and `width` describe the encoded field.
// Outputs: Returns the decoded field and advances `bit_pos`; throws when the segment is truncated.
std::uint32_t read_prefix_bits(std::span<const std::byte> stream, std::size_t& bit_pos, std::size_t limit_bits,
                               std::uint32_t width) {
    std::uint32_t value = 0;
    for (std::uint32_t bit = 0; bit < width; ++bit) {
        value |= read_prefix_bit(stream, bit_pos, limit_bits) << bit;
    }
    return value;
}

// Purpose: Decode one byte from SuperZip's static GPU prefix code.
// Inputs: `stream`, `bit_pos`, and `limit_bits` describe one encoded segment.
// Outputs: Returns one decoded byte; throws on malformed group payloads.
std::byte decode_prefix_byte_cpu(std::span<const std::byte> stream, std::size_t& bit_pos, std::size_t limit_bits) {
    if (read_prefix_bit(stream, bit_pos, limit_bits) == 0U) {
        return static_cast<std::byte>(read_prefix_bits(stream, bit_pos, limit_bits, 2));
    }
    if (read_prefix_bit(stream, bit_pos, limit_bits) == 0U) {
        return static_cast<std::byte>(4U + read_prefix_bits(stream, bit_pos, limit_bits, 4));
    }
    if (read_prefix_bit(stream, bit_pos, limit_bits) == 0U) {
        return static_cast<std::byte>(20U + read_prefix_bits(stream, bit_pos, limit_bits, 6));
    }
    const auto value = read_prefix_bits(stream, bit_pos, limit_bits, 8);
    if (value > 171U) {
        throw ArchiveError("GPU prefix block contains an invalid high-byte code");
    }
    return static_cast<std::byte>(84U + value);
}

// Purpose: Decode one byte from SuperZip's adaptive GPU prefix code.
// Inputs: `codebook`, `stream`, `bit_pos`, and `limit_bits` describe one encoded segment.
// Outputs: Returns one decoded byte; throws on malformed group payloads.
std::byte decode_adaptive_prefix_byte_cpu(std::span<const std::byte> codebook, std::span<const std::byte> stream,
                                          std::size_t& bit_pos, std::size_t limit_bits) {
    if (codebook.size() != kGpuAdaptivePrefixCodebookBytes) {
        throw ArchiveError("GPU adaptive prefix codebook is invalid");
    }
    if (read_prefix_bit(stream, bit_pos, limit_bits) == 0U) {
        return codebook[read_prefix_bits(stream, bit_pos, limit_bits, 2)];
    }
    if (read_prefix_bit(stream, bit_pos, limit_bits) == 0U) {
        return codebook[kGpuAdaptivePrefixSmallSymbols + read_prefix_bits(stream, bit_pos, limit_bits, 4)];
    }
    if (read_prefix_bit(stream, bit_pos, limit_bits) == 0U) {
        return codebook[kGpuAdaptivePrefixSmallSymbols + kGpuAdaptivePrefixMediumSymbols +
                        read_prefix_bits(stream, bit_pos, limit_bits, 6)];
    }
    return static_cast<std::byte>(read_prefix_bits(stream, bit_pos, limit_bits, 8));
}

// Purpose: Decode one static-prefix block in bounded CPU memory.
// Inputs: `payload` is the block payload and `output` is the exact decoded destination.
// Outputs: Writes decoded bytes into `output`; throws on malformed table or bitstream metadata.
void materialize_prefix_cpu(std::span<const std::byte> payload, std::span<std::byte> output) {
    const auto segment_count = (output.size() + kGpuPrefixSegmentBytes - 1U) / kGpuPrefixSegmentBytes;
    const auto table_entries = segment_count + 1U;
    const auto table_bytes = table_entries * sizeof(std::uint32_t);
    if (segment_count == 0 || payload.size() <= table_bytes || payload.size() >= output.size()) {
        throw ArchiveError("GPU prefix block metadata is invalid");
    }
    const auto bitstream = payload.subspan(table_bytes);
    std::uint32_t previous = read_prefix_u32(payload, 0);
    if (previous != 0U) {
        throw ArchiveError("GPU prefix block table must start at zero");
    }
    std::size_t decoded_offset = 0;
    for (std::size_t segment = 0; segment < segment_count; ++segment) {
        const auto next = read_prefix_u32(payload, (segment + 1U) * sizeof(std::uint32_t));
        if (next < previous || next > bitstream.size()) {
            throw ArchiveError("GPU prefix block table is not monotonic");
        }
        const auto segment_output_len = std::min<std::size_t>(kGpuPrefixSegmentBytes, output.size() - decoded_offset);
        const auto encoded = bitstream.subspan(previous, next - previous);
        std::size_t bit_pos = 0;
        const auto limit_bits = encoded.size() * 8U;
        for (std::size_t i = 0; i < segment_output_len; ++i) {
            output[decoded_offset + i] = decode_prefix_byte_cpu(encoded, bit_pos, limit_bits);
        }
        decoded_offset += segment_output_len;
        previous = next;
    }
    if (previous != bitstream.size()) {
        throw ArchiveError("GPU prefix block payload has trailing bytes");
    }
}

// Purpose: Decode one adaptive-prefix block in bounded CPU memory.
// Inputs: `payload` is the block payload and `output` is the exact decoded destination.
// Outputs: Writes decoded bytes into `output`; throws on malformed codebook, table, or bitstream metadata.
void materialize_adaptive_prefix_cpu(std::span<const std::byte> payload, std::span<std::byte> output) {
    const auto segment_count = (output.size() + kGpuPrefixSegmentBytes - 1U) / kGpuPrefixSegmentBytes;
    const auto table_entries = segment_count + 1U;
    const auto table_bytes = table_entries * sizeof(std::uint32_t);
    const auto header_bytes = kGpuAdaptivePrefixCodebookBytes + table_bytes;
    if (segment_count == 0 || payload.size() <= header_bytes || payload.size() >= output.size()) {
        throw ArchiveError("GPU adaptive prefix block metadata is invalid");
    }
    const auto codebook = payload.first(kGpuAdaptivePrefixCodebookBytes);
    const auto table = payload.subspan(kGpuAdaptivePrefixCodebookBytes, table_bytes);
    const auto bitstream = payload.subspan(header_bytes);
    std::uint32_t previous = read_prefix_u32(table, 0);
    if (previous != 0U) {
        throw ArchiveError("GPU adaptive prefix block table must start at zero");
    }
    std::size_t decoded_offset = 0;
    for (std::size_t segment = 0; segment < segment_count; ++segment) {
        const auto next = read_prefix_u32(table, (segment + 1U) * sizeof(std::uint32_t));
        if (next < previous || next > bitstream.size()) {
            throw ArchiveError("GPU adaptive prefix block table is not monotonic");
        }
        const auto segment_output_len = std::min<std::size_t>(kGpuPrefixSegmentBytes, output.size() - decoded_offset);
        const auto encoded = bitstream.subspan(previous, next - previous);
        std::size_t bit_pos = 0;
        const auto limit_bits = encoded.size() * 8U;
        for (std::size_t i = 0; i < segment_output_len; ++i) {
            output[decoded_offset + i] = decode_adaptive_prefix_byte_cpu(codebook, encoded, bit_pos, limit_bits);
        }
        decoded_offset += segment_output_len;
        previous = next;
    }
    if (previous != bitstream.size()) {
        throw ArchiveError("GPU adaptive prefix block payload has trailing bytes");
    }
}

// Purpose: Compute output offsets and validate block spans before parallel decode.
// Inputs: `blocks`, `payload`, and `output` are caller-provided decode buffers.
// Outputs: Returns one output offset per block; throws on invalid bounds.
std::vector<std::size_t> validate_decode_blocks(std::span<const std::byte> payload,
                                                std::span<const BlockDescriptor> blocks, std::span<std::byte> output) {
    std::vector<std::size_t> offsets(blocks.size());
    std::size_t out_pos = 0;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        offsets[i] = out_pos;
        const auto& block = blocks[i];
        const auto len = static_cast<std::size_t>(block.uncompressed_len);
        if (out_pos > output.size() || len > output.size() - out_pos) {
            throw ArchiveError("decoded block exceeds output buffer");
        }
        if (block_kind_has_payload(block.kind)) {
            const auto offset = static_cast<std::size_t>(block.encoded_offset);
            const auto encoded_len = static_cast<std::size_t>(block.encoded_len);
            if (encoded_len == 0 || offset > payload.size() || encoded_len > payload.size() - offset) {
                throw ArchiveError("encoded block exceeds payload buffer");
            }
            if (block.kind == BlockKind::Raw && encoded_len != len) {
                throw ArchiveError("raw block length does not match decoded length");
            }
            if (block.kind == BlockKind::Pattern &&
                (encoded_len < 2 || encoded_len > kMaxGpuPatternBytes || encoded_len >= len)) {
                throw ArchiveError("GPU pattern block metadata is invalid");
            }
            if (block.kind == BlockKind::GpuPrefix) {
                const auto segment_count = (len + kGpuPrefixSegmentBytes - 1U) / kGpuPrefixSegmentBytes;
                const auto table_bytes = (segment_count + 1U) * sizeof(std::uint32_t);
                if (encoded_len <= table_bytes || encoded_len >= len) {
                    throw ArchiveError("GPU prefix block metadata is invalid");
                }
            }
            if (block.kind == BlockKind::GpuAdaptivePrefix) {
                const auto segment_count = (len + kGpuPrefixSegmentBytes - 1U) / kGpuPrefixSegmentBytes;
                const auto header_bytes =
                    kGpuAdaptivePrefixCodebookBytes + ((segment_count + 1U) * sizeof(std::uint32_t));
                if (encoded_len <= header_bytes || encoded_len >= len) {
                    throw ArchiveError("GPU adaptive prefix block metadata is invalid");
                }
            }
        } else if (block.kind == BlockKind::Fill) {
            if (block.encoded_len != 0) {
                throw ArchiveError("fill block contains encoded payload bytes");
            }
        } else {
            throw ArchiveError("unknown block kind");
        }
        out_pos += len;
    }
    if (out_pos != output.size()) {
        throw ArchiveError("decoded size does not match expected output size");
    }
    return offsets;
}

// Purpose: Copy, fill, inflate, or expand decoded blocks over a validated block table.
// Inputs: `payload`, `blocks`, `offsets`, `output`, and `worker_count` describe bounded decode work.
// Outputs: Writes decoded bytes into `output`.
void materialize_blocks_cpu(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                            std::span<const std::size_t> offsets, std::span<std::byte> output,
                            std::uint32_t worker_count) {
    run_parallel_ranges(blocks.size(), worker_count, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const auto& block = blocks[i];
            const auto len = static_cast<std::size_t>(block.uncompressed_len);
            const auto out_pos = offsets[i];
            if (block.kind == BlockKind::Fill) {
                std::ranges::fill(output.subspan(out_pos, len), static_cast<std::byte>(block.fill_value));
            } else if (block.kind == BlockKind::Raw) {
                std::copy_n(payload.data() + static_cast<std::size_t>(block.encoded_offset), len,
                            output.data() + out_pos);
            } else if (block.kind == BlockKind::Deflate) {
                inflate_deflate_block(payload, block.encoded_offset, block.encoded_len, output.subspan(out_pos, len));
            } else if (block.kind == BlockKind::Pattern) {
                materialize_pattern_cpu(payload.subspan(static_cast<std::size_t>(block.encoded_offset),
                                                        static_cast<std::size_t>(block.encoded_len)),
                                        output.subspan(out_pos, len));
            } else if (block.kind == BlockKind::GpuPrefix) {
                materialize_prefix_cpu(payload.subspan(static_cast<std::size_t>(block.encoded_offset),
                                                       static_cast<std::size_t>(block.encoded_len)),
                                       output.subspan(out_pos, len));
            } else if (block.kind == BlockKind::GpuAdaptivePrefix) {
                materialize_adaptive_prefix_cpu(payload.subspan(static_cast<std::size_t>(block.encoded_offset),
                                                                static_cast<std::size_t>(block.encoded_len)),
                                                output.subspan(out_pos, len));
            } else {
                throw ArchiveError("unknown block kind");
            }
        }
    });
}

}  // namespace

// Purpose: Report whether a SUZIP block kind reserves bytes in the encoded payload window.
// Inputs: `kind` is a native archive block encoding tag.
// Outputs: Returns true for raw, deflate, GPU-pattern, and GPU-prefix block payloads.
bool block_kind_has_payload(BlockKind kind) {
    return kind == BlockKind::Raw || kind == BlockKind::Deflate || kind == BlockKind::Pattern ||
           kind == BlockKind::GpuPrefix || kind == BlockKind::GpuAdaptivePrefix;
}

EncodedChunk encode_chunk_cpu(std::span<const std::byte> input, const ArchiveCodecOptions& options) {
    validate_encode_options(options);
    reject_oversized_codec_span(input.size(), "codec input");

    EncodedChunk out;
    const auto block_size = std::max<std::uint32_t>(1U, options.block_size);
    const auto block_count = (input.size() + block_size - 1U) / block_size;
    std::vector<EncodedBlockWork> block_work(block_count);

    run_parallel_ranges(block_count, options.worker_count, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const auto pos = i * static_cast<std::size_t>(block_size);
            const auto len = static_cast<std::uint32_t>(std::min<std::size_t>(block_size, input.size() - pos));
            const auto block = input.subspan(pos, len);
            std::uint8_t fill = 0;
            if (block_is_fill(block, fill)) {
                block_work[i].descriptor = BlockDescriptor{
                    .kind = BlockKind::Fill,
                    .fill_value = fill,
                    .uncompressed_len = len,
                    .encoded_offset = 0,
                    .encoded_len = 0,
                };
            } else {
                auto compressed = try_deflate_block(block, options.compression_level);
                if (!compressed.empty()) {
                    block_work[i].descriptor = BlockDescriptor{
                        .kind = BlockKind::Deflate,
                        .fill_value = 0,
                        .uncompressed_len = len,
                        .encoded_offset = 0,
                        .encoded_len = static_cast<std::uint32_t>(compressed.size()),
                    };
                    block_work[i].payload = std::move(compressed);
                } else {
                    block_work[i].descriptor = BlockDescriptor{
                        .kind = BlockKind::Raw,
                        .fill_value = 0,
                        .uncompressed_len = len,
                        .encoded_offset = 0,
                        .encoded_len = len,
                    };
                }
            }
        }
    });

    out.blocks.resize(block_count);
    std::uint64_t encoded_offset = 0;
    bool all_raw = true;
    for (std::size_t i = 0; i < block_work.size(); ++i) {
        out.blocks[i] = block_work[i].descriptor;
        if (block_kind_has_payload(out.blocks[i].kind)) {
            out.blocks[i].encoded_offset = encoded_offset;
            encoded_offset += out.blocks[i].encoded_len;
        }
        if (out.blocks[i].kind != BlockKind::Raw) {
            all_raw = false;
        }
    }
    if (encoded_offset == 0) {
        return out;
    }
    if (all_raw) {
        out.payload.resize(input.size());
        run_parallel_ranges(out.blocks.size(), options.worker_count, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                const auto offset = i * static_cast<std::size_t>(block_size);
                const auto len = static_cast<std::size_t>(out.blocks[i].uncompressed_len);
                std::copy_n(input.data() + offset, len, out.payload.data() + offset);
            }
        });
        return out;
    }

    out.payload.resize(static_cast<std::size_t>(encoded_offset));
    run_parallel_ranges(out.blocks.size(), options.worker_count, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const auto& descriptor = out.blocks[i];
            if (descriptor.kind == BlockKind::Fill) {
                continue;
            }
            if (descriptor.kind == BlockKind::Deflate) {
                std::copy(block_work[i].payload.begin(), block_work[i].payload.end(),
                          out.payload.begin() + static_cast<std::ptrdiff_t>(descriptor.encoded_offset));
                continue;
            }
            const auto source_offset = i * static_cast<std::size_t>(block_size);
            const auto len = static_cast<std::size_t>(descriptor.uncompressed_len);
            std::copy_n(input.data() + source_offset, len,
                        out.payload.data() + static_cast<std::size_t>(descriptor.encoded_offset));
        }
    });
    return out;
}

// Purpose: Report whether a decoded SUZIP block table needs CPU Deflate handling.
// Inputs: `blocks` is a validated or soon-to-be-validated block descriptor table.
// Outputs: Returns true when any block is encoded with Deflate.
bool block_table_contains_deflate(std::span<const BlockDescriptor> blocks) {
    return std::ranges::any_of(blocks, [](const BlockDescriptor& block) { return block.kind == BlockKind::Deflate; });
}

// Purpose: Decode only the Deflate blocks in a SUZIP chunk into an existing output buffer.
// Inputs: `payload`, `blocks`, `output`, and `options` describe one archive chunk and CPU decode settings.
// Outputs: Mutates `output` for Deflate blocks; throws on malformed metadata or miniz failures.
void decode_deflate_blocks_cpu(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                               std::span<std::byte> output, const ArchiveCodecOptions& options) {
    validate_decode_options(options);
    reject_oversized_codec_span(payload.size(), "codec payload");
    reject_oversized_codec_span(output.size(), "codec output");
    if (blocks.size() > kMaxBlocksPerEntry) {
        throw ArchiveError("codec block table exceeds SuperZip resource limit");
    }
    const auto offsets = validate_decode_blocks(payload, blocks, output);
    run_parallel_ranges(blocks.size(), options.worker_count, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const auto& block = blocks[i];
            if (block.kind == BlockKind::Deflate) {
                inflate_deflate_block(payload, block.encoded_offset, block.encoded_len,
                                      output.subspan(offsets[i], static_cast<std::size_t>(block.uncompressed_len)));
            }
        }
    });
}

// Purpose: Decode one SUZIP chunk on the CPU.
// Inputs: `payload`, `blocks`, `output`, and `options` describe the encoded chunk and CPU decode settings.
// Outputs: Fills `output` with decoded bytes; throws on malformed metadata or unsupported block shapes.
void decode_chunk_cpu(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                      std::span<std::byte> output, const ArchiveCodecOptions& options) {
    validate_decode_options(options);
    reject_oversized_codec_span(payload.size(), "codec payload");
    reject_oversized_codec_span(output.size(), "codec output");
    if (blocks.size() > kMaxBlocksPerEntry) {
        throw ArchiveError("codec block table exceeds SuperZip resource limit");
    }
    const auto offsets = validate_decode_blocks(payload, blocks, output);
    materialize_blocks_cpu(payload, blocks, offsets, output, options.worker_count);
}

}  // namespace superzip
