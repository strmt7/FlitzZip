#include "gpu/hip_codec_support.hpp"
#include "gpu/entropy_sampling.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <queue>
#include <vector>
#include <hip/hip_runtime.h>

namespace superzip::hip_detail {
namespace {

struct AdaptiveEncodeTable {
    std::uint16_t code[256];
    std::uint8_t width[256];
};

struct AdaptiveCodebookEstimate {
    std::vector<std::byte> codebook;
    std::uint64_t sampled_bits = 0;
    std::size_t sample_count = 0;
};

struct AdaptiveEncodeSegmentPlan {
    std::uint64_t block_start;
    std::uint32_t block_len;
    std::uint32_t segment_index;
    std::uint32_t table_index;
};

constexpr std::uint32_t kMaxEntropyCandidates = 16U;

struct EntropyLengthSegmentPlan {
    std::uint64_t block_start;
    std::uint32_t block_len;
    std::uint32_t segment_index;
    std::uint32_t table_index;
    std::uint32_t candidate_count;
    std::uint32_t length_offset;
};

struct EntropyEncodeCandidate {
    BlockKind kind;
    std::uint32_t table_index;
    std::vector<std::byte> codebook;
};

struct AdaptiveEncodeBlockPlan {
    std::size_t block_start = 0;
    std::uint32_t block_len = 0;
    std::uint32_t payload_limit = 0;
    std::uint32_t segment_offset = 0;
    std::uint32_t segment_count = 0;
    std::uint32_t table_index = 0;
    bool use_adaptive = false;
    std::uint32_t bitstream_offset = 0;
    std::uint32_t bitstream_bytes = 0;
    BlockKind kind = BlockKind::GpuAdaptivePrefix;
    std::vector<std::byte> codebook;
    std::vector<std::uint32_t> offsets;
    std::vector<EntropyEncodeCandidate> candidates;
};

struct AdaptiveBatchSelection {
    std::uint64_t adaptive_blocks = 0;
    std::uint32_t bitstream_bytes = 0;
    std::vector<AdaptiveEncodeSegmentPlan> pack_plans;
    std::vector<std::uint32_t> pack_offsets;
    std::vector<AdaptiveEncodeTable> pack_tables;
};

// Purpose: Rank symbols identically for both admission probes and full adaptive codebooks.
// Inputs: A complete or sampled byte histogram.
// Outputs: Returns all byte values ordered by descending frequency, with stable numeric tie breaks.
std::array<std::uint16_t, 256> rank_adaptive_symbols(const std::array<std::uint64_t, 256>& histogram) {
    std::array<std::uint16_t, 256> order{};
    std::iota(order.begin(), order.end(), static_cast<std::uint16_t>(0));
    std::stable_sort(order.begin(), order.end(), [&](std::uint16_t lhs, std::uint16_t rhs) {
        if (histogram[lhs] != histogram[rhs]) {
            return histogram[lhs] > histogram[rhs];
        }
        return lhs < rhs;
    });
    return order;
}

// Purpose: Admit promising low/mid-effort adaptive candidates without scanning full blocks unnecessarily.
// Inputs: One eligible source block and validated compression level 2-9.
// Outputs: Returns true for high efforts or when actual code widths predict a clear sampled size gain.
bool should_try_sampled_adaptive_prefix(std::span<const std::byte> block, int compression_level) {
    if (compression_level >= 7) {
        return true;
    }
    std::array<std::uint64_t, 256> histogram{};
    constexpr std::size_t window_bytes = 256U;
    for (const auto start : {std::size_t{0}, block.size() / 2U, block.size() - window_bytes}) {
        for (std::size_t index = start; index < start + window_bytes; ++index) {
            ++histogram[static_cast<std::uint8_t>(block[index])];
        }
    }
    const auto order = rank_adaptive_symbols(histogram);
    std::uint64_t static_bits = 0;
    std::uint64_t adaptive_bits = 0;
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
        const auto value = order[rank];
        const auto static_width = value < 4U ? 3U : value < 20U ? 6U : value < 84U ? 9U : 11U;
        const auto adaptive_width = rank < kGpuAdaptivePrefixSmallSymbols                                     ? 3U
                                    : rank < kGpuAdaptivePrefixSmallSymbols + kGpuAdaptivePrefixMediumSymbols ? 6U
                                    : rank < kGpuAdaptivePrefixCodebookBytes                                  ? 9U
                                                                                                              : 11U;
        static_bits += histogram[value] * static_width;
        adaptive_bits += histogram[value] * adaptive_width;
    }
    return adaptive_bits * 100U < static_bits * 95U;
}

// Purpose: Count encoded bits for one worker range using a per-block adaptive prefix table.
// Inputs: `input`, `table`, `block_start`, `start`, and `end` describe readable bytes and adaptive widths.
// Outputs: Returns the number of encoded bits for the worker range.
__device__ std::uint32_t gpu_adaptive_prefix_range_bit_count(const std::byte* input, const AdaptiveEncodeTable* table,
                                                             std::size_t block_start, std::size_t start,
                                                             std::size_t end) {
    std::uint32_t bits = 0;
    for (auto pos = start; pos < end; ++pos) {
        bits += table->width[static_cast<std::uint8_t>(input[block_start + pos])];
    }
    return bits;
}

// Purpose: Measure every admitted entropy table while loading each source segment only once.
// Inputs: Bounded plans, contiguous candidate tables, output counts, and Capacity (2/4/8/16) bound shared scratch.
// Outputs: Writes exact, word-aligned segment lengths for every candidate without packing any payload.
template <std::uint32_t Capacity>
__global__ void entropy_segment_lengths_batch_kernel(const std::byte* input, const EntropyLengthSegmentPlan* plans,
                                                     const AdaptiveEncodeTable* tables, std::uint32_t* segment_lengths,
                                                     std::uint32_t segment_count) {
    const auto segment = static_cast<std::uint32_t>(blockIdx.x);
    if (segment >= segment_count) {
        return;
    }
    const auto& plan = plans[segment];
    __shared__ std::uint8_t widths[Capacity][256];
    __shared__ std::uint32_t sums[Capacity][kGpuPrefixSegmentThreads];
    const auto thread_id = static_cast<std::uint32_t>(threadIdx.x);
#pragma unroll
    for (std::uint32_t candidate = 0U; candidate < Capacity; ++candidate) {
        if (candidate < plan.candidate_count) {
            for (auto symbol = thread_id; symbol < 256U; symbol += kGpuPrefixSegmentThreads) {
                widths[candidate][symbol] = tables[plan.table_index + candidate].width[symbol];
            }
        }
    }
    __syncthreads();
    std::uint32_t local_bits[Capacity] = {};
    const auto start = static_cast<std::size_t>(plan.segment_index) * kGpuPrefixSegmentBytes;
    const auto end = min(static_cast<std::size_t>(plan.block_len), start + kGpuPrefixSegmentBytes);
    for (auto pos = start + thread_id; pos < end; pos += kGpuPrefixSegmentThreads) {
        const auto value = static_cast<std::uint8_t>(input[static_cast<std::size_t>(plan.block_start) + pos]);
#pragma unroll
        for (std::uint32_t candidate = 0U; candidate < Capacity; ++candidate) {
            if (candidate < plan.candidate_count) {
                local_bits[candidate] += widths[candidate][value];
            }
        }
    }
#pragma unroll
    for (std::uint32_t candidate = 0U; candidate < Capacity; ++candidate) {
        if (candidate < plan.candidate_count) {
            sums[candidate][thread_id] = local_bits[candidate];
        }
    }
    __syncthreads();
    for (std::uint32_t offset = kGpuPrefixSegmentThreads / 2U; offset > 0U; offset >>= 1U) {
        if (thread_id < offset) {
#pragma unroll
            for (std::uint32_t candidate = 0U; candidate < Capacity; ++candidate) {
                if (candidate < plan.candidate_count) {
                    sums[candidate][thread_id] += sums[candidate][thread_id + offset];
                }
            }
        }
        __syncthreads();
    }
    if (thread_id == 0U) {
        for (std::uint32_t candidate = 0U; candidate < plan.candidate_count; ++candidate) {
            const auto bytes = (sums[candidate][0] + 7U) / 8U;
            segment_lengths[plan.length_offset + candidate] = (bytes + 3U) & ~3U;
        }
    }
}

// Purpose: Pack a batched list of byte segments with adaptive GPU prefix code tables.
// Inputs: `input`, `plans`, `tables`, `segment_offsets`, and `encoded` describe the selected output spans.
// Outputs: Writes adaptive prefix bitstreams with atomic word updates.
__global__ void adaptive_prefix_pack_segments_batch_kernel(const std::byte* input,
                                                           const AdaptiveEncodeSegmentPlan* plans,
                                                           const AdaptiveEncodeTable* tables,
                                                           const std::uint32_t* segment_offsets, std::byte* encoded,
                                                           std::uint32_t segment_count) {
    const auto segment = static_cast<std::uint32_t>(blockIdx.x);
    if (segment >= segment_count) {
        return;
    }
    const auto& plan = plans[segment];
    const auto* table = tables + plan.table_index;
    __shared__ std::uint32_t sums[kGpuPrefixSegmentThreads];
    const auto thread_id = static_cast<std::uint32_t>(threadIdx.x);
    std::size_t start = 0;
    std::size_t end = 0;
    gpu_prefix_thread_range(plan.block_len, plan.segment_index, thread_id, start, end);
    const auto local_bits =
        gpu_adaptive_prefix_range_bit_count(input, table, static_cast<std::size_t>(plan.block_start), start, end);
    const auto thread_bit_base = gpu_prefix_exclusive_scan(sums, local_bits);
    if (local_bits == 0U) {
        return;
    }
    unsigned int scratch[kGpuPrefixMaxThreadWords] = {};
    auto local_bit_pos = thread_bit_base % 32U;
    for (auto pos = start; pos < end; ++pos) {
        const auto value = static_cast<std::uint8_t>(input[static_cast<std::size_t>(plan.block_start) + pos]);
        const auto width = static_cast<std::uint32_t>(table->width[value]);
        const auto code = static_cast<std::uint32_t>(table->code[value]);
        for (std::uint32_t bit = 0; bit < width; ++bit) {
            if (((code >> bit) & 1U) != 0U) {
                scratch[local_bit_pos / 32U] |= 1U << (local_bit_pos % 32U);
            }
            ++local_bit_pos;
        }
    }
    auto* segment_words = reinterpret_cast<unsigned int*>(encoded + segment_offsets[segment]);
    const auto word_base = thread_bit_base / 32U;
    const auto word_count = ((thread_bit_base % 32U) + local_bits + 31U) / 32U;
    for (std::uint32_t word = 0; word < word_count; ++word) {
        if (scratch[word] != 0U) {
            atomicOr(segment_words + word_base + word, scratch[word]);
        }
    }
}

// Purpose: Build an adaptive prefix codebook and device encode table for one archive block.
// Inputs: Immutable sampled byte frequencies and a mutable device encoder table.
// Outputs: Returns the serialized codebook and sampled bit estimate; fills `table` with device-ready codes and widths.
AdaptiveCodebookEstimate build_adaptive_prefix_codebook(const std::array<std::uint64_t, 256>& histogram,
                                                        AdaptiveEncodeTable& table) {
    const auto order = rank_adaptive_symbols(histogram);

    AdaptiveCodebookEstimate estimate;
    estimate.codebook.resize(kGpuAdaptivePrefixCodebookBytes);
    for (std::size_t i = 0; i < estimate.codebook.size(); ++i) {
        estimate.codebook[i] = static_cast<std::byte>(order[i]);
    }
    for (std::uint32_t value = 0; value < 256U; ++value) {
        table.code[value] = static_cast<std::uint16_t>(0x7U | (value << 3U));
        table.width[value] = 11U;
    }
    for (std::uint32_t rank = 0; rank < kGpuAdaptivePrefixCodebookBytes; ++rank) {
        const auto value = static_cast<std::uint8_t>(estimate.codebook[rank]);
        if (rank < kGpuAdaptivePrefixSmallSymbols) {
            table.code[value] = static_cast<std::uint16_t>(rank << 1U);
            table.width[value] = 3U;
        } else if (rank < kGpuAdaptivePrefixSmallSymbols + kGpuAdaptivePrefixMediumSymbols) {
            table.code[value] = static_cast<std::uint16_t>(0x1U | ((rank - kGpuAdaptivePrefixSmallSymbols) << 2U));
            table.width[value] = 6U;
        } else {
            table.code[value] = static_cast<std::uint16_t>(
                0x3U | ((rank - kGpuAdaptivePrefixSmallSymbols - kGpuAdaptivePrefixMediumSymbols) << 3U));
            table.width[value] = 9U;
        }
    }
    for (std::uint32_t value = 0; value < histogram.size(); ++value) {
        estimate.sampled_bits += histogram[value] * table.width[value];
        estimate.sample_count += histogram[value];
    }
    return estimate;
}

struct HuffmanNode {
    std::uint64_t weight = 0U;
    std::uint16_t parent = 0xFFFFU;
    std::uint16_t smallest_symbol = 0U;
};

// Purpose: Construct a deterministic length-bounded canonical Huffman code from a sampled block.
// Inputs: Immutable sampled frequencies, whether every source byte was sampled, and a mutable encoder table.
// Outputs: Returns a sample estimate and device table, or empty for overlong trees; serializes no losing lookup.
std::optional<AdaptiveCodebookEstimate> build_huffman_prefix_codebook(const std::array<std::uint64_t, 256>& histogram,
                                                                      bool complete_sample,
                                                                      AdaptiveEncodeTable& table) {
    const auto sample_count = std::accumulate(histogram.begin(), histogram.end(), std::uint64_t{0});
    const auto frequency_floor = std::max<std::uint64_t>(1U, sample_count / 4096U);

    std::array<HuffmanNode, 511> nodes{};
    std::uint16_t active_count = 0U;
    for (std::uint16_t symbol = 0U; symbol < 256U; ++symbol) {
        if (complete_sample && histogram[symbol] == 0U) {
            continue;
        }
        nodes[symbol].weight = histogram[symbol] + frequency_floor;
        nodes[symbol].smallest_symbol = symbol;
        ++active_count;
    }
    if (active_count < 2U) {
        return std::nullopt;
    }
    const auto lower_priority = [&](std::uint16_t lhs, std::uint16_t rhs) {
        if (nodes[lhs].weight != nodes[rhs].weight) {
            return nodes[lhs].weight > nodes[rhs].weight;
        }
        return nodes[lhs].smallest_symbol > nodes[rhs].smallest_symbol;
    };
    std::vector<std::uint16_t> heap_storage;
    heap_storage.reserve(256U);
    std::priority_queue<std::uint16_t, std::vector<std::uint16_t>, decltype(lower_priority)> heap(
        lower_priority, std::move(heap_storage));
    for (std::uint16_t symbol = 0U; symbol < 256U; ++symbol) {
        if (!complete_sample || histogram[symbol] != 0U) {
            heap.push(symbol);
        }
    }
    for (std::uint16_t parent = 256U; parent < 256U + active_count - 1U; ++parent) {
        const auto left = heap.top();
        heap.pop();
        const auto right = heap.top();
        heap.pop();
        nodes[left].parent = parent;
        nodes[right].parent = parent;
        nodes[parent].weight = nodes[left].weight + nodes[right].weight;
        nodes[parent].smallest_symbol = std::min(nodes[left].smallest_symbol, nodes[right].smallest_symbol);
        heap.push(parent);
    }

    std::array<std::uint16_t, kGpuHuffmanLookupBits + 1U> width_counts{};
    for (std::uint16_t symbol = 0U; symbol < 256U; ++symbol) {
        if (complete_sample && histogram[symbol] == 0U) {
            continue;
        }
        std::uint16_t width = 0U;
        for (auto node = symbol; nodes[node].parent != 0xFFFFU; node = nodes[node].parent) {
            ++width;
        }
        if (width == 0U || width > kGpuHuffmanLookupBits) {
            return std::nullopt;
        }
        table.width[symbol] = static_cast<std::uint8_t>(width);
        ++width_counts[width];
    }
    std::array<std::uint16_t, kGpuHuffmanLookupBits + 1U> next_code{};
    std::uint32_t code = 0U;
    for (std::uint32_t width = 1U; width <= kGpuHuffmanLookupBits; ++width) {
        code = (code + width_counts[width - 1U]) << 1U;
        next_code[width] = static_cast<std::uint16_t>(code);
    }
    AdaptiveCodebookEstimate estimate;
    for (std::uint16_t symbol = 0U; symbol < 256U; ++symbol) {
        if (complete_sample && histogram[symbol] == 0U) {
            continue;
        }
        const auto width = table.width[symbol];
        const auto canonical = next_code[width]++;
        std::uint16_t reversed = 0U;
        for (std::uint32_t bit = 0U; bit < width; ++bit) {
            reversed = static_cast<std::uint16_t>((reversed << 1U) | ((canonical >> bit) & 1U));
        }
        table.code[symbol] = reversed;
        estimate.sampled_bits += histogram[symbol] * width;
        estimate.sample_count += histogram[symbol];
    }
    return estimate;
}

// Purpose: Serialize only a winning Huffman table into the unchanged version-eight lookup layout.
// Inputs: Internally generated least-significant-bit-first codes and bounded widths (zero denotes absent symbols).
// Outputs: Returns a complete decoder lookup; throws before packing on overlapping, incomplete, or invalid leaves.
std::vector<std::byte> serialize_huffman_lookup(const AdaptiveEncodeTable& table) {
    std::vector<std::byte> lookup(kGpuHuffmanLookupBytes, std::byte{0});
    for (std::uint32_t symbol = 0U; symbol < 256U; ++symbol) {
        const auto width = table.width[symbol];
        if (width == 0U) {
            continue;
        }
        if (width > kGpuHuffmanLookupBits) {
            throw GpuError("GPU Huffman code width exceeds lookup limits");
        }
        for (std::uint32_t slot = table.code[symbol]; slot < kGpuHuffmanLookupEntries; slot += 1U << width) {
            const auto offset = slot * sizeof(std::uint16_t);
            if (lookup[offset + 1U] != std::byte{0}) {
                throw GpuError("GPU Huffman code tree has overlapping leaves");
            }
            lookup[offset] = static_cast<std::byte>(symbol);
            lookup[offset + 1U] = static_cast<std::byte>(width);
        }
    }
    if (!huffman_lookup_is_complete(lookup)) {
        throw GpuError("Generated GPU Huffman lookup is invalid");
    }
    return lookup;
}

// Purpose: Return the expected block byte range for one verified descriptor.
// Inputs: input_size bounds bytes, block_size caps each block, cursor is its offset, and block supplies its length.
// Outputs: Returns the block start; throws if verified metadata no longer matches the chunk layout.
std::size_t checked_block_start(std::size_t input_size, std::uint32_t block_size, std::size_t& cursor,
                                const BlockDescriptor& block) {
    const auto start = cursor;
    if (start > input_size || block.uncompressed_len > input_size - start || block.uncompressed_len > block_size) {
        throw GpuError("GPU adaptive prefix source block exceeds uploaded chunk");
    }
    cursor += block.uncompressed_len;
    return start;
}

// Purpose: Append an unmodified verified non-prefix block to a rebuilt encoded chunk.
// Inputs: `out`, `source_block`, `input`, `block_start`, and `payload_offset` describe the fallback block.
// Outputs: Mutates `out` and `payload_offset` with raw/fill/pattern payload bytes in descriptor order.
void append_fallback_block(EncodedChunk& out, const BlockDescriptor& source_block, std::span<const std::byte> input,
                           std::size_t block_start, std::uint64_t& payload_offset) {
    const auto len = static_cast<std::size_t>(source_block.uncompressed_len);
    if (source_block.kind == BlockKind::Fill) {
        out.blocks.push_back(BlockDescriptor{.kind = BlockKind::Fill,
                                             .fill_value = source_block.fill_value,
                                             .uncompressed_len = source_block.uncompressed_len,
                                             .encoded_offset = payload_offset,
                                             .encoded_len = 0});
        return;
    }
    if (source_block.kind == BlockKind::Pattern) {
        const auto period = static_cast<std::size_t>(source_block.encoded_len);
        if (period == 0 || period > len) {
            throw GpuError("GPU adaptive prefix fallback pattern block is invalid");
        }
        out.blocks.push_back(BlockDescriptor{.kind = BlockKind::Pattern,
                                             .fill_value = 0,
                                             .uncompressed_len = source_block.uncompressed_len,
                                             .encoded_offset = payload_offset,
                                             .encoded_len = source_block.encoded_len});
        out.payload.insert(out.payload.end(), input.begin() + static_cast<std::ptrdiff_t>(block_start),
                           input.begin() + static_cast<std::ptrdiff_t>(block_start + period));
        payload_offset += period;
        return;
    }
    if (source_block.kind != BlockKind::Raw) {
        throw GpuError("GPU adaptive prefix fallback block kind is not supported");
    }
    out.blocks.push_back(BlockDescriptor{.kind = BlockKind::Raw,
                                         .fill_value = 0,
                                         .uncompressed_len = source_block.uncompressed_len,
                                         .encoded_offset = payload_offset,
                                         .encoded_len = source_block.uncompressed_len});
    out.payload.insert(out.payload.end(), input.begin() + static_cast<std::ptrdiff_t>(block_start),
                       input.begin() + static_cast<std::ptrdiff_t>(block_start + len));
    payload_offset += len;
}

// Purpose: Admit one distinct entropy table without serializing a losing Huffman lookup or packing a payload.
// Inputs: Bounded block, effort 2-9, codec kind, shared sample, mutable plan, and contiguous code tables.
// Outputs: Appends an admitted unique candidate; leaves the plan unchanged for unhelpful or duplicate tables.
void append_entropy_candidate(std::span<const std::byte> block, int level, bool huffman, AdaptiveEncodeBlockPlan& plan,
                              std::vector<AdaptiveEncodeTable>& code_tables,
                              const std::array<std::uint64_t, 256>& histogram, bool complete_sample) {
    const auto header_bytes = huffman ? kGpuHuffmanLookupBytes : kGpuAdaptivePrefixCodebookBytes;
    const auto overhead = header_bytes + (static_cast<std::size_t>(plan.segment_count) + 1U) * sizeof(std::uint32_t);
    if (overhead >= plan.payload_limit) {
        return;
    }
    AdaptiveEncodeTable table{};
    auto estimate = huffman ? build_huffman_prefix_codebook(histogram, complete_sample, table)
                            : std::optional(build_adaptive_prefix_codebook(histogram, table));
    if (!estimate) {
        return;
    }
    const auto estimated_bits = estimate->sampled_bits * block.size() / estimate->sample_count;
    if ((huffman || level < 7) && overhead + (estimated_bits + 7U) / 8U >= plan.payload_limit) {
        return;
    }
    const auto kind = huffman ? BlockKind::GpuHuffman : BlockKind::GpuAdaptivePrefix;
    for (const auto& candidate : plan.candidates) {
        const auto& previous = code_tables[candidate.table_index];
        // Equal widths and equal header sizes have identical measured cost; retain the earlier table on ties.
        if (candidate.kind == kind &&
            std::equal(std::begin(table.width), std::end(table.width), std::begin(previous.width))) {
            return;
        }
    }
    if (plan.candidates.size() >= kMaxEntropyCandidates ||
        code_tables.size() >= std::numeric_limits<std::uint32_t>::max()) {
        throw GpuError("GPU entropy candidate count exceeds its bounded plan");
    }
    plan.candidates.push_back(EntropyEncodeCandidate{.kind = kind,
                                                     .table_index = static_cast<std::uint32_t>(code_tables.size()),
                                                     .codebook = std::move(estimate->codebook)});
    code_tables.push_back(table);
}

// Purpose: Make compression efforts nested searches rather than replacements of lower-effort tables.
// Inputs: One eligible block, immutable static-baseline kind, maximum effort, and mutable candidate/table lists.
// Outputs: Adds at most sixteen unique candidates; both codecs reuse nested samples counted only once.
void build_entropy_candidates(std::span<const std::byte> block, BlockKind baseline_kind, int level,
                              AdaptiveEncodeBlockPlan& plan, std::vector<AdaptiveEncodeTable>& code_tables) {
    const auto try_low_adaptive = should_try_sampled_adaptive_prefix(block, 2);
    std::array<std::size_t, 3> previous_samples{};
    EntropyHistogramSampler sampler(block);
    const auto offset_bytes = (static_cast<std::size_t>(plan.segment_count) + 1U) * sizeof(std::uint32_t);
    for (int effort = 2; effort <= level; ++effort) {
        const auto samples = entropy_sample_count(block.size(), effort);
        const auto phase = effort < 7 ? 0U : 1U;
        const bool adaptive = (try_low_adaptive || effort >= 7) && previous_samples[phase] != samples &&
                              kGpuAdaptivePrefixCodebookBytes + offset_bytes < plan.payload_limit;
        const bool huffman = (effort != 2 || baseline_kind == BlockKind::GpuPrefix) && previous_samples[2] != samples &&
                             kGpuHuffmanLookupBytes + offset_bytes < plan.payload_limit;
        if (!adaptive && !huffman) {
            continue;
        }
        const auto& histogram = sampler.sample_to(samples);
        const bool complete_sample = samples == block.size();
        if (adaptive) {
            append_entropy_candidate(block, effort, false, plan, code_tables, histogram, complete_sample);
            previous_samples[phase] = samples;
        }
        if (huffman) {
            append_entropy_candidate(block, effort, true, plan, code_tables, histogram, complete_sample);
            previous_samples[2] = samples;
        }
    }
}

// Purpose: Build one length-work item per source segment, sharing all nested entropy candidates.
// Inputs: Verified source blocks, immutable static baseline, maximum effort, and mutable device-work lists.
// Outputs: Returns bounded per-block candidates and dense per-segment length-output windows.
std::vector<AdaptiveEncodeBlockPlan> build_adaptive_encode_plans(std::span<const std::byte> input,
                                                                 std::uint32_t block_size,
                                                                 std::span<const BlockDescriptor> source_blocks,
                                                                 const EncodedChunk* baseline, int compression_level,
                                                                 std::vector<EntropyLengthSegmentPlan>& segment_plans,
                                                                 std::vector<AdaptiveEncodeTable>& code_tables) {
    std::vector<AdaptiveEncodeBlockPlan> block_plans;
    if (baseline && baseline->blocks.size() != source_blocks.size()) {
        throw GpuError("GPU adaptive baseline block count differs from source");
    }
    block_plans.reserve(source_blocks.size());
    std::size_t cursor = 0;
    std::uint32_t length_count = 0U;
    for (std::uint32_t block_index = 0; block_index < source_blocks.size(); ++block_index) {
        const auto& source_block = source_blocks[block_index];
        const auto start = checked_block_start(input.size(), block_size, cursor, source_block);
        const auto len = source_block.uncompressed_len;
        const auto& previous = baseline ? baseline->blocks[block_index] : source_block;
        if (previous.uncompressed_len != len) {
            throw GpuError("GPU adaptive baseline block length differs from source");
        }
        const bool eligible = source_block.kind == BlockKind::Raw && len >= kGpuPrefixSegmentBytes;
        const auto segment_count = eligible ? (len + kGpuPrefixSegmentBytes - 1U) / kGpuPrefixSegmentBytes : 0U;
        AdaptiveEncodeBlockPlan block_plan{
            .block_start = start,
            .block_len = len,
            .payload_limit = previous.encoded_len,
            .segment_offset = length_count,
            .segment_count = len >= kGpuPrefixSegmentBytes ? segment_count : 0U,
            .table_index = static_cast<std::uint32_t>(code_tables.size()),
        };
        if (block_plan.segment_count != 0U) {
            build_entropy_candidates(input.subspan(start, len), previous.kind, compression_level, block_plan,
                                     code_tables);
            const auto count = static_cast<std::uint32_t>(block_plan.candidates.size());
            if (count == 0U) {
                block_plan.segment_count = 0U;
            } else {
                for (std::uint32_t segment = 0; segment < block_plan.segment_count; ++segment) {
                    segment_plans.push_back(EntropyLengthSegmentPlan{
                        .block_start = static_cast<std::uint64_t>(start),
                        .block_len = len,
                        .segment_index = segment,
                        .table_index = block_plan.table_index,
                        .candidate_count = count,
                        .length_offset = length_count,
                    });
                    length_count = checked_prefix_offset_add(length_count, count, "GPU entropy length output count");
                }
            }
        }
        block_plans.push_back(std::move(block_plan));
    }
    if (cursor != input.size()) {
        throw GpuError("GPU entropy source blocks do not cover the uploaded chunk");
    }
    return block_plans;
}

// Purpose: Select the smallest register/shared-memory specialization for this batch's candidate count.
// Inputs: Device buffers, segment count, maximum candidates (1-16), and an exclusive borrowed event pair.
// The pair's previous interval must be complete and collected before this launch.
// Outputs: Launches exactly one coalesced length kernel on the existing per-thread stream.
void launch_entropy_length_batch(const std::byte* input, const EntropyLengthSegmentPlan* plans,
                                 const AdaptiveEncodeTable* tables, std::uint32_t* lengths, std::uint32_t segments,
                                 std::uint32_t candidates, const HipEventPair& events) {
    if (candidates <= 2U) {
        launch_measured_kernel(entropy_segment_lengths_batch_kernel<2U>, segments, kGpuPrefixSegmentThreads, 0,
                               hipStreamPerThread, events, "launch entropy segment lengths", input, plans, tables,
                               lengths, segments);
    } else if (candidates <= 4U) {
        launch_measured_kernel(entropy_segment_lengths_batch_kernel<4U>, segments, kGpuPrefixSegmentThreads, 0,
                               hipStreamPerThread, events, "launch entropy segment lengths", input, plans, tables,
                               lengths, segments);
    } else if (candidates <= 8U) {
        launch_measured_kernel(entropy_segment_lengths_batch_kernel<8U>, segments, kGpuPrefixSegmentThreads, 0,
                               hipStreamPerThread, events, "launch entropy segment lengths", input, plans, tables,
                               lengths, segments);
    } else {
        launch_measured_kernel(entropy_segment_lengths_batch_kernel<16U>, segments, kGpuPrefixSegmentThreads, 0,
                               hipStreamPerThread, events, "launch entropy segment lengths", input, plans, tables,
                               lengths, segments);
    }
}

// Purpose: Compute adaptive-prefix encoded segment byte lengths on the AMD GPU.
// Inputs: `device_input`, `segment_plans`, `code_tables`, and `telemetry` describe one uploaded chunk.
// `events` is an exclusive borrowed pair; this pass completes and collects its interval before returning.
// Outputs: Returns an interleaved encoded byte length for each segment/candidate pair.
std::vector<std::uint32_t> compute_adaptive_prefix_lengths_batch_device(
    const std::byte* device_input, std::span<const EntropyLengthSegmentPlan> segment_plans,
    std::span<const AdaptiveEncodeTable> code_tables, GpuTelemetry* telemetry, const HipEventPair& events) {
    if (segment_plans.empty()) {
        return {};
    }
    const auto output_count = checked_prefix_offset_add(
        segment_plans.back().length_offset, segment_plans.back().candidate_count, "GPU entropy length count");
    std::vector<std::uint32_t> segment_lengths(output_count);
    if (segment_plans.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw GpuError("GPU adaptive prefix segment count exceeds HIP launch limits");
    }
    const auto plan_bytes =
        checked_multiply_bytes(segment_plans.size(), sizeof(EntropyLengthSegmentPlan), "GPU entropy length plans");
    const auto table_bytes =
        checked_multiply_bytes(code_tables.size(), sizeof(AdaptiveEncodeTable), "GPU adaptive prefix tables");
    const auto length_bytes =
        checked_multiply_bytes(segment_lengths.size(), sizeof(std::uint32_t), "GPU entropy lengths");
    auto required_bytes = checked_add_bytes(plan_bytes, table_bytes, "GPU adaptive prefix length memory");
    required_bytes = checked_add_bytes(required_bytes, length_bytes, "GPU adaptive prefix length memory");
    HipDeviceMemoryReservation reservation(required_bytes, "GPU adaptive prefix length batch");
    HipDeviceBuffer<EntropyLengthSegmentPlan> device_plans(plan_bytes, "hipMalloc entropy length plans");
    HipDeviceBuffer<AdaptiveEncodeTable> device_tables(table_bytes, "hipMalloc adaptive prefix length tables");
    HipDeviceBuffer<std::uint32_t> device_lengths(length_bytes, "hipMalloc adaptive prefix segment lengths");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(required_bytes));
    check_hip(copy_on_codec_stream(device_plans.get(), segment_plans.data(), plan_bytes, hipMemcpyHostToDevice),
              "hipMemcpy adaptive prefix length plans");
    check_hip(copy_on_codec_stream(device_tables.get(), code_tables.data(), table_bytes, hipMemcpyHostToDevice),
              "hipMemcpy adaptive prefix length tables");
    record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(plan_bytes + table_bytes));
    std::uint32_t maximum_candidates = 0U;
    for (const auto& plan : segment_plans) {
        maximum_candidates = std::max(maximum_candidates, plan.candidate_count);
    }
    if (maximum_candidates == 0U || maximum_candidates > kMaxEntropyCandidates) {
        throw GpuError("GPU entropy length batch exceeds candidate limits");
    }
    launch_entropy_length_batch(device_input, device_plans.get(), device_tables.get(), device_lengths.get(),
                                static_cast<std::uint32_t>(segment_plans.size()), maximum_candidates, events);
    finish_measured_kernel(telemetry, events, "synchronize entropy segment lengths");
    check_hip(copy_on_codec_stream(segment_lengths.data(), device_lengths.get(), length_bytes, hipMemcpyDeviceToHost),
              "hipMemcpy adaptive prefix segment lengths");
    record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(length_bytes));
    device_plans.reset_checked("hipFree adaptive prefix length plans");
    device_tables.reset_checked("hipFree adaptive prefix length tables");
    device_lengths.reset_checked("hipFree adaptive prefix segment lengths");
    return segment_lengths;
}

