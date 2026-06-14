#include "gpu/gpu_codec.hpp"

#include "core/result.hpp"

#include <algorithm>
#include <cstring>

namespace superzip {

namespace {

// Purpose: Detect whether a block is a repeated byte value.
// Inputs: `bytes` is the block to inspect and `value` receives the repeated byte when true.
// Outputs: Returns true for empty or uniform blocks; does not allocate.
bool block_is_fill(std::span<const std::byte> bytes, std::uint8_t& value) {
    if (bytes.empty()) {
        value = 0;
        return true;
    }
    value = static_cast<std::uint8_t>(bytes.front());
    return std::ranges::all_of(bytes, [&](std::byte byte) {
        return static_cast<std::uint8_t>(byte) == value;
    });
}

// Purpose: Encode a chunk with the bounded CPU fallback codec.
// Inputs: `input` is uncompressed data and `options.block_size` controls block partitioning.
// Outputs: Returns fill/raw descriptors and raw payload bytes.
EncodedChunk encode_chunk_cpu(std::span<const std::byte> input, const GpuCodecOptions& options) {
    EncodedChunk out;
    const auto block_size = std::max<std::uint32_t>(1, options.block_size);
    std::uint64_t encoded_offset = 0;
    for (std::size_t pos = 0; pos < input.size(); pos += block_size) {
        const auto len = static_cast<std::uint32_t>(std::min<std::size_t>(block_size, input.size() - pos));
        const auto block = input.subspan(pos, len);
        std::uint8_t fill = 0;
        if (block_is_fill(block, fill)) {
            out.blocks.push_back(BlockDescriptor{
                .kind = BlockKind::Fill,
                .fill_value = fill,
                .uncompressed_len = len,
                .encoded_offset = encoded_offset,
                .encoded_len = 0,
            });
            continue;
        }
        out.blocks.push_back(BlockDescriptor{
            .kind = BlockKind::Raw,
            .fill_value = 0,
            .uncompressed_len = len,
            .encoded_offset = encoded_offset,
            .encoded_len = len,
        });
        out.payload.insert(out.payload.end(), block.begin(), block.end());
        encoded_offset += len;
    }
    return out;
}

// Purpose: Decode a chunk with the bounded CPU fallback codec.
// Inputs: `payload` and `blocks` are validated archive data, and `output` is the exact destination buffer.
// Outputs: Writes decoded bytes into `output`; throws `ArchiveError` on invalid bounds or decoded size.
void decode_chunk_cpu(
    std::span<const std::byte> payload,
    std::span<const BlockDescriptor> blocks,
    std::span<std::byte> output) {
    std::size_t out_pos = 0;
    for (const auto& block : blocks) {
        const auto len = static_cast<std::size_t>(block.uncompressed_len);
        if (out_pos + len > output.size()) {
            throw ArchiveError("decoded block exceeds output buffer");
        }
        if (block.kind == BlockKind::Fill) {
            std::ranges::fill(output.subspan(out_pos, len), static_cast<std::byte>(block.fill_value));
        } else if (block.kind == BlockKind::Raw) {
            const auto offset = static_cast<std::size_t>(block.encoded_offset);
            const auto encoded_len = static_cast<std::size_t>(block.encoded_len);
            if (encoded_len != len || offset + encoded_len > payload.size()) {
                throw ArchiveError("raw block exceeds encoded payload");
            }
            std::memcpy(output.data() + out_pos, payload.data() + offset, len);
        } else {
            throw ArchiveError("unknown block kind");
        }
        out_pos += len;
    }
    if (out_pos != output.size()) {
        throw ArchiveError("decoded size does not match expected output size");
    }
}

}  // namespace

GpuInfo query_gpu_info_cpu_only();
EncodedChunk encode_chunk_hip(std::span<const std::byte> input, const GpuCodecOptions& options);
void decode_chunk_hip(
    std::span<const std::byte> payload,
    std::span<const BlockDescriptor> blocks,
    std::span<std::byte> output,
    const GpuCodecOptions& options);

GpuInfo query_gpu_info() {
#if SUPERZIP_ENABLE_HIP
    return query_gpu_info_cpu_only();
#else
    return GpuInfo{
        .hip_compiled = false,
        .available = false,
        .status = "Built without HIP acceleration",
    };
#endif
}

EncodedChunk encode_chunk(std::span<const std::byte> input, const GpuCodecOptions& options) {
#if SUPERZIP_ENABLE_HIP
    try {
        auto encoded = encode_chunk_hip(input, options);
        encoded.gpu_used = true;
        return encoded;
    } catch (const GpuError&) {
        if (options.require_gpu) {
            throw;
        }
    }
#else
    if (options.require_gpu) {
        throw GpuError("SuperZip was built without HIP acceleration");
    }
#endif
    return encode_chunk_cpu(input, options);
}

bool decode_chunk(
    std::span<const std::byte> payload,
    std::span<const BlockDescriptor> blocks,
    std::span<std::byte> output,
    const GpuCodecOptions& options) {
#if SUPERZIP_ENABLE_HIP
    try {
        decode_chunk_hip(payload, blocks, output, options);
        return true;
    } catch (const GpuError&) {
        if (options.require_gpu) {
            throw;
        }
    }
#else
    if (options.require_gpu) {
        throw GpuError("SuperZip was built without HIP acceleration");
    }
#endif
    decode_chunk_cpu(payload, blocks, output);
    return false;
}

}  // namespace superzip
