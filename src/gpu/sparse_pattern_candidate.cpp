#include "gpu/sparse_pattern_candidate.hpp"

#include "core/sparse_pattern_block.hpp"
#include "gpu/gpu_codec.hpp"
#include "gpu/sparse_pattern_device.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace superzip::sparse_pattern {
namespace {

// Purpose: Find one likely repeated motif using bounded exact anchors and a sparse host sample.
// Inputs: `block` is an immutable archive block whose sampled fill/exact-pattern path did not win.
// Outputs: Returns the lowest estimated-cost 2..16 KiB period, or zero without a low-mismatch anchor.
std::uint32_t sampled_sparse_period(std::span<const std::byte> block) {
    constexpr std::size_t kAnchorBytes = 16U;
    constexpr std::size_t kMaxAnchorTrials = 32U;
    constexpr std::size_t kSampleBytes = 64U * 1024U;
    constexpr std::size_t kSampleStride = 13U;
    if (block.size() < 4096U) {
        return 0U;
    }
    const auto max_period = std::min<std::size_t>(kMaxGpuPatternBytes, block.size() / 2U);
    const auto sample_end = std::min<std::size_t>(block.size(), kSampleBytes);
    auto search = block.begin() + 2U;
    const auto search_end = block.begin() + static_cast<std::ptrdiff_t>(max_period + kAnchorBytes);
    std::uint32_t best_period = 0U;
    std::size_t best_estimated_bytes = std::numeric_limits<std::size_t>::max();
    for (std::size_t trial = 0U; trial < kMaxAnchorTrials; ++trial) {
        const auto found = std::search(search, search_end, block.begin(), block.begin() + kAnchorBytes);
        if (found == search_end) {
            break;
        }
        const auto period = static_cast<std::size_t>(found - block.begin());
        std::size_t mismatches = 0U;
        std::size_t samples = 0U;
        for (std::size_t position = period; position < sample_end; position += kSampleStride) {
            mismatches += block[position] != block[position % period];
            ++samples;
        }
        if (samples != 0U && mismatches * 3U <= samples) {
            const auto estimated_bytes =
                kSparsePatternHeaderBytes + period +
                (mismatches * block.size() * kSparsePatternPatchBytes + samples / 2U) / samples;
            if (estimated_bytes < best_estimated_bytes) {
                best_estimated_bytes = estimated_bytes;
                best_period = static_cast<std::uint32_t>(period);
            }
        }
        search = found + 1U;
    }
    if (best_period != 0U) {
        constexpr std::size_t kDistributedSamples = 256U;
        std::size_t mismatches = 0U;
        const auto remaining = block.size() - best_period;
        for (std::size_t sample = 0U; sample < kDistributedSamples; ++sample) {
            const auto position = best_period + sample * (remaining - 1U) / (kDistributedSamples - 1U);
            mismatches += block[position] != block[position % best_period];
        }
        if (mismatches * 3U > kDistributedSamples) {
            return 0U;
        }
    }
    return best_period;
}

// Purpose: Append one 32-bit field in the version-five sparse wire order.
// Inputs: `payload` is the growing block and `value` is a bounded unsigned field.
// Outputs: Appends exactly four little-endian bytes.
void append_sparse_u32(std::vector<std::byte>& payload, std::uint32_t value) {
    for (std::uint32_t byte = 0U; byte < sizeof(value); ++byte) {
        payload.push_back(static_cast<std::byte>((value >> (byte * 8U)) & 0xFFU));
    }
}

// Purpose: Convert one unordered HIP mismatch list into a canonical payload only when it beats baseline.
// Inputs: `block` is the original source, `period` is sampled, and `positions` are device-collected mismatches.
// Outputs: Returns a validated sorted sparse block, or empty when no smaller valid candidate exists.
std::vector<std::byte> frame_sparse_candidate(std::span<const std::byte> block, std::uint32_t period,
                                              std::vector<std::uint32_t> positions, std::uint32_t baseline_bytes) {
    if (positions.empty()) {
        return {};
    }
    std::sort(positions.begin(), positions.end());
    const auto encoded_bytes = kSparsePatternHeaderBytes + period + positions.size() * kSparsePatternPatchBytes;
    if (encoded_bytes >= baseline_bytes || encoded_bytes >= block.size()) {
        return {};
    }
    std::vector<std::byte> payload;
    payload.reserve(encoded_bytes);
    append_sparse_u32(payload, period);
    append_sparse_u32(payload, static_cast<std::uint32_t>(positions.size()));
    payload.insert(payload.end(), block.begin(), block.begin() + period);
    std::uint32_t previous = period - 1U;
    for (const auto position : positions) {
        if (position <= previous || position >= block.size() || block[position] == block[position % period]) {
            throw GpuError("HIP sparse mismatch list disagrees with source bytes");
        }
        append_sparse_u32(payload, position);
        payload.push_back(block[position]);
        previous = position;
    }
    (void)parse_sparse_pattern_block(payload, static_cast<std::uint32_t>(block.size()));
    return payload;
}

}  // namespace