// Purpose: Choose one measured candidate for a block without losing a smaller lower-effort representation.
// Inputs: Nested candidates, exact interleaved GPU segment lengths, and matching encode tables.
// Outputs: Sets the winning kind, offsets, and serialized codebook only for a strict complete-payload saving.
bool select_entropy_block(AdaptiveEncodeBlockPlan& plan, std::span<const std::uint32_t> lengths,
                          std::span<const AdaptiveEncodeTable> tables) {
    auto best_bytes = static_cast<std::uint64_t>(plan.payload_limit);
    auto winner = plan.candidates.size();
    std::vector<std::uint32_t> offsets(static_cast<std::size_t>(plan.segment_count) + 1U, 0U);
    for (std::size_t candidate = 0U; candidate < plan.candidates.size(); ++candidate) {
        for (std::uint32_t segment = 0U; segment < plan.segment_count; ++segment) {
            const auto index =
                static_cast<std::size_t>(plan.segment_offset) + segment * plan.candidates.size() + candidate;
            if (index >= lengths.size()) {
                throw GpuError("GPU entropy measured lengths exceed their output table");
            }
            offsets[segment + 1U] =
                checked_prefix_offset_add(offsets[segment], lengths[index], "GPU entropy block size");
        }
        const auto header = plan.candidates[candidate].kind == BlockKind::GpuHuffman ? kGpuHuffmanLookupBytes
                                                                                     : kGpuAdaptivePrefixCodebookBytes;
        const auto payload_bytes = header + offsets.size() * sizeof(std::uint32_t) + offsets.back();
        if (offsets.back() != 0U && payload_bytes < best_bytes) {
            best_bytes = payload_bytes;
            winner = candidate;
            plan.offsets = offsets;
        }
    }
    if (winner == plan.candidates.size()) {
        return false;
    }
    auto& selected = plan.candidates[winner];
    if (selected.table_index >= tables.size()) {
        throw GpuError("GPU entropy selected table exceeds its bounded table list");
    }
    plan.kind = selected.kind;
    plan.table_index = selected.table_index;
    plan.codebook = selected.kind == BlockKind::GpuHuffman ? serialize_huffman_lookup(tables[selected.table_index])
                                                           : std::move(selected.codebook);
    plan.use_adaptive = true;
    plan.bitstream_bytes = plan.offsets.back();
    return true;
}

