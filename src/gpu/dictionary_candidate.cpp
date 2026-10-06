#include "gpu/dictionary_candidate.hpp"

#include "core/result.hpp"
#include "gpu/dictionary_device.hpp"
#include "gpu/gpu_codec.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <memory_resource>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace superzip::dictionary {
namespace {

// Purpose: Avoid expensive dictionary trials on blocks without sampled repeated substrings.
// Inputs: One bounded source block sampled evenly at no more than 4,096 twelve-byte positions.
// Outputs: Returns true after eight segment-local repeats; all scratch is released on return or exception.
bool has_dictionary_sample_repeats(std::span<const std::byte> input) {
    constexpr std::size_t kMaxSamples = 4096U;
    constexpr std::size_t kRequiredRepeats = 8U;
    constexpr std::size_t kSampleBytes = 12U;
    const auto stride = std::max<std::size_t>(64U, (input.size() + kMaxSamples - 1U) / kMaxSamples);
    // Recycle nodes across segments; reserve only the maximum keys live in one segment.
    std::pmr::unsynchronized_pool_resource pool;
    std::pmr::unordered_set<std::string_view> seen{&pool};
    seen.reserve(std::min<std::size_t>((kSegmentBytes + stride - 1U) / stride, input.size() / stride + 1U));
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
// Inputs: One independent dictionary segment; first try powers of two, then bounded exact-anchor offsets.
// Outputs: Returns a sampled distance or zero; HIP must verify every reference before encoding.
std::uint16_t sampled_periodic_distance(std::span<const std::byte> segment) {
    constexpr std::array<std::uint16_t, 8> kDistances{256U, 512U, 1024U, 2048U, 4096U, 8192U, 16384U, 32768U};
    constexpr std::size_t kAnchorBytes = 16U;
    constexpr std::size_t kSamples = 128U;
    const auto supports_distance = [&](std::size_t distance) {
        if (segment.size() < static_cast<std::size_t>(distance) * 2U ||
            !std::equal(segment.begin(), segment.begin() + kAnchorBytes, segment.begin() + distance)) {
            return false;
        }
        const auto remaining = segment.size() - distance;
        std::size_t matches = 0U;
        for (std::size_t sample = 0U; sample < kSamples; ++sample) {
            const auto offset = static_cast<std::size_t>(distance) + sample * (remaining - 1U) / (kSamples - 1U);
            matches += segment[offset] == segment[offset - distance];
        }
        return matches * 16U >= kSamples * 15U;
    };
    for (const auto distance : kDistances) {
        if (supports_distance(distance)) {
            return distance;
        }
    }
    const auto max_distance = std::min<std::size_t>(32768U, segment.size() / 2U);
    if (max_distance < 256U) {
        return 0U;
    }
    const auto finish = segment.begin() + static_cast<std::ptrdiff_t>(max_distance + 1U);
    for (auto candidate = std::find(segment.begin() + 256U, finish, segment.front()); candidate != finish;
         candidate = std::find(candidate + 1, finish, segment.front())) {
        const auto distance = static_cast<std::size_t>(candidate - segment.begin());
        if (supports_distance(distance)) {
            return static_cast<std::uint16_t>(distance);
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
// Inputs: Source bytes and their current GPU-native descriptor.
// Outputs: Returns true when sampled repeats or an off-grid periodic segment can plausibly improve encoded size.
bool should_try_dictionary(std::span<const std::byte> input, const BlockDescriptor& baseline) {
    if (input.size() < 4096U || baseline.encoded_len == 0U ||
        (baseline.kind != BlockKind::Raw && baseline.kind != BlockKind::GpuPrefix &&
         baseline.kind != BlockKind::GpuAdaptivePrefix && baseline.kind != BlockKind::GpuHuffman &&
         !is_gpu_sparse_pattern_kind(baseline.kind))) {
        return false;
    }
    if (has_dictionary_sample_repeats(input)) {
        return true;
    }
    return sampled_periodic_distance(input.first(std::min<std::size_t>(input.size(), kSegmentBytes))) != 0U;
}

// Purpose: Append a bounded little-endian segment offset to a native dictionary block payload.
// Inputs: `payload` is the growing header and `offset` is the cumulative encoded segment byte count.
// Outputs: Appends exactly four bytes.
void append_segment_offset(std::vector<std::byte>& payload, std::uint32_t offset) {
    for (std::uint32_t byte = 0U; byte < sizeof(std::uint32_t); ++byte) {
        payload.push_back(static_cast<std::byte>((offset >> (byte * 8U)) & 0xFFU));
    }
}

// Purpose: Record exact completed dictionary work after its encoder-specific admission has been checked.
// Inputs: Validated telemetry, borrowed source bytes and optional operation counters.
// Outputs: Accumulates actual transfers, owned workspace, launches and HIP event time, including discarded trials.
void record_dictionary_work(const EncodedBatch& encoded, std::size_t input_bytes, GpuTelemetry* telemetry) {
    record_gpu_h2d_bytes(telemetry, encoded.h2d_bytes);
    record_gpu_d2h_bytes(telemetry, encoded.d2h_bytes);
    record_gpu_device_allocation_bytes(telemetry, encoded.device_workspace_bytes - input_bytes);
    record_gpu_kernel_work(telemetry, encoded.explicit_kernel_launches,
                           encoded.device_ms.value_or(std::numeric_limits<double>::quiet_NaN()));
}

// Purpose: Account for the measured work of one dictionary batch using an existing device input buffer.
// Inputs: Validated packed batch metadata, borrowed input bytes, and optional operation telemetry.
// Outputs: Adds metadata transfers, allocated workspace, explicit launches, and HIP device time.
void record_dictionary_batch(const PackedEncodedBatch& batch, std::size_t input_bytes, GpuTelemetry* telemetry) {
    const auto& encoded = batch.telemetry;
    const auto period_bytes = ((input_bytes + kSegmentBytes - 1U) / kSegmentBytes) * sizeof(std::uint16_t);
    if ((encoded.h2d_bytes != 0U && encoded.h2d_bytes != period_bytes) ||
        encoded.explicit_kernel_launches != (encoded.h2d_bytes == 0U ? 4U : 2U) ||
        encoded.device_workspace_bytes < input_bytes) {
        throw GpuError("borrowed dictionary input recorded an invalid transfer or workspace");
    }
    record_dictionary_work(encoded, input_bytes, telemetry);
}

// Purpose: Require honest variable-launch telemetry from the staged minimum-byte borrowed-input encoder.
// Inputs: Completed Neutron batch and its exact nonempty admitted source extent.
// Outputs: Rejects inconsistent launches, uploads or workspace before accumulating the actual device work.
void record_neutron_batch(const PackedEncodedBatch& batch, std::size_t input_bytes, GpuTelemetry* telemetry) {
    if (input_bytes == 0U || input_bytes > kMaxNeutronBatchBytes) {
        throw GpuError("Neutron dictionary telemetry source extent is invalid");
    }
    const auto segment_bytes = std::min<std::size_t>(input_bytes, kSegmentBytes);
    const auto segments = (input_bytes + kSegmentBytes - 1U) / kSegmentBytes;
    const auto search_tiles = (input_bytes + kNeutronSearchTileBytes - 1U) / kNeutronSearchTileBytes;
    const auto emit_tiles = (segment_bytes / 4U + kNeutronEmitSequences) / kNeutronEmitSequences;
    const auto& encoded = batch.telemetry;
    std::size_t parse_bytes = 0U;
    for (std::size_t segment = 0U; segment < segments; ++segment) {
        if ((encoded.neutron_active_segment_mask & (1U << segment)) != 0U) {
            parse_bytes =
                std::max(parse_bytes, std::min(input_bytes - segment * kSegmentBytes, std::size_t{kSegmentBytes}));
        }
    }
    const auto expected_parse_launches = (parse_bytes + kNeutronParseTilePositions - 1U) / kNeutronParseTilePositions;
    const auto admitted_segment_mask = (1U << segments) - 1U;
    const auto budget_launches = encoded.neutron_budget_kernel_launches;
    if (!encoded.gpu_used || encoded.h2d_bytes != 0U ||
        (encoded.neutron_active_segment_mask & ~admitted_segment_mask) != 0U ||
        (encoded.neutron_pruned_segment_mask & ~admitted_segment_mask) != 0U ||
        (encoded.neutron_pruned_segment_mask & encoded.neutron_active_segment_mask) != 0U || budget_launches > 2U ||
        ((budget_launches == 2U) != (encoded.neutron_pruned_segment_mask != 0U)) ||
        encoded.neutron_parse_launches != expected_parse_launches ||
        encoded.explicit_kernel_launches < 7U + search_tiles + expected_parse_launches + budget_launches ||
        encoded.explicit_kernel_launches > 6U + search_tiles + expected_parse_launches + emit_tiles + budget_launches ||
        encoded.device_workspace_bytes < input_bytes || encoded.device_workspace_bytes > kMaxWorkspaceBytes) {
        throw GpuError("Neutron dictionary batch recorded inconsistent device work");
    }
    record_dictionary_work(encoded, input_bytes, telemetry);
}

// Purpose: Frame one block's contiguous LZ4 segments only when its complete payload wins.
// Inputs: Validated segment sizes, their concatenated bytes, and the current GPU-native block payload length.
// Outputs: Returns a dense offset table plus segment bytes, or empty when it cannot improve the block.
std::vector<std::byte> frame_dictionary_candidate(std::span<const std::uint32_t> sizes,
                                                  std::span<const std::byte> packed, std::uint32_t baseline_bytes) {
    const auto table_bytes = (sizes.size() + 1U) * sizeof(std::uint32_t);
    if (table_bytes >= baseline_bytes) {
        return {};
    }
    std::size_t encoded_bytes = 0U;
    for (const auto size : sizes) {
        if (size == 0U || size >= baseline_bytes - table_bytes - encoded_bytes) {
            return {};
        }
        encoded_bytes += size;
    }
    if (encoded_bytes != packed.size()) {
        throw GpuError("dictionary packed payload differs from segment sizes");
    }
    std::vector<std::byte> payload;
    payload.reserve(table_bytes + encoded_bytes);
    std::uint32_t cumulative = 0U;
    append_segment_offset(payload, cumulative);
    for (const auto size : sizes) {
        cumulative += size;
        append_segment_offset(payload, cumulative);
    }
    payload.insert(payload.end(), packed.begin(), packed.end());
    return payload;
}

// Purpose: Compare a complete bounded minimum-byte parse against one block's best existing payload.
// Inputs: One admitted source/device mirror, baseline full payload bytes, telemetry and cancellation checkpoint.
// Outputs: Returns a smaller framed dictionary payload or empty; abandons trials that already cannot win.
std::vector<std::byte> encode_neutron_candidate(std::span<const std::byte> input, const std::byte* device_input,
                                                std::uint32_t baseline_bytes, GpuTelemetry* telemetry,
                                                const EncodeCheckpoint& checkpoint) {
    const auto segment_count = (input.size() + kSegmentBytes - 1U) / kSegmentBytes;
    const auto table_bytes = (segment_count + 1U) * sizeof(std::uint32_t);
    if (table_bytes >= baseline_bytes) {
        return {};
    }
    std::vector<std::uint32_t> segment_sizes;
    std::vector<std::byte> packed;
    segment_sizes.reserve(segment_count);
    for (std::size_t offset = 0U; offset < input.size();) {
        const auto bytes = std::min(kMaxNeutronBatchBytes, input.size() - offset);
        const auto remaining = baseline_bytes - table_bytes - packed.size();
        const std::array<NeutronParseBudget, 1> budget{
            {{static_cast<std::uint32_t>(bytes), static_cast<std::uint32_t>(remaining)}}};
        const auto limits =
            remaining <= bytes ? std::span<const NeutronParseBudget>(budget) : std::span<const NeutronParseBudget>{};
        auto encoded = encode_neutron_segments_from_device_hip(input.subspan(offset, bytes), device_input + offset,
                                                               checkpoint, limits);
        record_neutron_batch(encoded, bytes, telemetry);
        if (encoded.payload.size() >= baseline_bytes - table_bytes - packed.size()) {
            return {};
        }
        segment_sizes.insert(segment_sizes.end(), encoded.segment_sizes.begin(), encoded.segment_sizes.end());
        packed.insert(packed.end(), encoded.payload.begin(), encoded.payload.end());
        offset += bytes;
    }
    if (segment_sizes.size() != segment_count) {
        throw GpuError("Neutron candidate segment count differs from its source block");
    }
    return frame_dictionary_candidate(segment_sizes, packed, baseline_bytes);
}

// Purpose: Group only complete segment-aligned blocks whose framing can still beat their existing winner.
// Inputs: Validated descriptor and winner bytes; the caller also bounds the total batch to one MiB.
// Outputs: Returns true for a competitive aligned block without changing any payload or GPU limit.
bool can_group_neutron_block(const BlockDescriptor& block, const std::vector<std::byte>& replacement) {
    const auto bytes = static_cast<std::size_t>(block.uncompressed_len);
    const auto baseline = replacement.empty() ? block.encoded_len : replacement.size();
    const auto table_bytes = (bytes / kSegmentBytes + 1U) * sizeof(std::uint32_t);
    return bytes % kSegmentBytes == 0U && bytes <= kMaxNeutronBatchBytes && table_bytes < baseline;
}

// Purpose: Share one bounded Neutron parse across adjacent blocks while preserving independent block framing.
// Inputs: Admitted aligned host/device extent, matching descriptors/winners, telemetry and checkpoint.
// Outputs: Replaces only smaller complete block payloads; validates packed extents before forming subviews.
void encode_neutron_group(std::span<const std::byte> input, const std::byte* device_input,
                          std::span<const BlockDescriptor> blocks, std::span<std::vector<std::byte>> replacements,
                          GpuTelemetry* telemetry, const EncodeCheckpoint& checkpoint) {
    std::vector<NeutronParseBudget> budgets;
    budgets.reserve(blocks.size());
    for (std::size_t index = 0U; index < blocks.size(); ++index) {
        const auto bytes = blocks[index].uncompressed_len;
        const auto baseline = replacements[index].empty() ? blocks[index].encoded_len : replacements[index].size();
        const auto table_bytes = (bytes / kSegmentBytes + 1U) * sizeof(std::uint32_t);
        budgets.push_back({bytes, static_cast<std::uint32_t>(baseline - table_bytes)});
    }
    const auto encoded = encode_neutron_segments_from_device_hip(input, device_input, checkpoint, budgets);
    record_neutron_batch(encoded, input.size(), telemetry);
    if (encoded.segment_sizes.size() != input.size() / kSegmentBytes) {
        throw GpuError("Neutron group segment count differs from its source extent");
    }
    std::size_t packed_bytes = 0U;
    for (const auto size : encoded.segment_sizes) {
        if (size == 0U || size > encoded.payload.size() - packed_bytes) {
            throw GpuError("Neutron group segment exceeds its packed payload");
        }
        packed_bytes += size;
    }
    if (packed_bytes != encoded.payload.size()) {
        throw GpuError("Neutron group sizes do not cover its packed payload");
    }
    std::size_t segment_offset = 0U;
    std::size_t payload_offset = 0U;
    for (std::size_t index = 0U; index < blocks.size(); ++index) {
        const auto count = blocks[index].uncompressed_len / kSegmentBytes;
        if (count > encoded.segment_sizes.size() - segment_offset) {
            throw GpuError("Neutron group has fewer segments than its block descriptors");
        }
        const auto sizes = std::span(encoded.segment_sizes).subspan(segment_offset, count);
        std::size_t bytes = 0U;
        for (const auto size : sizes) {
            if (size > encoded.payload.size() - payload_offset - bytes) {
                throw GpuError("Neutron group block exceeds its packed payload");
            }
            bytes += size;
        }
        const auto baseline = replacements[index].empty() ? blocks[index].encoded_len : replacements[index].size();
        auto candidate = frame_dictionary_candidate(sizes, std::span(encoded.payload).subspan(payload_offset, bytes),
                                                    static_cast<std::uint32_t>(baseline));
        if (!candidate.empty()) {
            replacements[index] = std::move(candidate);
        }
        segment_offset += count;
        payload_offset += bytes;
    }
    if (segment_offset != encoded.segment_sizes.size() || payload_offset != encoded.payload.size()) {
        throw GpuError("Neutron group descriptors do not cover its packed segments");
    }
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
    std::vector<std::uint32_t> segment_sizes;
    segment_sizes.reserve(segment_count);
    std::vector<std::byte> packed_payload;
    packed_payload.reserve(std::min<std::size_t>(input.size(), baseline_bytes));
    for (std::size_t offset = 0U; offset < input.size();) {
        auto bytes = std::min<std::size_t>(kMaxBatchBytes, input.size() - offset);
        auto batch = input.subspan(offset, bytes);
        std::vector<std::uint16_t> distances;
        for (const auto candidate_bytes : {kMaxPeriodicBatchBytes, kMaxBatchBytes * 2U}) {
            if (input.size() - offset < candidate_bytes) {
                continue;
            }
            auto periodic = sampled_batch_distances(input.subspan(offset, candidate_bytes));
            if (!periodic.empty()) {
                bytes = candidate_bytes;
                batch = input.subspan(offset, bytes);
                distances = std::move(periodic);
                break;
            }
        }
        if (distances.empty()) {
            distances = sampled_batch_distances(batch);
        }
        auto encoded = encode_segments_from_device_hip(batch, device_input + offset, effort, distances);
        record_dictionary_batch(encoded, bytes, telemetry);
        segment_sizes.insert(segment_sizes.end(), encoded.segment_sizes.begin(), encoded.segment_sizes.end());
        packed_payload.insert(packed_payload.end(), encoded.payload.begin(), encoded.payload.end());
        offset += bytes;
    }
    if (segment_sizes.size() != segment_count) {
        throw GpuError("dictionary candidate segment count differs from its source block");
    }
    return frame_dictionary_candidate(segment_sizes, packed_payload, baseline_bytes);
}

// Purpose: Encode adjacent independent blocks in one bounded HIP batch without merging their archive payloads.
// Inputs: Contiguous source/device bytes, aligned block descriptors, effort, optional verified periodic distances,
// telemetry, and disjoint replacement slots. Outputs: Writes only winning per-block payloads or throws on mismatch.
void encode_dictionary_group(std::span<const std::byte> input, const std::byte* device_input,
                             std::span<const BlockDescriptor> blocks, const Effort& effort,
                             std::span<const std::uint16_t> distances, GpuTelemetry* telemetry,
                             std::span<std::vector<std::byte>> replacements) {
    if (blocks.size() != replacements.size()) {
        throw GpuError("dictionary group descriptor count differs from replacement slots");
    }
    const auto encoded = encode_segments_from_device_hip(input, device_input, effort, distances);
    record_dictionary_batch(encoded, input.size(), telemetry);
    std::size_t segment_offset = 0U;
    std::size_t payload_offset = 0U;
    for (std::size_t block = 0U; block < blocks.size(); ++block) {
        const auto segment_count = blocks[block].uncompressed_len / kSegmentBytes;
        if (segment_count > encoded.segment_sizes.size() - segment_offset) {
            throw GpuError("dictionary group has fewer segments than its block descriptors");
        }
        const auto block_sizes = std::span(encoded.segment_sizes).subspan(segment_offset, segment_count);
        std::size_t block_bytes = 0U;
        for (const auto size : block_sizes) {
            block_bytes += size;
        }
        if (block_bytes > encoded.payload.size() - payload_offset) {
            throw GpuError("dictionary group payload exceeds packed batch");
        }
        replacements[block] = frame_dictionary_candidate(
            block_sizes, std::span(encoded.payload).subspan(payload_offset, block_bytes), blocks[block].encoded_len);
        segment_offset += segment_count;
        payload_offset += block_bytes;
    }
    if (segment_offset != encoded.segment_sizes.size() || payload_offset != encoded.payload.size()) {
        throw GpuError("dictionary batch segments do not match grouped source blocks");
    }
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
        eligible[index] = should_try_dictionary(input.subspan(source_offsets[index], length), blocks[index]);
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
        if (length % kSegmentBytes == 0U && length <= kMaxPeriodicBatchBytes) {
            std::size_t periodic_end = index;
            std::size_t periodic_bytes = 0U;
            while (periodic_end < blocks.size() && eligible[periodic_end] &&
                   blocks[periodic_end].uncompressed_len % kSegmentBytes == 0U &&
                   blocks[periodic_end].uncompressed_len <= kMaxPeriodicBatchBytes - periodic_bytes) {
                periodic_bytes += blocks[periodic_end].uncompressed_len;
                ++periodic_end;
            }
            if (periodic_end > index + 1U && periodic_bytes > kMaxBatchBytes) {
                const auto batch = input.subspan(source_offsets[index], periodic_bytes);
                const auto distances = sampled_batch_distances(batch);
                if (!distances.empty()) {
                    encode_dictionary_group(batch, device_input + source_offsets[index],
                                            blocks.subspan(index, periodic_end - index), effort, distances, telemetry,
                                            std::span(replacements).subspan(index, periodic_end - index));
                    index = periodic_end;
                    continue;
                }
            }
        }
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
        encode_dictionary_group(batch, device_input + source_offsets[first], blocks.subspan(first, index - first),
                                effort, distances, telemetry, std::span(replacements).subspan(first, index - first));
    }
    return replacements;
}

// Purpose: Retain the complete ordinary portfolio while considering stronger minimum-byte GPU dictionary payloads.
// Inputs: Exact source/device mirrors, validated block descriptors, existing winners, telemetry and checkpoint.
// Outputs: Mutates only winning replacement slots; rejects coverage inconsistencies before pointer formation.
void improve_neutron_replacements(std::span<const std::byte> input, const std::byte* device_input,
                                  std::span<const BlockDescriptor> blocks, DictionaryReplacements& replacements,
                                  GpuTelemetry* telemetry, const EncodeCheckpoint& checkpoint) {
    if (device_input == nullptr || blocks.size() != replacements.size()) {
        throw GpuError("Neutron replacement inputs are inconsistent");
    }
    validate_neutron_layout(input.size(), blocks, replacements);
    std::size_t offset = 0U;
    for (std::size_t index = 0U; index < blocks.size();) {
        const auto bytes = static_cast<std::size_t>(blocks[index].uncompressed_len);
        std::size_t end = index;
        std::size_t group_bytes = 0U;
        while (end < blocks.size() && can_group_neutron_block(blocks[end], replacements[end]) &&
               blocks[end].uncompressed_len <= kMaxNeutronBatchBytes - group_bytes) {
            group_bytes += blocks[end].uncompressed_len;
            ++end;
        }
        if (end > index + 1U) {
            encode_neutron_group(input.subspan(offset, group_bytes), device_input + offset,
                                 blocks.subspan(index, end - index),
                                 std::span(replacements).subspan(index, end - index), telemetry, checkpoint);
            offset += group_bytes;
            index = end;
            continue;
        }
        const auto baseline = replacements[index].empty() ? blocks[index].encoded_len : replacements[index].size();
        auto candidate = encode_neutron_candidate(input.subspan(offset, bytes), device_input + offset,
                                                  static_cast<std::uint32_t>(baseline), telemetry, checkpoint);
        if (!candidate.empty()) {
            replacements[index] = std::move(candidate);
        }
        offset += bytes;
        ++index;
    }
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
