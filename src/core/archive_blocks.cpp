#include "core/archive_blocks.hpp"
#include "core/dictionary_block.hpp"
#include "core/huffman_lookup.hpp"
#include "core/parallel_ranges.hpp"
#include "core/sparse_pattern_block.hpp"

#include "core/result.hpp"
#include "zstd/zstd_runtime.hpp"

#include "miniz.h"
#include "lz4.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace superzip {
namespace {

struct EncodedBlockWork {
    BlockDescriptor descriptor;
    std::vector<std::byte> payload;
};

// Purpose: Validate CPU archive codec options before block work begins.
// Inputs: `options` contains block size, worker count, and CPU codec effort.
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

// Purpose: Retain the smallest complete Deflate representation from a nested bounded effort search.
// Inputs: A non-fill short block and maximum requested effort 1-9.
// Outputs: Returns bytes smaller than raw or empty, preserving earlier frames on ties; throws on codec failure.
std::vector<std::byte> try_deflate_block(std::span<const std::byte> block, int compression_level) {
    // A zlib stream needs six framing bytes plus at least two bytes for its Deflate block.
    if (block.size() <= 8U) {
        return {};
    }
    std::vector<std::byte> trial(block.size() - 1U);
    std::vector<std::byte> best;
    for (int effort = 1; effort <= compression_level; ++effort) {
        auto written = static_cast<mz_ulong>(trial.size());
        const auto status = compress2(reinterpret_cast<unsigned char*>(trial.data()), &written,
                                      reinterpret_cast<const unsigned char*>(block.data()),
                                      static_cast<mz_ulong>(block.size()), effort);
        if (status == MZ_BUF_ERROR) {
            continue;
        }
        if (status != MZ_OK || written > trial.size()) {
            throw ArchiveError("Deflate native block compression failed");
        }
        if (best.empty() || written < best.size()) {
            best.assign(trial.begin(), trial.begin() + static_cast<std::ptrdiff_t>(written));
        }
    }
    return best;
}

// Purpose: Retain the smallest complete Zstandard frame across all policies admitted by the requested effort.
// Inputs: One non-fill block, maximum effort 1-9, an exclusive worker context, and reusable trial storage.
// Outputs: Returns a frame smaller than raw or empty, retaining earlier ties; throws on runtime failure.
std::vector<std::byte> try_zstd_block(std::span<const std::byte> block, int compression_level,
                                      ZstdCompressionContext* context, std::vector<std::byte>& trial) {
    const auto& zstd = zstd_runtime();
    const auto bound = zstd.block_compress_bound(block.size());
    if (zstd.is_error(bound) || bound < block.size() ||
        bound > kMaxArchiveBlockBytes + kMaxArchiveBlockBytes / 128U + 128U) {
        throw ArchiveError("Zstandard native block capacity is invalid");
    }
    trial.resize(bound);
    std::vector<std::byte> best;
    for (int effort = 1; effort <= compression_level; ++effort) {
        const auto written =
            zstd.compress_block_with_context(context, trial.data(), trial.size(), block.data(), block.size(), effort);
        if (zstd.is_error(written)) {
            throw ArchiveError("Zstandard native block compression failed: " + zstd.error_name(written));
        }
        if (written > trial.size()) {
            throw ArchiveError("Zstandard native block encoded length is invalid");
        }
        if (written < block.size() && (best.empty() || written < best.size())) {
            best.assign(trial.begin(), trial.begin() + static_cast<std::ptrdiff_t>(written));
        }
    }
    return best;
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

// Purpose: Decode exactly one bounded version-six CPU frame without accepting trailing or size-lying input.
// Inputs: An untrusted encoded block payload and its already bounded exact output span.
// Outputs: Restores all bytes or throws `ArchiveError` before output publication.
void materialize_zstd_block_cpu(std::span<const std::byte> encoded, std::span<std::byte> output) {
    const auto& zstd = zstd_runtime();
    const auto first_frame = zstd.first_frame_size(encoded.data(), encoded.size());
    if (zstd.is_error(first_frame) || first_frame != encoded.size() ||
        zstd.frame_content_size(encoded.data(), encoded.size()) != output.size()) {
        throw ArchiveError("Zstandard native block frame metadata is invalid");
    }
    const auto written = zstd.decompress_block(output.data(), output.size(), encoded.data(), encoded.size());
    if (zstd.is_error(written) || written != output.size()) {
        throw ArchiveError("Zstandard native block failed to decompress");
    }
}

// Purpose: Materialize one repeated pattern into an output block.
// Inputs: `pattern` is the compact payload, `output` is the destination, and `max_period` is the versioned bound.
// Outputs: Writes `output.size()` bytes by repeating the compact pattern.
void materialize_pattern_cpu(std::span<const std::byte> pattern, std::span<std::byte> output,
                             std::uint32_t max_period = kMaxGpuPatternBytes) {
    if (pattern.size() < 2 || pattern.size() > max_period || pattern.size() >= output.size()) {
        throw ArchiveError("GPU pattern block metadata is invalid");
    }
    std::copy(pattern.begin(), pattern.end(), output.begin());
    std::size_t filled = pattern.size();
    while (filled < output.size()) {
        const auto count = std::min(filled, output.size() - filled);
        std::copy_n(output.begin(), count, output.begin() + static_cast<std::ptrdiff_t>(filled));
        filled += count;
    }
}

// Purpose: Read one little-endian native segment-table entry.
// Inputs: `payload` is the block payload and `offset` is the table byte offset.
// Outputs: Returns the decoded unsigned offset; throws when the table is truncated.
std::uint32_t read_segment_u32(std::span<const std::byte> payload, std::size_t offset) {
    if (offset > payload.size() || payload.size() - offset < sizeof(std::uint32_t)) {
        throw ArchiveError("native block segment table is truncated");
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
    std::uint32_t previous = read_segment_u32(payload, 0);
    if (previous != 0U) {
        throw ArchiveError("GPU prefix block table must start at zero");
    }
    std::size_t decoded_offset = 0;
    for (std::size_t segment = 0; segment < segment_count; ++segment) {
        const auto next = read_segment_u32(payload, (segment + 1U) * sizeof(std::uint32_t));
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
    std::uint32_t previous = read_segment_u32(table, 0);
    if (previous != 0U) {
        throw ArchiveError("GPU adaptive prefix block table must start at zero");
    }
    std::size_t decoded_offset = 0;
    for (std::size_t segment = 0; segment < segment_count; ++segment) {
        const auto next = read_segment_u32(table, (segment + 1U) * sizeof(std::uint32_t));
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

// Purpose: Decode a version-eight GPU Huffman block through its bounded 12-bit lookup table.
// Inputs: `payload` contains lookup entries, segment offsets, and bitstream; `output` is exact decoded storage.
// Outputs: Restores every byte or throws on malformed offsets, lookup widths, or truncated codes.
void materialize_huffman_cpu(std::span<const std::byte> payload, std::span<std::byte> output) {
    const auto segment_count = (output.size() + kGpuPrefixSegmentBytes - 1U) / kGpuPrefixSegmentBytes;
    const auto table_bytes = (segment_count + 1U) * sizeof(std::uint32_t);
    const auto header_bytes = kGpuHuffmanLookupBytes + table_bytes;
    if (segment_count == 0U || payload.size() <= header_bytes || payload.size() >= output.size()) {
        throw ArchiveError("GPU Huffman block metadata is invalid");
    }
    if (!huffman_lookup_is_complete(payload.first(kGpuHuffmanLookupBytes))) {
        throw ArchiveError("GPU Huffman lookup is invalid");
    }
    const auto offsets = payload.subspan(kGpuHuffmanLookupBytes, table_bytes);
    const auto bitstream = payload.subspan(header_bytes);
    auto previous = read_segment_u32(offsets, 0U);
    if (previous != 0U) {
        throw ArchiveError("GPU Huffman table must start at zero");
    }
    std::size_t decoded_offset = 0U;
    for (std::size_t segment = 0U; segment < segment_count; ++segment) {
        const auto next = read_segment_u32(offsets, (segment + 1U) * sizeof(std::uint32_t));
        if (next < previous || next > bitstream.size()) {
            throw ArchiveError("GPU Huffman table is not monotonic");
        }
        const auto encoded = bitstream.subspan(previous, next - previous);
        const auto decoded_len = std::min<std::size_t>(kGpuPrefixSegmentBytes, output.size() - decoded_offset);
        const auto limit_bits = encoded.size() * 8U;
        std::size_t bit_pos = 0U;
        for (std::size_t i = 0U; i < decoded_len; ++i) {
            std::uint32_t code = 0U;
            for (std::uint32_t bit = 0U; bit < kGpuHuffmanLookupBits && bit_pos + bit < limit_bits; ++bit) {
                const auto absolute = bit_pos + bit;
                code |= ((static_cast<std::uint8_t>(encoded[absolute >> 3U]) >> (absolute & 7U)) & 1U) << bit;
            }
            const auto entry_offset = code * sizeof(std::uint16_t);
            const auto entry =
                static_cast<std::uint8_t>(payload[entry_offset]) |
                (static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[entry_offset + 1U])) << 8U);
            const auto width = entry >> 8U;
            if (width == 0U || width > kGpuHuffmanLookupBits || width > limit_bits - bit_pos) {
                throw ArchiveError("GPU Huffman code is invalid or truncated");
            }
            output[decoded_offset + i] = static_cast<std::byte>(entry & 0xFFU);
            bit_pos += width;
        }
        decoded_offset += decoded_len;
        previous = next;
    }
    if (previous != bitstream.size()) {
        throw ArchiveError("GPU Huffman payload has trailing bytes");
    }
}

// Purpose: Decode independent bounded LZ4 segments from a version-four native block.
// Inputs: `payload` starts with cumulative 32-bit segment offsets and `output` is the exact decoded block span.
// Outputs: Writes every decoded byte or throws for invalid tables, segment extents, or LZ4 data.
void materialize_dictionary_cpu(std::span<const std::byte> payload, std::span<std::byte> output) {
    for (const auto& segment : parse_dictionary_segments(payload, static_cast<std::uint32_t>(output.size()))) {
        const auto actual =
            LZ4_decompress_safe(reinterpret_cast<const char*>(payload.data() + segment.encoded_offset),
                                reinterpret_cast<char*>(output.data() + segment.decoded_offset),
                                static_cast<int>(segment.encoded_size), static_cast<int>(segment.decoded_size));
        if (actual != static_cast<int>(segment.decoded_size)) {
            throw ArchiveError("GPU dictionary block failed to decompress");
        }
    }
}

// Purpose: Expand one admitted version-five motif and its sorted corrections in bounded host memory.
// Inputs: `payload` is an untrusted sparse block, `output` is exact, and `max_period` is versioned.
// Outputs: Writes all decoded bytes or throws `ArchiveError` before writing when metadata is invalid.
void materialize_sparse_pattern_cpu(std::span<const std::byte> payload, std::span<std::byte> output,
                                    std::uint32_t max_period) {
    const auto layout = parse_sparse_pattern_block(payload, static_cast<std::uint32_t>(output.size()), max_period);
    materialize_pattern_cpu(layout.motif, output, max_period);
    for (std::size_t index = 0U; index < layout.patch_count; ++index) {
        const auto offset = index * kSparsePatternPatchBytes;
        output[read_sparse_u32(layout.patches, offset)] = layout.patches[offset + sizeof(std::uint32_t)];
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
            if ((block.kind == BlockKind::Deflate || block.kind == BlockKind::CpuZstd) && encoded_len >= len) {
                throw ArchiveError("CPU-compressed block length is invalid");
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
            if (block.kind == BlockKind::GpuHuffman) {
                const auto segment_count = (len + kGpuPrefixSegmentBytes - 1U) / kGpuPrefixSegmentBytes;
                const auto header_bytes = kGpuHuffmanLookupBytes + ((segment_count + 1U) * sizeof(std::uint32_t));
                if (encoded_len <= header_bytes || encoded_len >= len) {
                    throw ArchiveError("GPU Huffman block metadata is invalid");
                }
            }
            if (block.kind == BlockKind::GpuDictionary) {
                const auto segment_count = (len + kGpuDictionarySegmentBytes - 1U) / kGpuDictionarySegmentBytes;
                const auto table_bytes = (segment_count + 1U) * sizeof(std::uint32_t);
                if (segment_count == 0U || encoded_len <= table_bytes || encoded_len >= len) {
                    throw ArchiveError("GPU dictionary block metadata is invalid");
                }
            }
            if (is_gpu_sparse_pattern_kind(block.kind)) {
                (void)parse_sparse_pattern_block(payload.subspan(static_cast<std::size_t>(block.encoded_offset),
                                                                 static_cast<std::size_t>(block.encoded_len)),
                                                 static_cast<std::uint32_t>(len),
                                                 block.kind == BlockKind::GpuLongSparsePattern
                                                     ? kMaxGpuLongSparsePatternBytes
                                                     : kMaxGpuPatternBytes);
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
            } else if (block.kind == BlockKind::CpuZstd) {
                materialize_zstd_block_cpu(payload.subspan(static_cast<std::size_t>(block.encoded_offset),
                                                           static_cast<std::size_t>(block.encoded_len)),
                                           output.subspan(out_pos, len));
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
            } else if (block.kind == BlockKind::GpuHuffman) {
                materialize_huffman_cpu(payload.subspan(static_cast<std::size_t>(block.encoded_offset),
                                                        static_cast<std::size_t>(block.encoded_len)),
                                        output.subspan(out_pos, len));
            } else if (block.kind == BlockKind::GpuDictionary) {
                materialize_dictionary_cpu(payload.subspan(static_cast<std::size_t>(block.encoded_offset),
                                                           static_cast<std::size_t>(block.encoded_len)),
                                           output.subspan(out_pos, len));
            } else if (is_gpu_sparse_pattern_kind(block.kind)) {
                materialize_sparse_pattern_cpu(payload.subspan(static_cast<std::size_t>(block.encoded_offset),
                                                               static_cast<std::size_t>(block.encoded_len)),
                                               output.subspan(out_pos, len),
                                               block.kind == BlockKind::GpuLongSparsePattern
                                                   ? kMaxGpuLongSparsePatternBytes
                                                   : kMaxGpuPatternBytes);
            } else {
                throw ArchiveError("unknown block kind");
            }
        }
    });
}

}  // namespace

// Purpose: Use one overflow-safe complete-block grouping policy for decode execution and concurrency admission.
// Inputs: Borrowed blocks, a starting index through size, and a positive decoded-byte window limit.
// Outputs: Returns exact extent and bytes, or throws ArchiveError before invalid metadata can enter a worker.
DecodeBlockWindow resolve_decode_block_window(std::span<const BlockDescriptor> blocks, std::size_t first,
                                              std::uint64_t window_bytes) {
    if (window_bytes == 0 || first > blocks.size()) {
        throw ArchiveError("invalid decode block window bounds");
    }
    DecodeBlockWindow window{.end = first};
    while (window.end < blocks.size()) {
        const auto length = blocks[window.end].uncompressed_len;
        if (length == 0 || length > window_bytes) {
            throw ArchiveError("archive block is outside decode window bounds");
        }
        if (length > window_bytes - window.uncompressed_size) {
            break;
        }
        window.uncompressed_size += length;
        ++window.end;
        if (window.uncompressed_size == window_bytes) {
            break;
        }
    }
    return window;
}

// Purpose: Count the exact windows that the streaming decoder will submit.
// Inputs: Borrowed block metadata and the same positive window byte limit used during execution.
// Outputs: Returns zero for empty entries; throws ArchiveError for invalid block/window bounds.
std::uint64_t count_decode_block_windows(std::span<const BlockDescriptor> blocks, std::uint64_t window_bytes) {
    auto window = resolve_decode_block_window(blocks, 0, window_bytes);
    std::uint64_t count = 0;
    for (std::size_t first = 0; first < blocks.size();) {
        ++count;
        first = window.end;
        window = resolve_decode_block_window(blocks, first, window_bytes);
    }
    return count;
}

// Purpose: Report whether a SUZIP block kind reserves bytes in the encoded payload window.
// Inputs: `kind` is a native archive block encoding tag.
// Outputs: Returns true for raw, CPU-compressed, and GPU-compressed payload blocks.
bool block_kind_has_payload(BlockKind kind) {
    return kind == BlockKind::Raw || kind == BlockKind::Deflate || kind == BlockKind::CpuZstd ||
           kind == BlockKind::Pattern || kind == BlockKind::GpuPrefix || kind == BlockKind::GpuAdaptivePrefix ||
           kind == BlockKind::GpuHuffman || kind == BlockKind::GpuDictionary || is_gpu_sparse_pattern_kind(kind);
}

// Purpose: Encode a contiguous native CPU block range with one bounded worker-owned Zstandard context.
// Inputs: Source bytes, fixed block size/level, disjoint result slots, and the selected index range.
// Outputs: Fills each slot with a complete descriptor and optional compressed payload, or throws on codec failure.
static void encode_cpu_block_range(std::span<const std::byte> input, std::uint32_t block_size, int compression_level,
                                   std::span<EncodedBlockWork> block_work, std::size_t begin, std::size_t end) {
    const auto release_context = [](ZstdCompressionContext* context) {
        zstd_runtime().free_compression_context(context);
    };
    std::unique_ptr<ZstdCompressionContext, decltype(release_context)> context(nullptr, release_context);
    std::vector<std::byte> trial;
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
            continue;
        }
        const bool use_zstd = block.size() >= kMinArchiveBlockBytes;
        if (use_zstd && !context) {
            context.reset(zstd_runtime().create_compression_context());
            if (!context) {
                throw ArchiveError("Zstandard native compression context allocation failed");
            }
        }
        auto compressed = use_zstd ? try_zstd_block(block, compression_level, context.get(), trial)
                                   : try_deflate_block(block, compression_level);
        if (!compressed.empty()) {
            block_work[i].descriptor = BlockDescriptor{
                .kind = use_zstd ? BlockKind::CpuZstd : BlockKind::Deflate,
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

// Purpose: Compress one bounded native chunk into independently decodable CPU block descriptors and payload.
// Inputs: Source bytes and validated block size, worker count, and effort options.
// Outputs: Returns a dense encoded chunk or throws before exposing incomplete codec output.
EncodedChunk encode_chunk_cpu(std::span<const std::byte> input, const ArchiveCodecOptions& options) {
    validate_encode_options(options);
    reject_oversized_codec_span(input.size(), "codec input");

    EncodedChunk out;
    const auto block_size = std::max<std::uint32_t>(1U, options.block_size);
    const auto block_count = (input.size() + block_size - 1U) / block_size;
    std::vector<EncodedBlockWork> block_work(block_count);

    // Bound concurrently retained Zstandard workspaces on smaller hosts.
    constexpr std::uint32_t kMaxConcurrentZstdBlocks = 4U;
    run_parallel_ranges(
        block_count, std::min(options.worker_count, kMaxConcurrentZstdBlocks), [&](std::size_t begin, std::size_t end) {
            encode_cpu_block_range(input, block_size, options.compression_level, block_work, begin, end);
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
            if (descriptor.kind == BlockKind::Deflate || descriptor.kind == BlockKind::CpuZstd) {
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

// Purpose: Report whether a decoded SUZIP block table needs CPU-only codec handling.
// Inputs: `blocks` is a validated or soon-to-be-validated block descriptor table.
// Outputs: Returns true when any block uses Deflate or Zstandard.
bool block_table_contains_cpu_only(std::span<const BlockDescriptor> blocks) {
    return std::ranges::any_of(blocks, [](const BlockDescriptor& block) {
        return block.kind == BlockKind::Deflate || block.kind == BlockKind::CpuZstd;
    });
}

// Purpose: Decode only CPU-compressed blocks in a SUZIP chunk into an existing output buffer.
// Inputs: `payload`, `blocks`, `output`, and `options` describe one archive chunk and CPU decode settings.
// Outputs: Mutates `output` for Deflate/Zstandard blocks; throws on malformed metadata or codec failures.
void decode_cpu_only_blocks_cpu(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
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
            } else if (block.kind == BlockKind::CpuZstd) {
                materialize_zstd_block_cpu(
                    payload.subspan(static_cast<std::size_t>(block.encoded_offset),
                                    static_cast<std::size_t>(block.encoded_len)),
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