// Purpose: Select entropy winners and build one combined pack plan with no losing encode tables.
// Inputs: Mutable block plans, exact GPU lengths, and internally generated candidate tables.
// Outputs: Packs only strict complete-payload improvements; ties preserve lower-effort and baseline bytes.
AdaptiveBatchSelection select_adaptive_blocks_for_batch(std::vector<AdaptiveEncodeBlockPlan>& block_plans,
                                                        std::span<const std::uint32_t> segment_lengths,
                                                        std::span<const AdaptiveEncodeTable> code_tables) {
    AdaptiveBatchSelection selection;
    for (auto& block_plan : block_plans) {
        if (block_plan.segment_count == 0U || !select_entropy_block(block_plan, segment_lengths, code_tables)) {
            continue;
        }
        const auto pack_table_index = static_cast<std::uint32_t>(selection.pack_tables.size());
        selection.pack_tables.push_back(code_tables[block_plan.table_index]);
        block_plan.bitstream_offset = selection.bitstream_bytes;
        ++selection.adaptive_blocks;
        for (std::uint32_t segment = 0; segment < block_plan.segment_count; ++segment) {
            selection.pack_plans.push_back(AdaptiveEncodeSegmentPlan{
                .block_start = static_cast<std::uint64_t>(block_plan.block_start),
                .block_len = block_plan.block_len,
                .segment_index = segment,
                .table_index = pack_table_index,
            });
            selection.pack_offsets.push_back(checked_prefix_offset_add(
                selection.bitstream_bytes, block_plan.offsets[segment], "GPU adaptive prefix packed payload"));
        }
        selection.bitstream_bytes = checked_prefix_offset_add(selection.bitstream_bytes, block_plan.bitstream_bytes,
                                                              "GPU adaptive prefix combined payload");
    }
    return selection;
}

