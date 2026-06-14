#include "core/archive_blocks.hpp"

#include "core/result.hpp"

#include "miniz.h"

#include <algorithm>
#include <array>
#include <cstring>
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
    if (options.compression_level < 1 || options.compression_level > 9) {
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
// Inputs: `count` is the number of work items, `worker_count` is the requested worker budget, and `fn` processes `[begin,end)`.
// Outputs: Blocks until all ranges finish; propagates worker exceptions.
template <typename Fn>
void run_parallel_ranges(std::size_t count, std::uint32_t worker_count, Fn fn) {
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
        futures.push_back(std::async(std::launch::async, [begin, end, &fn] {
            fn(begin, end);
        }));
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
    return std::ranges::all_of(bytes, [&](std::byte byte) {
        return static_cast<std::uint8_t>(byte) == value;
    });
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
    const auto status = compress2(
        reinterpret_cast<unsigned char*>(compressed.data()),
        &bound,
        reinterpret_cast<const unsigned char*>(block.data()),
        static_cast<mz_ulong>(block.size()),
        compression_level);
    if (status != MZ_OK || bound >= block.size()) {
        return {};
    }
    compressed.resize(static_cast<std::size_t>(bound));
    return compressed;
}

// Purpose: Inflate one miniz deflate payload into the caller-owned output span.
// Inputs: `payload`, `encoded_offset`, `encoded_len`, and `output` describe one block.
// Outputs: Writes exactly `output.size()` bytes or throws `ArchiveError`.
void inflate_deflate_block(
    std::span<const std::byte> payload,
    std::uint64_t encoded_offset,
    std::uint32_t encoded_len,
    std::span<std::byte> output) {
    mz_ulong output_len = static_cast<mz_ulong>(output.size());
    const auto status = uncompress(
        reinterpret_cast<unsigned char*>(output.data()),
        &output_len,
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

// Purpose: Compute output offsets and validate block spans before parallel decode.
// Inputs: `blocks`, `payload`, and `output` are caller-provided decode buffers.
// Outputs: Returns one output offset per block; throws on invalid bounds.
std::vector<std::size_t> validate_decode_blocks(
    std::span<const std::byte> payload,
    std::span<const BlockDescriptor> blocks,
    std::span<std::byte> output) {
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
void materialize_blocks_cpu(
    std::span<const std::byte> payload,
    std::span<const BlockDescriptor> blocks,
    std::span<const std::size_t> offsets,
    std::span<std::byte> output,
    std::uint32_t worker_count) {
    run_parallel_ranges(blocks.size(), worker_count, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const auto& block = blocks[i];
            const auto len = static_cast<std::size_t>(block.uncompressed_len);
            const auto out_pos = offsets[i];
            if (block.kind == BlockKind::Fill) {
                std::ranges::fill(output.subspan(out_pos, len), static_cast<std::byte>(block.fill_value));
            } else if (block.kind == BlockKind::Raw) {
                std::memcpy(
                    output.data() + out_pos,
                    payload.data() + static_cast<std::size_t>(block.encoded_offset),
                    len);
            } else if (block.kind == BlockKind::Deflate) {
                inflate_deflate_block(
                    payload,
                    block.encoded_offset,
                    block.encoded_len,
                    output.subspan(out_pos, len));
            } else if (block.kind == BlockKind::Pattern) {
                materialize_pattern_cpu(
                    payload.subspan(
                        static_cast<std::size_t>(block.encoded_offset),
                        static_cast<std::size_t>(block.encoded_len)),
                    output.subspan(out_pos, len));
            } else {
                throw ArchiveError("unknown block kind");
            }
        }
    });
}

}  // namespace

bool block_kind_has_payload(BlockKind kind) {
    return kind == BlockKind::Raw || kind == BlockKind::Deflate || kind == BlockKind::Pattern;
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
                std::memcpy(out.payload.data() + offset, input.data() + offset, len);
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
                std::memcpy(
                    out.payload.data() + static_cast<std::size_t>(descriptor.encoded_offset),
                    block_work[i].payload.data(),
                    block_work[i].payload.size());
                continue;
            }
            const auto source_offset = i * static_cast<std::size_t>(block_size);
            const auto len = static_cast<std::size_t>(descriptor.uncompressed_len);
            std::memcpy(
                out.payload.data() + static_cast<std::size_t>(descriptor.encoded_offset),
                input.data() + source_offset,
                len);
        }
    });
    return out;
}

EncodedChunk deflate_classified_raw_blocks_cpu(
    std::span<const std::byte> input,
    EncodedChunk classified,
    const ArchiveCodecOptions& options) {
    validate_encode_options(options);
    reject_oversized_codec_span(input.size(), "codec input");

    const auto block_size = std::max<std::uint32_t>(1U, options.block_size);
    std::vector<EncodedBlockWork> block_work(classified.blocks.size());
    run_parallel_ranges(classified.blocks.size(), options.worker_count, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            block_work[i].descriptor = classified.blocks[i];
            if (classified.blocks[i].kind != BlockKind::Raw) {
                continue;
            }
            const auto source_offset = i * static_cast<std::size_t>(block_size);
            const auto len = static_cast<std::size_t>(classified.blocks[i].uncompressed_len);
            auto compressed = try_deflate_block(input.subspan(source_offset, len), options.compression_level);
            if (!compressed.empty()) {
                block_work[i].descriptor.kind = BlockKind::Deflate;
                block_work[i].descriptor.encoded_len = static_cast<std::uint32_t>(compressed.size());
                block_work[i].payload = std::move(compressed);
            }
        }
    });

    EncodedChunk out;
    out.gpu_used = classified.gpu_used;
    out.source_crc32 = classified.source_crc32;
    out.source_crc32_available = classified.source_crc32_available;
    out.blocks.resize(classified.blocks.size());
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
        out.payload = std::move(classified.payload);
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
                std::memcpy(
                    out.payload.data() + static_cast<std::size_t>(descriptor.encoded_offset),
                    block_work[i].payload.data(),
                    block_work[i].payload.size());
                continue;
            }
            if (descriptor.kind == BlockKind::Pattern) {
                const auto& source_descriptor = classified.blocks[i];
                std::memcpy(
                    out.payload.data() + static_cast<std::size_t>(descriptor.encoded_offset),
                    classified.payload.data() + static_cast<std::size_t>(source_descriptor.encoded_offset),
                    static_cast<std::size_t>(source_descriptor.encoded_len));
                continue;
            }
            const auto source_offset = i * static_cast<std::size_t>(block_size);
            const auto len = static_cast<std::size_t>(descriptor.uncompressed_len);
            std::memcpy(
                out.payload.data() + static_cast<std::size_t>(descriptor.encoded_offset),
                input.data() + source_offset,
                len);
        }
    });
    return out;
}

bool block_table_contains_deflate(std::span<const BlockDescriptor> blocks) {
    return std::ranges::any_of(blocks, [](const BlockDescriptor& block) {
        return block.kind == BlockKind::Deflate;
    });
}

void decode_deflate_blocks_cpu(
    std::span<const std::byte> payload,
    std::span<const BlockDescriptor> blocks,
    std::span<std::byte> output,
    const ArchiveCodecOptions& options) {
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
                inflate_deflate_block(
                    payload,
                    block.encoded_offset,
                    block.encoded_len,
                    output.subspan(offsets[i], static_cast<std::size_t>(block.uncompressed_len)));
            }
        }
    });
}

void decode_chunk_cpu(
    std::span<const std::byte> payload,
    std::span<const BlockDescriptor> blocks,
    std::span<std::byte> output,
    const ArchiveCodecOptions& options) {
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
