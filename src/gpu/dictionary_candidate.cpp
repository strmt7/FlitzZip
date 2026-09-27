#include "gpu/dictionary_candidate.hpp"

#include "core/result.hpp"
#include "gpu/dictionary_device.hpp"
#include "gpu/gpu_codec.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace superzip::dictionary {
namespace {

// Purpose: Avoid expensive dictionary trials on blocks without sampled repeated substrings.
// Inputs: One bounded source block sampled evenly at no more than 4,096 twelve-byte positions.
// Outputs: Returns true after eight repeats reachable within independent 64 KiB dictionary segments.
bool has_dictionary_sample_repeats(std::span<const std::byte> input) {
    constexpr std::size_t kMaxSamples = 4096U;
    constexpr std::size_t kRequiredRepeats = 8U;
    constexpr std::size_t kSampleBytes = 12U;
    const auto stride = std::max<std::size_t>(64U, (input.size() + kMaxSamples - 1U) / kMaxSamples);
    std::unordered_set<std::string_view> seen;
    seen.reserve(std::min<std::size_t>(kMaxSamples, input.size() / stride + 1U));
    std::size_t repeats = 0U;
    std::size_t previous_segment = 0U;
    for (std::size_t offset = 0U; offset + kSampleBytes <= input.size(); offset += stride) {
        const auto segment = offset / kSegmentBytes;
        if (segment != previous_segment) {
            seen.clear();
            previous_segment = segment;
        }
        if (offset + kSampleBytes > (segment + 1U) * kSegmentBytes) {
            continue;
        }
        const auto key = std::string_view(reinterpret_cast<const char*>(input.data() + offset), kSampleBytes);
        if (!seen.insert(key).second && ++repeats >= kRequiredRepeats) {
            return true;
        }
    }
    return false;
}

// Purpose: Propose a repeated distance only when distributed samples support a segment-local match.
// Inputs: One independent dictionary segment; proposed distances are powers of two from 256 to 32 KiB.
// Outputs: Returns a sampled distance or zero; HIP must verify every reference before encoding.
std::uint16_t sampled_periodic_distance(std::span<const std::byte> segment) {
    constexpr std::array<std::uint16_t, 8> kDistances{256U, 512U, 1024U, 2048U, 4096U, 8192U, 16384U, 32768U};
    constexpr std::size_t kAnchorBytes = 16U;
    constexpr std::size_t kSamples = 128U;
    for (const auto distance : kDistances) {
        if (segment.size() < static_cast<std::size_t>(distance) * 2U ||
            !std::equal(segment.begin(), segment.begin() + kAnchorBytes, segment.begin() + distance)) {
            continue;
        }
        const auto remaining = segment.size() - distance;
        std::size_t matches = 0U;
        for (std::size_t sample = 0U; sample < kSamples; ++sample) {
            const auto offset = static_cast<std::size_t>(distance) + sample * (remaining - 1U) / (kSamples - 1U);
            matches += segment[offset] == segment[offset - distance];
        }
        if (matches * 16U >= kSamples * 15U) {
            return distance;
        }
    }
    return 0U;
}

// Purpose: Admit a periodic-index trial only when every independent segment has a supported distance.
// Inputs: A bounded dictionary batch; the final segment may be short.
// Outputs: Returns one distance per segment, or empty to select the ordinary exact-prefix index.
std::vector<std::uint16_t> sampled_batch_distances(std::span<const std::byte> input) {
    std::vector<std::uint16_t> distances;
    distances.reserve((input.size() + kSegmentBytes - 1U) / kSegmentBytes);
    for (std::size_t offset = 0U; offset < input.size(); offset += kSegmentBytes) {
        const auto segment = input.subspan(offset, std::min<std::size_t>(kSegmentBytes, input.size() - offset));
        const auto distance = sampled_periodic_distance(segment);
        if (distance == 0U) {
            return {};
        }
        distances.push_back(distance);
    }
    return distances;
}

// Purpose: Screen baseline blocks before allocating a dictionary search workspace.
// Inputs: Source bytes, their current GPU-native descriptor, and requested effort level.
// Outputs: Returns true when a dictionary candidate can plausibly improve encoded size.
bool should_try_dictionary(std::span<const std::byte> input, const BlockDescriptor& baseline, int level) {
    if (input.size() < 4096U || baseline.encoded_len == 0U ||
        (baseline.kind != BlockKind::Raw && baseline.kind != BlockKind::GpuPrefix &&
         baseline.kind != BlockKind::GpuAdaptivePrefix && baseline.kind != BlockKind::GpuSparsePattern)) {
        return false;
    }
    if (level < 7 && baseline.encoded_len <= input.size() / 2U) {
        return false;
    }
    return has_dictionary_sample_repeats(input);
}

// Purpose: Append a bounded little-endian segment offset to a native dictionary block payload.
// Inputs: `payload` is the growing header and `offset` is the cumulative encoded segment byte count.
// Outputs: Appends exactly four bytes.
void append_segment_offset(std::vector<std::byte>& payload, std::uint32_t offset) {
    for (std::uint32_t byte = 0U; byte < sizeof(std::uint32_t); ++byte) {
        payload.push_back(static_cast<std::byte>((offset >> (byte * 8U)) & 0xFFU));
    }
}

// Purpose: Account for the measured work of one dictionary batch using an existing device input buffer.
// Inputs: Validated batch metadata, borrowed input bytes, and optional operation telemetry.
// Outputs: Adds metadata transfers, allocated workspace, explicit launches, and HIP device time.
void record_dictionary_batch(const EncodedBatch& encoded, std::size_t input_bytes, GpuTelemetry* telemetry) {
    const auto period_bytes = ((input_bytes + kSegmentBytes - 1U) / kSegmentBytes) * sizeof(std::uint16_t);
    if ((encoded.h2d_bytes != 0U && encoded.h2d_bytes != period_bytes) ||
        encoded.explicit_kernel_launches != (encoded.h2d_bytes == 0U ? 4U : 2U) ||
        encoded.device_workspace_bytes < input_bytes) {
        throw GpuError("borrowed dictionary input recorded an invalid transfer or workspace");
    }
    record_gpu_h2d_bytes(telemetry, encoded.h2d_bytes);
    record_gpu_d2h_bytes(telemetry, encoded.d2h_bytes);
    record_gpu_device_allocation_bytes(telemetry, encoded.device_workspace_bytes - input_bytes);
    record_gpu_kernel_work(telemetry, encoded.explicit_kernel_launches,
                           encoded.device_ms.value_or(std::numeric_limits<double>::quiet_NaN()));
}

// Purpose: Frame one block's independent LZ4 segments only when its complete payload wins.
// Inputs: Contiguous encoded segments and the current GPU-native block payload length.
// Outputs: Returns a dense offset table plus segment bytes, or empty when it cannot improve the block.
std::vector<std::byte> frame_dictionary_candidate(std::span<const EncodedSegment> segments,
                                                  std::uint32_t baseline_bytes) {
    const auto table_bytes = (segments.size() + 1U) * sizeof(std::uint32_t);
    if (table_bytes >= baseline_bytes) {
        return {};
    }
    std::size_t encoded_bytes = 0U;
    for (const auto& segment : segments) {
        if (segment.payload.empty() || segment.payload.size() >= baseline_bytes - table_bytes - encoded_bytes) {
            return {};
        }
        encoded_bytes += segment.payload.size();
    }
    std::vector<std::byte> payload;
    payload.reserve(table_bytes + encoded_bytes);
    std::uint32_t cumulative = 0U;
    append_segment_offset(payload, cumulative);
    for (const auto& segment : segments) {
        cumulative += static_cast<std::uint32_t>(segment.payload.size());
        append_segment_offset(payload, cumulative);
    }
    for (const auto& segment : segments) {
        payload.insert(payload.end(), segment.payload.begin(), segment.payload.end());
    }
    return payload;
}

// Purpose: Encode independent 64 KiB segments from borrowed HIP input and admit only a smaller full block.
// Inputs: One source block, its device mirror, baseline payload bytes, effort, and telemetry.
// Outputs: Returns complete table-plus-segment bytes when smaller; otherwise an empty vector.
std::vector<std::byte> encode_dictionary_candidate(std::span<const std::byte> input, const std::byte* device_input,
                                                   std::uint32_t baseline_bytes, const Effort& effort,
                                                   GpuTelemetry* telemetry) {
    const auto segment_count = (input.size() + kSegmentBytes - 1U) / kSegmentBytes;
    const auto table_bytes = (segment_count + 1U) * sizeof(std::uint32_t);
    if (table_bytes >= baseline_bytes) {
        return {};
    }
    std::vector<EncodedSegment> segments;
    segments.reserve(segment_count);
    for (std::size_t offset = 0U; offset < input.size();) {
        auto bytes = std::min<std::size_t>(kMaxBatchBytes, input.size() - offset);
        auto batch = input.subspan(offset, bytes);
        std::vector<std::uint16_t> distances;
        if (input.size() - offset >= kMaxPeriodicBatchBytes) {
            auto periodic = sampled_batch_distances(input.subspan(offset, kMaxPeriodicBatchBytes));
            if (!periodic.empty()) {
                bytes = kMaxPeriodicBatchBytes;
                batch = input.subspan(offset, bytes);
                distances = std::move(periodic);
            }
        }
        if (distances.empty()) {
            distances = sampled_batch_distances(batch);
        }
        auto encoded = encode_segments_from_device_hip(batch, device_input + offset, effort, distances);
        record_dictionary_batch(encoded, bytes, telemetry);
        for (auto& segment : encoded.segments) {
            segments.push_back(std::move(segment));
        }
        offset += bytes;
    }
    if (segments.size() != segment_count) {
        throw GpuError("dictionary candidate segment count differs from its source block");
    }
    return frame_dictionary_candidate(segments, baseline_bytes);
}

}  // namespace