// Purpose: Pack all selected adaptive-prefix segments into one combined device buffer.
// Inputs: Uploaded bytes, selected segments/tables, and operation-owned telemetry.
// `events` is an exclusive borrowed pair whose preceding interval has already completed and been collected.
// Outputs: Returns the combined encoded bitstream for all selected adaptive-prefix blocks.
std::vector<std::byte> pack_adaptive_prefix_segments_batch_device(const std::byte* device_input,
                                                                  const AdaptiveBatchSelection& selection,
                                                                  GpuTelemetry* telemetry, const HipEventPair& events) {
    std::vector<std::byte> bitstream(selection.bitstream_bytes);
    if (selection.pack_plans.empty()) {
        return bitstream;
    }
    if (selection.pack_plans.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw GpuError("GPU adaptive prefix pack segment count exceeds HIP launch limits");
    }
    const auto plan_bytes = checked_multiply_bytes(selection.pack_plans.size(), sizeof(AdaptiveEncodeSegmentPlan),
                                                   "GPU adaptive prefix pack plans");
    const auto table_bytes =
        checked_multiply_bytes(selection.pack_tables.size(), sizeof(AdaptiveEncodeTable), "GPU entropy pack tables");
    const auto offset_bytes =
        checked_multiply_bytes(selection.pack_offsets.size(), sizeof(std::uint32_t), "GPU adaptive prefix offsets");
    auto required_bytes = checked_add_bytes(plan_bytes, table_bytes, "GPU adaptive prefix pack memory");
    required_bytes = checked_add_bytes(required_bytes, offset_bytes, "GPU adaptive prefix pack memory");
    required_bytes = checked_add_bytes(required_bytes, bitstream.size(), "GPU adaptive prefix pack memory");
    HipDeviceMemoryReservation reservation(required_bytes, "GPU adaptive prefix pack batch");
    HipDeviceBuffer<AdaptiveEncodeSegmentPlan> device_plans(plan_bytes, "hipMalloc adaptive prefix pack plans");
    HipDeviceBuffer<AdaptiveEncodeTable> device_tables(table_bytes, "hipMalloc adaptive prefix pack tables");
    HipDeviceBuffer<std::uint32_t> device_offsets(offset_bytes, "hipMalloc adaptive prefix pack offsets");
    HipDeviceBuffer<std::byte> device_encoded(bitstream.size(), "hipMalloc adaptive prefix payload");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(required_bytes));
    check_hip(copy_on_codec_stream(device_plans.get(), selection.pack_plans.data(), plan_bytes, hipMemcpyHostToDevice),
              "hipMemcpy adaptive prefix pack plans");
    check_hip(
        copy_on_codec_stream(device_tables.get(), selection.pack_tables.data(), table_bytes, hipMemcpyHostToDevice),
        "hipMemcpy adaptive prefix pack tables");
    check_hip(
        copy_on_codec_stream(device_offsets.get(), selection.pack_offsets.data(), offset_bytes, hipMemcpyHostToDevice),
        "hipMemcpy adaptive prefix pack offsets");
    record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(plan_bytes + table_bytes + offset_bytes));
    check_hip(hipMemsetAsync(device_encoded.get(), 0, bitstream.size(), hipStreamPerThread),
              "hipMemset adaptive prefix payload");
    launch_measured_kernel(adaptive_prefix_pack_segments_batch_kernel,
                           static_cast<unsigned int>(selection.pack_plans.size()), kGpuPrefixSegmentThreads, 0,
                           hipStreamPerThread, events, "launch adaptive_prefix_pack_segments_batch_kernel",
                           device_input, device_plans.get(), device_tables.get(), device_offsets.get(),
                           device_encoded.get(), static_cast<std::uint32_t>(selection.pack_plans.size()));
    finish_measured_kernel(telemetry, events, "synchronize adaptive_prefix_pack_segments_batch_kernel");
    check_hip(copy_on_codec_stream(bitstream.data(), device_encoded.get(), bitstream.size(), hipMemcpyDeviceToHost),
              "hipMemcpy adaptive prefix payload");
    record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(bitstream.size()));
    device_plans.reset_checked("hipFree adaptive prefix pack plans");
    device_tables.reset_checked("hipFree adaptive prefix pack tables");
    device_offsets.reset_checked("hipFree adaptive prefix pack offsets");
    device_encoded.reset_checked("hipFree adaptive prefix payload");
    return bitstream;
}