// Purpose: Evaluate independent sparse candidates against already selected GPU-native blocks.
// Inputs: Host/device mirrors are live; block descriptors cover the chunk exactly; telemetry is optional.
// Outputs: Returns only smaller complete payloads, with all speculative HIP work accounted for.
SparseReplacements select_replacements(std::span<const std::byte> input, const std::byte* device_input,
                                       std::span<const BlockDescriptor> blocks, GpuTelemetry* telemetry) {
    if (device_input == nullptr) {
        throw GpuError("sparse candidate device input is unavailable");
    }
    SparseReplacements replacements(blocks.size());
    std::vector<SparseCandidate> candidates;
    std::vector<std::size_t> candidate_block_indices;
    std::size_t source_offset = 0U;
    for (std::size_t index = 0U; index < blocks.size(); ++index) {
        const auto length = static_cast<std::size_t>(blocks[index].uncompressed_len);
        if (source_offset > input.size() || length > input.size() - source_offset) {
            throw GpuError("sparse candidate block exceeds source chunk");
        }
        const auto block = input.subspan(source_offset, length);
        const auto block_offset = source_offset;
        source_offset += length;
        if (blocks[index].kind == BlockKind::Fill || blocks[index].kind == BlockKind::Pattern ||
            blocks[index].kind == BlockKind::GpuSparsePattern) {
            continue;
        }
        const auto period = sampled_sparse_period(block);
        const auto baseline_bytes = std::min<std::size_t>(blocks[index].encoded_len, block.size());
        if (period == 0U || baseline_bytes <= kSparsePatternHeaderBytes + period + kSparsePatternPatchBytes) {
            continue;
        }
        const auto max_patches =
            std::min<std::size_t>((baseline_bytes - kSparsePatternHeaderBytes - period - 1U) / kSparsePatternPatchBytes,
                                  block.size() - period);
        if (max_patches == 0U) {
            continue;
        }
        candidates.push_back(SparseCandidate{
            .source_offset = static_cast<std::uint32_t>(block_offset),
            .input_bytes = static_cast<std::uint32_t>(length),
            .period = period,
            .max_patches = static_cast<std::uint32_t>(max_patches),
        });
        candidate_block_indices.push_back(index);
    }
    if (source_offset != input.size()) {
        throw GpuError("sparse candidate blocks do not cover source chunk");
    }
    if (!candidates.empty()) {
        auto positions = collect_positions_device_batch(device_input, static_cast<std::uint32_t>(input.size()),
                                                        candidates, telemetry);
        for (std::size_t candidate_index = 0U; candidate_index < candidates.size(); ++candidate_index) {
            const auto block_index = candidate_block_indices[candidate_index];
            const auto& candidate = candidates[candidate_index];
            replacements[block_index] =
                frame_sparse_candidate(input.subspan(candidate.source_offset, candidate.input_bytes), candidate.period,
                                       std::move(positions[candidate_index]), blocks[block_index].encoded_len);
        }
    }
    return replacements;
}

// Purpose: Replace only complete GPU sparse blocks whose bytes beat the baseline block.
// Inputs: A dense encoded chunk and one optional canonical payload per source block.
// Outputs: Returns dense mixed payloads while retaining CRC and GPU ownership metadata.
EncodedChunk apply_replacements(EncodedChunk baseline, const SparseReplacements& replacements) {
    if (replacements.size() != baseline.blocks.size()) {
        throw GpuError("sparse replacement count differs from baseline blocks");
    }
    EncodedChunk result;
    result.source_crc32 = baseline.source_crc32;
    result.source_crc32_available = baseline.source_crc32_available;
    result.gpu_used = baseline.gpu_used;
    result.blocks.reserve(baseline.blocks.size());
    result.payload.reserve(baseline.payload.size());
    for (std::size_t index = 0U; index < baseline.blocks.size(); ++index) {
        auto descriptor = baseline.blocks[index];
        const auto& replacement = replacements[index];
        const auto offset = static_cast<std::size_t>(descriptor.encoded_offset);
        const auto length = static_cast<std::size_t>(descriptor.encoded_len);
        if (offset > baseline.payload.size() || length > baseline.payload.size() - offset) {
            throw GpuError("sparse baseline block exceeds encoded payload");
        }
        descriptor.encoded_offset = result.payload.size();
        if (!replacement.empty() && replacement.size() < descriptor.encoded_len) {
            if (replacement.size() >= descriptor.uncompressed_len) {
                throw GpuError("sparse replacement does not improve the source block");
            }
            descriptor.kind = BlockKind::GpuSparsePattern;
            descriptor.encoded_len = static_cast<std::uint32_t>(replacement.size());
            result.payload.insert(result.payload.end(), replacement.begin(), replacement.end());
        } else {
            result.payload.insert(result.payload.end(), baseline.payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                  baseline.payload.begin() + static_cast<std::ptrdiff_t>(offset + length));
        }
        result.blocks.push_back(descriptor);
    }
    return result;
}

}  // namespace superzip::sparse_pattern