// Purpose: Select only dictionary blocks whose complete version-four payload beats the existing GPU block.
// Inputs: One source chunk, its already uploaded HIP mirror, baseline descriptors, and effort level.
// Outputs: Returns ordered optional dictionary payloads; records speculative HIP resource use.
DictionaryReplacements select_dictionary_replacements(std::span<const std::byte> input, const std::byte* device_input,
                                                      std::span<const BlockDescriptor> blocks, int level,
                                                      GpuTelemetry* telemetry) {
    if (device_input == nullptr) {
        throw GpuError("dictionary candidate device input is unavailable");
    }
    const auto effort = effort_for_level(level);
    DictionaryReplacements replacements(blocks.size());
    std::vector<std::size_t> source_offsets(blocks.size() + 1U);
    std::vector<bool> eligible(blocks.size());
    for (std::size_t index = 0U; index < blocks.size(); ++index) {
        const auto length = static_cast<std::size_t>(blocks[index].uncompressed_len);
        if (source_offsets[index] > input.size() || length > input.size() - source_offsets[index]) {
            throw GpuError("dictionary candidate block exceeds source chunk");
        }
        eligible[index] = should_try_dictionary(input.subspan(source_offsets[index], length), blocks[index], level);
        source_offsets[index + 1U] = source_offsets[index] + length;
    }
    if (source_offsets.back() != input.size()) {
        throw GpuError("dictionary candidate blocks do not cover source chunk");
    }
    for (std::size_t index = 0U; index < blocks.size();) {
        if (!eligible[index]) {
            ++index;
            continue;
        }
        const auto length = static_cast<std::size_t>(blocks[index].uncompressed_len);
        if (length % kSegmentBytes != 0U || length > kMaxBatchBytes) {
            replacements[index] = encode_dictionary_candidate(input.subspan(source_offsets[index], length),
                                                              device_input + source_offsets[index],
                                                              blocks[index].encoded_len, effort, telemetry);
            ++index;
            continue;
        }
        const auto first = index;
        std::size_t batch_bytes = 0U;
        while (index < blocks.size() && eligible[index] && blocks[index].uncompressed_len % kSegmentBytes == 0U &&
               blocks[index].uncompressed_len <= kMaxBatchBytes - batch_bytes) {
            batch_bytes += blocks[index].uncompressed_len;
            ++index;
        }
        const auto batch = input.subspan(source_offsets[first], batch_bytes);
        const auto distances = sampled_batch_distances(batch);
        const auto encoded =
            encode_segments_from_device_hip(batch, device_input + source_offsets[first], effort, distances);
        record_dictionary_batch(encoded, batch_bytes, telemetry);
        std::size_t segment_offset = 0U;
        for (std::size_t block = first; block < index; ++block) {
            const auto segment_count = blocks[block].uncompressed_len / kSegmentBytes;
            replacements[block] = frame_dictionary_candidate(
                std::span(encoded.segments).subspan(segment_offset, segment_count), blocks[block].encoded_len);
            segment_offset += segment_count;
        }
        if (segment_offset != encoded.segments.size()) {
            throw GpuError("dictionary batch segments do not match grouped source blocks");
        }
    }
    return replacements;
}

// Purpose: Publish only smaller dictionary blocks without changing baseline bytes for other blocks.
// Inputs: A complete encoded chunk and optional candidate payloads in block order.
// Outputs: Returns a dense mixed chunk with the original source CRC and unchanged nonwinning payloads.
EncodedChunk apply_dictionary_replacements(EncodedChunk baseline, const DictionaryReplacements& replacements) {
    if (replacements.size() != baseline.blocks.size()) {
        throw GpuError("dictionary replacement count differs from baseline blocks");
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
            throw GpuError("dictionary baseline block exceeds encoded payload");
        }
        descriptor.encoded_offset = result.payload.size();
        if (!replacement.empty()) {
            if (replacement.size() >= descriptor.encoded_len || replacement.size() >= descriptor.uncompressed_len) {
                throw GpuError("dictionary replacement does not improve the baseline block");
            }
            descriptor.kind = BlockKind::GpuDictionary;
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

}  // namespace superzip::dictionary