// Purpose: Append one selected adaptive-prefix payload codebook, table, and bitstream slice.
// Inputs: `out`, `block_plan`, and `bitstream` describe a selected adaptive-prefix block.
// Outputs: Mutates `out.payload` with codebook bytes, block-local offset table, and encoded bytes.
void append_adaptive_prefix_payload(EncodedChunk& out, const AdaptiveEncodeBlockPlan& block_plan,
                                    std::span<const std::byte> bitstream) {
    out.payload.insert(out.payload.end(), block_plan.codebook.begin(), block_plan.codebook.end());
    for (const auto offset : block_plan.offsets) {
        append_prefix_u32(out.payload, offset);
    }
    const auto start = static_cast<std::size_t>(block_plan.bitstream_offset);
    const auto end = start + static_cast<std::size_t>(block_plan.bitstream_bytes);
    out.payload.insert(out.payload.end(), bitstream.begin() + static_cast<std::ptrdiff_t>(start),
                       bitstream.begin() + static_cast<std::ptrdiff_t>(end));
}

// Purpose: Preserve an already encoded block when adaptive coding cannot improve it.
// Inputs: Output chunk, immutable baseline, valid block index, and running destination payload offset.
// Outputs: Copies the baseline block's exact bytes, rebases its offset, and throws on inconsistent payload bounds.
void append_baseline_block(EncodedChunk& out, const EncodedChunk& baseline, std::size_t index,
                           std::uint64_t& payload_offset) {
    auto block = baseline.blocks[index];
    if (block.encoded_offset > baseline.payload.size() ||
        block.encoded_len > baseline.payload.size() - block.encoded_offset) {
        throw GpuError("GPU adaptive baseline block exceeds its payload");
    }
    const auto bytes = std::span<const std::byte>(baseline.payload)
                           .subspan(static_cast<std::size_t>(block.encoded_offset), block.encoded_len);
    block.encoded_offset = payload_offset;
    out.blocks.push_back(block);
    out.payload.insert(out.payload.end(), bytes.begin(), bytes.end());
    payload_offset += block.encoded_len;
}

}  // namespace

