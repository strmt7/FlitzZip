#include "gpu/hip_codec_support.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <numeric>
#include <optional>
#include <vector>
#include <hip/hip_runtime.h>

namespace superzip::hip_detail {
namespace {

struct AdaptiveEncodeTable {
    std::uint16_t code[256];
    std::uint8_t width[256];
};

struct AdaptiveEncodeSegmentPlan {
    std::uint64_t block_start;
    std::uint32_t block_len;
    std::uint32_t segment_index;
    std::uint32_t table_index;
};

struct AdaptiveEncodeBlockPlan {
    std::size_t block_start = 0;
    std::uint32_t block_len = 0;
    std::uint32_t segment_offset = 0;
    std::uint32_t segment_count = 0;
    std::uint32_t table_index = 0;
    bool use_adaptive = false;
    std::uint32_t bitstream_offset = 0;
    std::uint32_t bitstream_bytes = 0;
    std::array<std::byte, kGpuAdaptivePrefixCodebookBytes> codebook{};
    std::vector<std::uint32_t> offsets;
};

struct AdaptiveBatchSelection {
    std::uint64_t adaptive_blocks = 0;
    std::uint32_t bitstream_bytes = 0;
    std::vector<AdaptiveEncodeSegmentPlan> pack_plans;
    std::vector<std::uint32_t> pack_offsets;
};

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

// Purpose: Compute adaptive-prefix encoded byte counts for many segments in one HIP launch.
// Inputs: `input`, `plans`, and `tables` describe uploaded bytes and per-block adaptive code tables.
// Outputs: Writes 4-byte-aligned encoded segment byte lengths.
__global__ void adaptive_prefix_segment_lengths_batch_kernel(const std::byte* input,
                                                             const AdaptiveEncodeSegmentPlan* plans,
                                                             const AdaptiveEncodeTable* tables,
                                                             std::uint32_t* segment_lengths,
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
    sums[thread_id] =
        gpu_adaptive_prefix_range_bit_count(input, table, static_cast<std::size_t>(plan.block_start), start, end);
    __syncthreads();
    for (std::uint32_t offset = kGpuPrefixSegmentThreads / 2U; offset > 0U; offset >>= 1U) {
        if (thread_id < offset) {
            sums[thread_id] += sums[thread_id + offset];
        }
        __syncthreads();
    }
    if (thread_id == 0U) {
        const auto bytes = (sums[0] + 7U) / 8U;
        segment_lengths[segment] = (bytes + 3U) & ~3U;
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
// Inputs: `block` is the host block sample/full span and `compression_level` controls sampling effort.
// Outputs: Returns the serialized codebook and fills `table` with device-ready codes and widths.
std::array<std::byte, kGpuAdaptivePrefixCodebookBytes>
build_adaptive_prefix_codebook(std::span<const std::byte> block, int compression_level, AdaptiveEncodeTable& table) {
    std::array<std::uint64_t, 256> histogram{};
    const auto target_samples = compression_level >= 7 ? block.size() : std::min<std::size_t>(block.size(), 65536U);
    const auto stride = target_samples == 0U ? 1U : std::max<std::size_t>(1U, block.size() / target_samples);
    for (std::size_t i = 0; i < block.size(); i += stride) {
        ++histogram[static_cast<std::uint8_t>(block[i])];
    }
    std::array<std::uint16_t, 256> order{};
    std::iota(order.begin(), order.end(), static_cast<std::uint16_t>(0));
    std::stable_sort(order.begin(), order.end(), [&](std::uint16_t lhs, std::uint16_t rhs) {
        if (histogram[lhs] != histogram[rhs]) {
            return histogram[lhs] > histogram[rhs];
        }
        return lhs < rhs;
    });

    std::array<std::byte, kGpuAdaptivePrefixCodebookBytes> codebook{};
    for (std::size_t i = 0; i < codebook.size(); ++i) {
        codebook[i] = static_cast<std::byte>(order[i]);
    }
    for (std::uint32_t value = 0; value < 256U; ++value) {
        table.code[value] = static_cast<std::uint16_t>(0x7U | (value << 3U));
        table.width[value] = 11U;
    }
    for (std::uint32_t rank = 0; rank < kGpuAdaptivePrefixCodebookBytes; ++rank) {
        const auto value = static_cast<std::uint8_t>(codebook[rank]);
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
    return codebook;
}

// Purpose: Return the expected block byte range for one verified descriptor.
// Inputs: `input_size`, `block_size`, `block_index`, and `block` describe one dense block in the chunk.
// Outputs: Returns the block start; throws if verified metadata no longer matches the chunk layout.
std::size_t checked_block_start(std::size_t input_size, std::uint32_t block_size, std::uint32_t block_index,
                                const BlockDescriptor& block) {
    const auto start = static_cast<std::size_t>(block_index) * block_size;
    if (start > input_size || block.uncompressed_len > input_size - start) {
        throw GpuError("GPU adaptive prefix source block exceeds uploaded chunk");
    }
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

// Purpose: Build host adaptive-prefix plans and device code tables for verified raw blocks inside one uploaded chunk.
// Inputs: `input`, `block_size`, `source_blocks`, and `compression_level` describe one bounded archive chunk.
// Outputs: Returns block plans while filling segment plans and code tables.
std::vector<AdaptiveEncodeBlockPlan> build_adaptive_encode_plans(std::span<const std::byte> input,
                                                                 std::uint32_t block_size,
                                                                 std::span<const BlockDescriptor> source_blocks,
                                                                 int compression_level,
                                                                 std::vector<AdaptiveEncodeSegmentPlan>& segment_plans,
                                                                 std::vector<AdaptiveEncodeTable>& code_tables) {
    std::vector<AdaptiveEncodeBlockPlan> block_plans;
    block_plans.reserve(source_blocks.size());
    for (std::uint32_t block_index = 0; block_index < source_blocks.size(); ++block_index) {
        const auto& source_block = source_blocks[block_index];
        const auto start = checked_block_start(input.size(), block_size, block_index, source_block);
        const auto len = source_block.uncompressed_len;
        const bool eligible = source_block.kind == BlockKind::Raw && len >= kGpuPrefixSegmentBytes;
        const auto segment_count = eligible ? (len + kGpuPrefixSegmentBytes - 1U) / kGpuPrefixSegmentBytes : 0U;
        AdaptiveEncodeBlockPlan block_plan{
            .block_start = start,
            .block_len = len,
            .segment_offset = static_cast<std::uint32_t>(segment_plans.size()),
            .segment_count = len >= kGpuPrefixSegmentBytes ? segment_count : 0U,
            .table_index = static_cast<std::uint32_t>(code_tables.size()),
        };
        if (block_plan.segment_count != 0U) {
            AdaptiveEncodeTable table{};
            block_plan.codebook = build_adaptive_prefix_codebook(input.subspan(start, len), compression_level, table);
            code_tables.push_back(table);
            for (std::uint32_t segment = 0; segment < block_plan.segment_count; ++segment) {
                segment_plans.push_back(AdaptiveEncodeSegmentPlan{
                    .block_start = static_cast<std::uint64_t>(start),
                    .block_len = len,
                    .segment_index = segment,
                    .table_index = block_plan.table_index,
                });
            }
        }
        block_plans.push_back(std::move(block_plan));
    }
    return block_plans;
}

// Purpose: Compute adaptive-prefix encoded segment byte lengths on the AMD GPU.
// Inputs: `device_input`, `segment_plans`, `code_tables`, and `telemetry` describe one uploaded chunk.
// Outputs: Returns one encoded byte length per segment.
std::vector<std::uint32_t> compute_adaptive_prefix_lengths_batch_device(
    const std::byte* device_input, std::span<const AdaptiveEncodeSegmentPlan> segment_plans,
    std::span<const AdaptiveEncodeTable> code_tables, GpuTelemetry* telemetry) {
    std::vector<std::uint32_t> segment_lengths(segment_plans.size());
    if (segment_plans.empty()) {
        return segment_lengths;
    }
    if (segment_plans.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw GpuError("GPU adaptive prefix segment count exceeds HIP launch limits");
    }
    const auto plan_bytes =
        checked_multiply_bytes(segment_plans.size(), sizeof(AdaptiveEncodeSegmentPlan), "GPU adaptive prefix plans");
    const auto table_bytes =
        checked_multiply_bytes(code_tables.size(), sizeof(AdaptiveEncodeTable), "GPU adaptive prefix tables");
    const auto length_bytes =
        checked_multiply_bytes(segment_plans.size(), sizeof(std::uint32_t), "GPU adaptive prefix lengths");
    auto required_bytes = checked_add_bytes(plan_bytes, table_bytes, "GPU adaptive prefix length memory");
    required_bytes = checked_add_bytes(required_bytes, length_bytes, "GPU adaptive prefix length memory");
    AdaptiveEncodeSegmentPlan* device_plans = nullptr;
    AdaptiveEncodeTable* device_tables = nullptr;
    std::uint32_t* device_lengths = nullptr;
    ensure_device_memory_budget(required_bytes, "GPU adaptive prefix length batch");
    check_hip(hipMalloc(&device_plans, plan_bytes), "hipMalloc adaptive prefix length plans");
    check_hip(hipMalloc(&device_tables, table_bytes), "hipMalloc adaptive prefix length tables");
    check_hip(hipMalloc(&device_lengths, length_bytes), "hipMalloc adaptive prefix segment lengths");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(required_bytes));
    try {
        check_hip(hipMemcpy(device_plans, segment_plans.data(), plan_bytes, hipMemcpyHostToDevice),
                  "hipMemcpy adaptive prefix length plans");
        check_hip(hipMemcpy(device_tables, code_tables.data(), table_bytes, hipMemcpyHostToDevice),
                  "hipMemcpy adaptive prefix length tables");
        record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(plan_bytes + table_bytes));
        auto events = make_hip_event_pair("create adaptive_prefix_segment_lengths_batch_kernel events");
        check_hip(hipEventRecord(events.start, hipStreamPerThread),
                  "record adaptive_prefix_segment_lengths_batch_kernel start");
        adaptive_prefix_segment_lengths_batch_kernel<<<static_cast<unsigned int>(segment_plans.size()),
                                                       kGpuPrefixSegmentThreads, 0, hipStreamPerThread>>>(
            device_input, device_plans, device_tables, device_lengths,
            static_cast<std::uint32_t>(segment_plans.size()));
        check_hip(hipGetLastError(), "launch adaptive_prefix_segment_lengths_batch_kernel");
        check_hip(hipEventRecord(events.stop, hipStreamPerThread),
                  "record adaptive_prefix_segment_lengths_batch_kernel stop");
        finish_measured_kernel(telemetry, events, "synchronize adaptive_prefix_segment_lengths_batch_kernel");
        check_hip(hipMemcpy(segment_lengths.data(), device_lengths, length_bytes, hipMemcpyDeviceToHost),
                  "hipMemcpy adaptive prefix segment lengths");
        record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(length_bytes));
        check_hip(hipFree(device_plans), "hipFree adaptive prefix length plans");
        device_plans = nullptr;
        check_hip(hipFree(device_tables), "hipFree adaptive prefix length tables");
        device_tables = nullptr;
        check_hip(hipFree(device_lengths), "hipFree adaptive prefix segment lengths");
        device_lengths = nullptr;
        return segment_lengths;
    } catch (...) {
        (void)hipFree(device_plans);
        (void)hipFree(device_tables);
        (void)hipFree(device_lengths);
        throw;
    }
}

// Purpose: Select adaptive-prefix blocks and build one combined pack plan.
// Inputs: `block_plans` are mutable host plans and `segment_lengths` are GPU-measured byte counts.
// Outputs: Returns combined pack plans and offsets; mutates selected block plans with payload metadata.
AdaptiveBatchSelection select_adaptive_blocks_for_batch(std::vector<AdaptiveEncodeBlockPlan>& block_plans,
                                                        std::span<const std::uint32_t> segment_lengths) {
    AdaptiveBatchSelection selection;
    for (auto& block_plan : block_plans) {
        if (block_plan.segment_count == 0U) {
            continue;
        }
        block_plan.offsets.assign(static_cast<std::size_t>(block_plan.segment_count) + 1U, 0U);
        for (std::uint32_t segment = 0; segment < block_plan.segment_count; ++segment) {
            const auto length_index = static_cast<std::size_t>(block_plan.segment_offset) + segment;
            block_plan.offsets[segment + 1U] = checked_prefix_offset_add(
                block_plan.offsets[segment], segment_lengths[length_index], "GPU adaptive prefix encoded block size");
        }
        const auto table_bytes =
            checked_multiply_bytes(block_plan.offsets.size(), sizeof(std::uint32_t), "GPU adaptive prefix table");
        auto payload_bytes =
            checked_add_bytes(kGpuAdaptivePrefixCodebookBytes, table_bytes, "GPU adaptive prefix payload");
        payload_bytes = checked_add_bytes(payload_bytes, block_plan.offsets.back(), "GPU adaptive prefix payload");
        if (block_plan.offsets.back() == 0U || payload_bytes >= block_plan.block_len) {
            block_plan.offsets.clear();
            continue;
        }
        block_plan.use_adaptive = true;
        block_plan.bitstream_offset = selection.bitstream_bytes;
        block_plan.bitstream_bytes = block_plan.offsets.back();
        ++selection.adaptive_blocks;
        for (std::uint32_t segment = 0; segment < block_plan.segment_count; ++segment) {
            selection.pack_plans.push_back(AdaptiveEncodeSegmentPlan{
                .block_start = static_cast<std::uint64_t>(block_plan.block_start),
                .block_len = block_plan.block_len,
                .segment_index = segment,
                .table_index = block_plan.table_index,
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
// Inputs: `device_input`, `selection`, `code_tables`, and `telemetry` describe selected adaptive segments.
// Outputs: Returns the combined encoded bitstream for all selected adaptive-prefix blocks.
std::vector<std::byte> pack_adaptive_prefix_segments_batch_device(const std::byte* device_input,
                                                                  const AdaptiveBatchSelection& selection,
                                                                  std::span<const AdaptiveEncodeTable> code_tables,
                                                                  GpuTelemetry* telemetry) {
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
        checked_multiply_bytes(code_tables.size(), sizeof(AdaptiveEncodeTable), "GPU adaptive prefix pack tables");
    const auto offset_bytes =
        checked_multiply_bytes(selection.pack_offsets.size(), sizeof(std::uint32_t), "GPU adaptive prefix offsets");
    auto required_bytes = checked_add_bytes(plan_bytes, table_bytes, "GPU adaptive prefix pack memory");
    required_bytes = checked_add_bytes(required_bytes, offset_bytes, "GPU adaptive prefix pack memory");
    required_bytes = checked_add_bytes(required_bytes, bitstream.size(), "GPU adaptive prefix pack memory");
    AdaptiveEncodeSegmentPlan* device_plans = nullptr;
    AdaptiveEncodeTable* device_tables = nullptr;
    std::uint32_t* device_offsets = nullptr;
    std::byte* device_encoded = nullptr;
    ensure_device_memory_budget(required_bytes, "GPU adaptive prefix pack batch");
    check_hip(hipMalloc(&device_plans, plan_bytes), "hipMalloc adaptive prefix pack plans");
    check_hip(hipMalloc(&device_tables, table_bytes), "hipMalloc adaptive prefix pack tables");
    check_hip(hipMalloc(&device_offsets, offset_bytes), "hipMalloc adaptive prefix pack offsets");
    check_hip(hipMalloc(&device_encoded, bitstream.size()), "hipMalloc adaptive prefix payload");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(required_bytes));
    try {
        check_hip(hipMemcpy(device_plans, selection.pack_plans.data(), plan_bytes, hipMemcpyHostToDevice),
                  "hipMemcpy adaptive prefix pack plans");
        check_hip(hipMemcpy(device_tables, code_tables.data(), table_bytes, hipMemcpyHostToDevice),
                  "hipMemcpy adaptive prefix pack tables");
        check_hip(hipMemcpy(device_offsets, selection.pack_offsets.data(), offset_bytes, hipMemcpyHostToDevice),
                  "hipMemcpy adaptive prefix pack offsets");
        record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(plan_bytes + table_bytes + offset_bytes));
        check_hip(hipMemset(device_encoded, 0, bitstream.size()), "hipMemset adaptive prefix payload");
        auto events = make_hip_event_pair("create adaptive_prefix_pack_segments_batch_kernel events");
        check_hip(hipEventRecord(events.start, hipStreamPerThread),
                  "record adaptive_prefix_pack_segments_batch_kernel start");
        adaptive_prefix_pack_segments_batch_kernel<<<static_cast<unsigned int>(selection.pack_plans.size()),
                                                     kGpuPrefixSegmentThreads, 0, hipStreamPerThread>>>(
            device_input, device_plans, device_tables, device_offsets, device_encoded,
            static_cast<std::uint32_t>(selection.pack_plans.size()));
        check_hip(hipGetLastError(), "launch adaptive_prefix_pack_segments_batch_kernel");
        check_hip(hipEventRecord(events.stop, hipStreamPerThread),
                  "record adaptive_prefix_pack_segments_batch_kernel stop");
        finish_measured_kernel(telemetry, events, "synchronize adaptive_prefix_pack_segments_batch_kernel");
        check_hip(hipMemcpy(bitstream.data(), device_encoded, bitstream.size(), hipMemcpyDeviceToHost),
                  "hipMemcpy adaptive prefix payload");
        record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(bitstream.size()));
        check_hip(hipFree(device_plans), "hipFree adaptive prefix pack plans");
        device_plans = nullptr;
        check_hip(hipFree(device_tables), "hipFree adaptive prefix pack tables");
        device_tables = nullptr;
        check_hip(hipFree(device_offsets), "hipFree adaptive prefix pack offsets");
        device_offsets = nullptr;
        check_hip(hipFree(device_encoded), "hipFree adaptive prefix payload");
        device_encoded = nullptr;
        return bitstream;
    } catch (...) {
        (void)hipFree(device_plans);
        (void)hipFree(device_tables);
        (void)hipFree(device_offsets);
        (void)hipFree(device_encoded);
        throw;
    }
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

}  // namespace

// Purpose: Replace verified raw HIP blocks with adaptive GPU prefix blocks when per-block codebooks improve size.
// Inputs: `device_input`, `input`, `block_size`, `source_blocks`, and `compression_level` describe one uploaded chunk.
// Outputs: Returns a new encoded chunk when adaptive blocks are useful; otherwise returns empty.
std::optional<EncodedChunk> encode_adaptive_prefix_chunk_device(const std::byte* device_input,
                                                                std::span<const std::byte> input,
                                                                std::uint32_t block_size,
                                                                std::span<const BlockDescriptor> source_blocks,
                                                                int compression_level, GpuTelemetry* telemetry) {
    std::vector<AdaptiveEncodeSegmentPlan> length_plans;
    std::vector<AdaptiveEncodeTable> code_tables;
    auto block_plans =
        build_adaptive_encode_plans(input, block_size, source_blocks, compression_level, length_plans, code_tables);
    if (length_plans.empty()) {
        return std::nullopt;
    }
    const auto segment_lengths =
        compute_adaptive_prefix_lengths_batch_device(device_input, length_plans, code_tables, telemetry);
    auto selection = select_adaptive_blocks_for_batch(block_plans, segment_lengths);
    if (selection.adaptive_blocks == 0U) {
        return std::nullopt;
    }
    auto bitstream = pack_adaptive_prefix_segments_batch_device(device_input, selection, code_tables, telemetry);
    EncodedChunk out;
    out.blocks.reserve(source_blocks.size());
    out.payload.reserve(input.size());
    std::uint64_t payload_offset = 0;
    for (std::uint32_t block_index = 0; block_index < source_blocks.size(); ++block_index) {
        const auto& block_plan = block_plans[block_index];
        const auto& source_block = source_blocks[block_index];
        const auto len = static_cast<std::size_t>(block_plan.block_len);
        if (block_plan.use_adaptive) {
            const auto table_bytes = checked_multiply_bytes(block_plan.offsets.size(), sizeof(std::uint32_t),
                                                            "GPU adaptive prefix block table");
            auto prefix_payload_size =
                checked_add_bytes(kGpuAdaptivePrefixCodebookBytes, table_bytes, "GPU adaptive prefix payload");
            prefix_payload_size =
                checked_add_bytes(prefix_payload_size, block_plan.bitstream_bytes, "GPU adaptive prefix payload");
            if (prefix_payload_size > std::numeric_limits<std::uint32_t>::max()) {
                throw GpuError("GPU adaptive prefix block exceeds block metadata limit");
            }
            out.blocks.push_back(BlockDescriptor{.kind = BlockKind::GpuAdaptivePrefix,
                                                 .fill_value = 0,
                                                 .uncompressed_len = static_cast<std::uint32_t>(len),
                                                 .encoded_offset = payload_offset,
                                                 .encoded_len = static_cast<std::uint32_t>(prefix_payload_size)});
            append_adaptive_prefix_payload(out, block_plan, bitstream);
            payload_offset += prefix_payload_size;
        } else {
            append_fallback_block(out, source_block, input, block_plan.block_start, payload_offset);
        }
    }
    return out;
}

}  // namespace superzip::hip_detail
