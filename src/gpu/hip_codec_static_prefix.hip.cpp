#include "gpu/hip_codec_support.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <vector>
#include <hip/hip_runtime.h>

namespace superzip::hip_detail {
namespace {

struct PrefixEncodeSegmentPlan {
    std::uint64_t block_start;
    std::uint32_t block_len;
    std::uint32_t segment_index;
};

struct PrefixEncodeBlockPlan {
    std::size_t block_start = 0;
    std::uint32_t block_len = 0;
    std::uint32_t segment_offset = 0;
    std::uint32_t segment_count = 0;
    bool use_prefix = false;
    std::uint32_t bitstream_offset = 0;
    std::uint32_t bitstream_bytes = 0;
    std::vector<std::uint32_t> offsets;
};

struct PrefixBatchSelection {
    std::uint64_t prefix_blocks = 0;
    std::uint32_t bitstream_bytes = 0;
    std::vector<PrefixEncodeSegmentPlan> pack_plans;
    std::vector<std::uint32_t> pack_offsets;
};

// Purpose: Return the static prefix-code bit width for one byte.
// Inputs: `value` is an uncompressed byte.
// Outputs: Returns the number of bits emitted by the GPU prefix codec.
__device__ std::uint32_t gpu_prefix_width(std::uint8_t value) {
    if (value <= 3U) {
        return 3U;
    }
    if (value <= 19U) {
        return 6U;
    }
    if (value <= 83U) {
        return 9U;
    }
    return 11U;
}

// Purpose: Return the static prefix-code bits for one byte in little-bit order.
// Inputs: `value` is an uncompressed byte and `width` receives the bit count.
// Outputs: Returns the code bits packed from least-significant to most-significant bit.
__device__ std::uint32_t gpu_prefix_code(std::uint8_t value, std::uint32_t& width) {
    width = gpu_prefix_width(value);
    if (value <= 3U) {
        return static_cast<std::uint32_t>(value) << 1U;
    }
    if (value <= 19U) {
        return 0x1U | ((static_cast<std::uint32_t>(value) - 4U) << 2U);
    }
    if (value <= 83U) {
        return 0x3U | ((static_cast<std::uint32_t>(value) - 20U) << 3U);
    }
    return 0x7U | ((static_cast<std::uint32_t>(value) - 84U) << 3U);
}

// Purpose: Compute one worker thread's prefix-code bit count for a contiguous byte range.
// Inputs: `input`, `block_start`, `start`, and `end` describe readable device input bytes.
// Outputs: Returns the number of encoded bits for the worker range.
__device__ std::uint32_t gpu_prefix_range_bit_count(const std::byte* input, std::size_t block_start, std::size_t start,
                                                    std::size_t end) {
    std::uint32_t bits = 0;
    for (auto pos = start; pos < end; ++pos) {
        bits += gpu_prefix_width(static_cast<std::uint8_t>(input[block_start + pos]));
    }
    return bits;
}

// Purpose: Compute encoded byte counts for a batched list of prefix-code segments.
// Inputs: `input` is a device chunk, `plans` maps each launched block to a source segment, and `segment_lengths`
// receives one byte count per segment. Outputs: Writes aligned compressed byte counts for the static prefix codec.
__global__ void prefix_segment_lengths_batch_kernel(const std::byte* input, const PrefixEncodeSegmentPlan* plans,
                                                    std::uint32_t* segment_lengths, std::uint32_t segment_count) {
    const auto segment = static_cast<std::uint32_t>(blockIdx.x);
    if (segment >= segment_count) {
        return;
    }
    const auto& plan = plans[segment];
    __shared__ std::uint32_t sums[kGpuPrefixSegmentThreads];
    const auto thread_id = static_cast<std::uint32_t>(threadIdx.x);
    std::size_t start = 0;
    std::size_t end = 0;
    gpu_prefix_thread_range(plan.block_len, plan.segment_index, thread_id, start, end);
    sums[thread_id] = gpu_prefix_range_bit_count(input, plan.block_start, start, end);
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

// Purpose: Pack a batched list of byte segments with SuperZip's static GPU prefix code.
// Inputs: `input` is a device chunk, `plans` maps launched blocks to source segments, `segment_offsets` contains
// 4-byte-aligned output offsets, and `encoded` is zeroed output storage. Outputs: Writes prefix bitstreams.
__global__ void prefix_pack_segments_batch_kernel(const std::byte* input, const PrefixEncodeSegmentPlan* plans,
                                                  const std::uint32_t* segment_offsets, std::byte* encoded,
                                                  std::uint32_t segment_count) {
    const auto segment = static_cast<std::uint32_t>(blockIdx.x);
    if (segment >= segment_count) {
        return;
    }
    const auto& plan = plans[segment];
    __shared__ std::uint32_t sums[kGpuPrefixSegmentThreads];
    const auto thread_id = static_cast<std::uint32_t>(threadIdx.x);
    std::size_t start = 0;
    std::size_t end = 0;
    gpu_prefix_thread_range(plan.block_len, plan.segment_index, thread_id, start, end);
    const auto local_bits = gpu_prefix_range_bit_count(input, plan.block_start, start, end);
    const auto thread_bit_base = gpu_prefix_exclusive_scan(sums, local_bits);
    if (local_bits == 0U) {
        return;
    }
    unsigned int scratch[kGpuPrefixMaxThreadWords] = {};
    auto local_bit_pos = thread_bit_base % 32U;
    for (auto pos = start; pos < end; ++pos) {
        std::uint32_t width = 0;
        const auto code = gpu_prefix_code(static_cast<std::uint8_t>(input[plan.block_start + pos]), width);
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

// Purpose: Return the expected block byte range for one verified descriptor.
// Inputs: `input_size`, `block_size`, `block_index`, and `block` describe one dense block in the chunk.
// Outputs: Returns the block start; throws if verified metadata no longer matches the chunk layout.
std::size_t checked_block_start(std::size_t input_size, std::uint32_t block_size, std::uint32_t block_index,
                                const BlockDescriptor& block) {
    const auto start = static_cast<std::size_t>(block_index) * block_size;
    if (start > input_size || block.uncompressed_len > input_size - start) {
        throw GpuError("GPU prefix source block exceeds uploaded chunk");
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
            throw GpuError("GPU prefix fallback pattern block is invalid");
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
        throw GpuError("GPU prefix fallback block kind is not supported");
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

// Purpose: Build chunk-wide prefix segment plans for verified raw blocks large enough to encode.
// Inputs: `input_size`, `block_size`, and `source_blocks` describe one bounded archive chunk.
// Outputs: Returns host block plans and appends one device segment plan per eligible 4 KiB prefix segment.
std::vector<PrefixEncodeBlockPlan> build_prefix_encode_plans(std::size_t input_size, std::uint32_t block_size,
                                                             std::span<const BlockDescriptor> source_blocks,
                                                             std::vector<PrefixEncodeSegmentPlan>& segment_plans) {
    std::vector<PrefixEncodeBlockPlan> block_plans;
    block_plans.reserve(source_blocks.size());
    for (std::uint32_t block_index = 0; block_index < source_blocks.size(); ++block_index) {
        const auto& source_block = source_blocks[block_index];
        const auto start = checked_block_start(input_size, block_size, block_index, source_block);
        const auto len = source_block.uncompressed_len;
        const bool eligible = source_block.kind == BlockKind::Raw && len >= kGpuPrefixSegmentBytes;
        const auto segment_count = eligible ? (len + kGpuPrefixSegmentBytes - 1U) / kGpuPrefixSegmentBytes : 0U;
        PrefixEncodeBlockPlan block_plan{
            .block_start = start,
            .block_len = len,
            .segment_offset = static_cast<std::uint32_t>(segment_plans.size()),
            .segment_count = len >= kGpuPrefixSegmentBytes ? segment_count : 0U,
        };
        if (block_plan.segment_count != 0U) {
            segment_plans.reserve(segment_plans.size() + block_plan.segment_count);
            for (std::uint32_t segment = 0; segment < block_plan.segment_count; ++segment) {
                segment_plans.push_back(PrefixEncodeSegmentPlan{
                    .block_start = static_cast<std::uint64_t>(start),
                    .block_len = len,
                    .segment_index = segment,
                });
            }
        }
        block_plans.push_back(std::move(block_plan));
    }
    return block_plans;
}

// Purpose: Compute prefix segment byte lengths for the whole uploaded chunk in one HIP pass.
// Inputs: `device_input` is the chunk in VRAM, `segment_plans` maps every segment, and `telemetry` records HIP work.
// Outputs: Returns one aligned encoded byte count per segment.
std::vector<std::uint32_t> compute_prefix_lengths_batch_device(const std::byte* device_input,
                                                               std::span<const PrefixEncodeSegmentPlan> segment_plans,
                                                               GpuTelemetry* telemetry) {
    std::vector<std::uint32_t> segment_lengths(segment_plans.size());
    if (segment_plans.empty()) {
        return segment_lengths;
    }
    if (segment_plans.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw GpuError("GPU prefix segment count exceeds HIP launch limits");
    }
    const auto plan_bytes =
        checked_multiply_bytes(segment_plans.size(), sizeof(PrefixEncodeSegmentPlan), "GPU prefix length plans");
    const auto length_bytes =
        checked_multiply_bytes(segment_plans.size(), sizeof(std::uint32_t), "GPU prefix segment lengths");
    const auto required_bytes = checked_add_bytes(plan_bytes, length_bytes, "GPU prefix length memory");
    PrefixEncodeSegmentPlan* device_plans = nullptr;
    std::uint32_t* device_lengths = nullptr;
    ensure_device_memory_budget(required_bytes, "GPU prefix length batch");
    check_hip(hipMalloc(&device_plans, plan_bytes), "hipMalloc prefix length plans");
    check_hip(hipMalloc(&device_lengths, length_bytes), "hipMalloc prefix segment lengths");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(required_bytes));
    try {
        check_hip(hipMemcpy(device_plans, segment_plans.data(), plan_bytes, hipMemcpyHostToDevice),
                  "hipMemcpy prefix length plans");
        record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(plan_bytes));
        auto events = make_hip_event_pair("create prefix_segment_lengths_batch_kernel events");
        check_hip(hipEventRecord(events.start, hipStreamPerThread), "record prefix_segment_lengths_batch_kernel start");
        prefix_segment_lengths_batch_kernel<<<static_cast<unsigned int>(segment_plans.size()), kGpuPrefixSegmentThreads,
                                              0, hipStreamPerThread>>>(
            device_input, device_plans, device_lengths, static_cast<std::uint32_t>(segment_plans.size()));
        check_hip(hipGetLastError(), "launch prefix_segment_lengths_batch_kernel");
        check_hip(hipEventRecord(events.stop, hipStreamPerThread), "record prefix_segment_lengths_batch_kernel stop");
        finish_measured_kernel(telemetry, events, "synchronize prefix_segment_lengths_batch_kernel");
        check_hip(hipMemcpy(segment_lengths.data(), device_lengths, length_bytes, hipMemcpyDeviceToHost),
                  "hipMemcpy prefix segment lengths");
        record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(length_bytes));
        check_hip(hipFree(device_plans), "hipFree prefix length plans");
        device_plans = nullptr;
        check_hip(hipFree(device_lengths), "hipFree prefix segment lengths");
        device_lengths = nullptr;
        return segment_lengths;
    } catch (...) {
        (void)hipFree(device_plans);
        (void)hipFree(device_lengths);
        throw;
    }
}

// Purpose: Select prefix-compressed blocks and build one combined pack plan.
// Inputs: `block_plans` are mutable host plans and `segment_lengths` are GPU-measured byte counts.
// Outputs: Returns combined pack plans and offsets; mutates selected block plans with payload metadata.
PrefixBatchSelection select_prefix_blocks_for_batch(std::vector<PrefixEncodeBlockPlan>& block_plans,
                                                    std::span<const std::uint32_t> segment_lengths) {
    PrefixBatchSelection selection;
    for (auto& block_plan : block_plans) {
        if (block_plan.segment_count == 0U) {
            continue;
        }
        block_plan.offsets.assign(static_cast<std::size_t>(block_plan.segment_count) + 1U, 0U);
        for (std::uint32_t segment = 0; segment < block_plan.segment_count; ++segment) {
            const auto length_index = static_cast<std::size_t>(block_plan.segment_offset) + segment;
            block_plan.offsets[segment + 1U] = checked_prefix_offset_add(
                block_plan.offsets[segment], segment_lengths[length_index], "GPU prefix encoded block size");
        }
        const auto table_bytes =
            checked_multiply_bytes(block_plan.offsets.size(), sizeof(std::uint32_t), "GPU prefix block table");
        const auto payload_bytes =
            checked_add_bytes(table_bytes, block_plan.offsets.back(), "GPU prefix block payload");
        if (block_plan.offsets.back() == 0U || payload_bytes >= block_plan.block_len) {
            block_plan.offsets.clear();
            continue;
        }
        block_plan.use_prefix = true;
        block_plan.bitstream_offset = selection.bitstream_bytes;
        block_plan.bitstream_bytes = block_plan.offsets.back();
        ++selection.prefix_blocks;
        for (std::uint32_t segment = 0; segment < block_plan.segment_count; ++segment) {
            selection.pack_plans.push_back(PrefixEncodeSegmentPlan{
                .block_start = static_cast<std::uint64_t>(block_plan.block_start),
                .block_len = block_plan.block_len,
                .segment_index = segment,
            });
            selection.pack_offsets.push_back(checked_prefix_offset_add(
                selection.bitstream_bytes, block_plan.offsets[segment], "GPU prefix packed payload"));
        }
        selection.bitstream_bytes = checked_prefix_offset_add(selection.bitstream_bytes, block_plan.bitstream_bytes,
                                                              "GPU prefix combined payload");
    }
    return selection;
}

// Purpose: Pack all selected prefix segments into one combined device buffer.
// Inputs: `device_input` is the uploaded chunk, `selection` describes selected segments, and `telemetry` records HIP.
// Outputs: Returns the combined encoded bitstream for all selected prefix blocks.
std::vector<std::byte> pack_prefix_segments_batch_device(const std::byte* device_input,
                                                         const PrefixBatchSelection& selection,
                                                         GpuTelemetry* telemetry) {
    std::vector<std::byte> bitstream(selection.bitstream_bytes);
    if (selection.pack_plans.empty()) {
        return bitstream;
    }
    if (selection.pack_plans.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw GpuError("GPU prefix pack segment count exceeds HIP launch limits");
    }
    const auto plan_bytes =
        checked_multiply_bytes(selection.pack_plans.size(), sizeof(PrefixEncodeSegmentPlan), "GPU prefix pack plans");
    const auto offset_bytes =
        checked_multiply_bytes(selection.pack_offsets.size(), sizeof(std::uint32_t), "GPU prefix pack offsets");
    auto required_bytes = checked_add_bytes(plan_bytes, offset_bytes, "GPU prefix pack memory");
    required_bytes = checked_add_bytes(required_bytes, bitstream.size(), "GPU prefix pack memory");
    PrefixEncodeSegmentPlan* device_plans = nullptr;
    std::uint32_t* device_offsets = nullptr;
    std::byte* device_encoded = nullptr;
    ensure_device_memory_budget(required_bytes, "GPU prefix pack batch");
    check_hip(hipMalloc(&device_plans, plan_bytes), "hipMalloc prefix pack plans");
    check_hip(hipMalloc(&device_offsets, offset_bytes), "hipMalloc prefix pack offsets");
    check_hip(hipMalloc(&device_encoded, bitstream.size()), "hipMalloc prefix payload");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(required_bytes));
    try {
        check_hip(hipMemcpy(device_plans, selection.pack_plans.data(), plan_bytes, hipMemcpyHostToDevice),
                  "hipMemcpy prefix pack plans");
        check_hip(hipMemcpy(device_offsets, selection.pack_offsets.data(), offset_bytes, hipMemcpyHostToDevice),
                  "hipMemcpy prefix pack offsets");
        record_gpu_h2d_bytes(telemetry, static_cast<std::uint64_t>(plan_bytes + offset_bytes));
        check_hip(hipMemset(device_encoded, 0, bitstream.size()), "hipMemset prefix payload");
        auto events = make_hip_event_pair("create prefix_pack_segments_batch_kernel events");
        check_hip(hipEventRecord(events.start, hipStreamPerThread), "record prefix_pack_segments_batch_kernel start");
        prefix_pack_segments_batch_kernel<<<static_cast<unsigned int>(selection.pack_plans.size()),
                                            kGpuPrefixSegmentThreads, 0, hipStreamPerThread>>>(
            device_input, device_plans, device_offsets, device_encoded,
            static_cast<std::uint32_t>(selection.pack_plans.size()));
        check_hip(hipGetLastError(), "launch prefix_pack_segments_batch_kernel");
        check_hip(hipEventRecord(events.stop, hipStreamPerThread), "record prefix_pack_segments_batch_kernel stop");
        finish_measured_kernel(telemetry, events, "synchronize prefix_pack_segments_batch_kernel");
        check_hip(hipMemcpy(bitstream.data(), device_encoded, bitstream.size(), hipMemcpyDeviceToHost),
                  "hipMemcpy prefix payload");
        record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(bitstream.size()));
        check_hip(hipFree(device_plans), "hipFree prefix pack plans");
        device_plans = nullptr;
        check_hip(hipFree(device_offsets), "hipFree prefix pack offsets");
        device_offsets = nullptr;
        check_hip(hipFree(device_encoded), "hipFree prefix payload");
        device_encoded = nullptr;
        return bitstream;
    } catch (...) {
        (void)hipFree(device_plans);
        (void)hipFree(device_offsets);
        (void)hipFree(device_encoded);
        throw;
    }
}

// Purpose: Append one selected GPU-prefix payload table and bitstream slice.
// Inputs: `out`, `block_plan`, and `bitstream` describe a selected prefix block.
// Outputs: Mutates `out.payload` with the block-local offset table followed by encoded bytes.
void append_prefix_payload(EncodedChunk& out, const PrefixEncodeBlockPlan& block_plan,
                           std::span<const std::byte> bitstream) {
    for (const auto offset : block_plan.offsets) {
        append_prefix_u32(out.payload, offset);
    }
    const auto start = static_cast<std::size_t>(block_plan.bitstream_offset);
    const auto end = start + static_cast<std::size_t>(block_plan.bitstream_bytes);
    out.payload.insert(out.payload.end(), bitstream.begin() + static_cast<std::ptrdiff_t>(start),
                       bitstream.begin() + static_cast<std::ptrdiff_t>(end));
}

}  // namespace

// Purpose: Replace verified raw HIP blocks with smaller GPU prefix blocks where the static codec is effective.
// Inputs: `device_input`, `input`, `block_size`, and `source_blocks` describe one uploaded archive chunk.
// Outputs: Returns a new encoded chunk when at least one block is prefix-compressed; otherwise returns empty.
std::optional<EncodedChunk> encode_prefix_chunk_device(const std::byte* device_input, std::span<const std::byte> input,
                                                       std::uint32_t block_size,
                                                       std::span<const BlockDescriptor> source_blocks,
                                                       GpuTelemetry* telemetry) {
    std::vector<PrefixEncodeSegmentPlan> length_plans;
    auto block_plans = build_prefix_encode_plans(input.size(), block_size, source_blocks, length_plans);
    if (length_plans.empty()) {
        return std::nullopt;
    }
    const auto segment_lengths = compute_prefix_lengths_batch_device(device_input, length_plans, telemetry);
    auto selection = select_prefix_blocks_for_batch(block_plans, segment_lengths);
    if (selection.prefix_blocks == 0U) {
        return std::nullopt;
    }
    auto bitstream = pack_prefix_segments_batch_device(device_input, selection, telemetry);
    EncodedChunk out;
    out.blocks.reserve(source_blocks.size());
    out.payload.reserve(input.size());
    std::uint64_t payload_offset = 0;
    for (std::uint32_t block_index = 0; block_index < source_blocks.size(); ++block_index) {
        const auto& block_plan = block_plans[block_index];
        const auto& source_block = source_blocks[block_index];
        const auto len = static_cast<std::size_t>(block_plan.block_len);
        if (block_plan.use_prefix) {
            const auto table_bytes =
                checked_multiply_bytes(block_plan.offsets.size(), sizeof(std::uint32_t), "GPU prefix block table");
            const auto prefix_payload_size =
                checked_add_bytes(table_bytes, block_plan.bitstream_bytes, "GPU prefix payload");
            if (prefix_payload_size > std::numeric_limits<std::uint32_t>::max()) {
                throw GpuError("GPU prefix block exceeds block metadata limit");
            }
            out.blocks.push_back(BlockDescriptor{.kind = BlockKind::GpuPrefix,
                                                 .fill_value = 0,
                                                 .uncompressed_len = static_cast<std::uint32_t>(len),
                                                 .encoded_offset = payload_offset,
                                                 .encoded_len = static_cast<std::uint32_t>(prefix_payload_size)});
            append_prefix_payload(out, block_plan, bitstream);
            payload_offset += prefix_payload_size;
        } else {
            append_fallback_block(out, source_block, input, block_plan.block_start, payload_offset);
        }
    }
    return out;
}

}  // namespace superzip::hip_detail