// Purpose: Replace only blocks whose selected entropy payload improves the GPU-native baseline.
// Inputs: Uploaded and host bytes, verified descriptors, immutable static baseline, level, and mutable HIP telemetry.
// Outputs: Returns smaller replacements or empty; owns one timing pair across sequential completed passes.
std::optional<EncodedChunk>
encode_entropy_prefix_chunk_device(const std::byte* device_input, std::span<const std::byte> input,
                                   std::uint32_t block_size, std::span<const BlockDescriptor> source_blocks,
                                   const EncodedChunk* baseline, int compression_level, GpuTelemetry* telemetry) {
    std::vector<EntropyLengthSegmentPlan> length_plans;
    std::vector<AdaptiveEncodeTable> code_tables;
    auto block_plans = build_adaptive_encode_plans(input, block_size, source_blocks, baseline, compression_level,
                                                   length_plans, code_tables);
    if (length_plans.empty()) {
        return std::nullopt;
    }
    const auto events = make_hip_event_pair("create entropy prefix encode events");
    const auto segment_lengths =
        compute_adaptive_prefix_lengths_batch_device(device_input, length_plans, code_tables, telemetry, events);
    auto selection = select_adaptive_blocks_for_batch(block_plans, segment_lengths, code_tables);
    if (selection.adaptive_blocks == 0U) {
        return std::nullopt;
    }
    auto bitstream = pack_adaptive_prefix_segments_batch_device(device_input, selection, telemetry, events);
    EncodedChunk out;
    out.blocks.reserve(source_blocks.size());
    std::size_t payload_bytes = 0U;
    for (const auto& plan : block_plans) {
        const auto bytes = plan.use_adaptive ? plan.codebook.size() + plan.offsets.size() * sizeof(std::uint32_t) +
                                                   plan.bitstream_bytes
                                             : plan.payload_limit;
        payload_bytes = checked_add_bytes(payload_bytes, bytes, "GPU entropy selected payload");
    }
    out.payload.reserve(payload_bytes);
    std::uint64_t payload_offset = 0;
    for (std::uint32_t block_index = 0; block_index < source_blocks.size(); ++block_index) {
        const auto& block_plan = block_plans[block_index];
        const auto& source_block = source_blocks[block_index];
        const auto len = static_cast<std::size_t>(block_plan.block_len);
        if (block_plan.use_adaptive) {
            const auto table_bytes = checked_multiply_bytes(block_plan.offsets.size(), sizeof(std::uint32_t),
                                                            "GPU adaptive prefix block table");
            auto prefix_payload_size =
                checked_add_bytes(block_plan.codebook.size(), table_bytes, "GPU adaptive prefix payload");
            prefix_payload_size =
                checked_add_bytes(prefix_payload_size, block_plan.bitstream_bytes, "GPU adaptive prefix payload");
            if (prefix_payload_size > std::numeric_limits<std::uint32_t>::max()) {
                throw GpuError("GPU adaptive prefix block exceeds block metadata limit");
            }
            out.blocks.push_back(BlockDescriptor{.kind = block_plan.kind,
                                                 .fill_value = 0,
                                                 .uncompressed_len = static_cast<std::uint32_t>(len),
                                                 .encoded_offset = payload_offset,
                                                 .encoded_len = static_cast<std::uint32_t>(prefix_payload_size)});
            append_adaptive_prefix_payload(out, block_plan, bitstream);
            payload_offset += prefix_payload_size;
        } else if (baseline) {
            append_baseline_block(out, *baseline, block_index, payload_offset);
        } else {
            append_fallback_block(out, source_block, input, block_plan.block_start, payload_offset);
        }
    }
    return out;
}

}  // namespace superzip::hip_detail
