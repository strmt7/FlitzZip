#include "gpu/gpu_codec.hpp"

#include "core/result.hpp"

#include <algorithm>
#include <cstring>
#include <hip/hip_runtime.h>

namespace superzip {
GpuInfo query_gpu_info_cpu_only();

namespace {

struct DeviceBlock {
    std::uint8_t kind;
    std::uint8_t fill_value;
    std::uint16_t reserved;
    std::uint32_t uncompressed_len;
    std::uint64_t encoded_offset;
    std::uint32_t encoded_len;
};

// Purpose: Convert a HIP status into a SuperZip GPU exception.
// Inputs: `status` is a HIP API result and `action` labels the failing operation.
// Outputs: Returns on success; throws `GpuError` with the HIP error string on failure.
void check_hip(hipError_t status, const char* action) {
    if (status != hipSuccess) {
        throw GpuError(std::string(action) + ": " + hipGetErrorString(status));
    }
}

// Purpose: Classify each input block as uniform fill or raw bytes on the AMD GPU.
// Inputs: `input`/`input_len` are device bytes, `block_size` is bytes per block, `blocks` is device output metadata, and `block_count` bounds the launch.
// Outputs: Writes one `DeviceBlock` per block into device memory.
__global__ void analyze_blocks_kernel(
    const std::byte* input,
    std::size_t input_len,
    std::uint32_t block_size,
    DeviceBlock* blocks,
    std::uint32_t block_count) {
    const auto block_index = static_cast<std::uint32_t>(blockIdx.x);
    if (block_index >= block_count) {
        return;
    }
    const std::size_t start = static_cast<std::size_t>(block_index) * block_size;
    const std::size_t end = min(start + block_size, input_len);
    __shared__ int mismatch;
    __shared__ unsigned int first_value;
    if (threadIdx.x == 0) {
        mismatch = 0;
        first_value = start < end ? static_cast<unsigned int>(input[start]) & 0xFFU : 0U;
    }
    __syncthreads();
    for (std::size_t pos = start + threadIdx.x; pos < end; pos += blockDim.x) {
        if ((static_cast<unsigned int>(input[pos]) & 0xFFU) != first_value) {
            atomicExch(&mismatch, 1);
        }
    }
    __syncthreads();
    if (threadIdx.x == 0) {
        const auto len = static_cast<std::uint32_t>(end - start);
        blocks[block_index].kind = mismatch == 0 ? 1 : 0;
        blocks[block_index].fill_value = static_cast<std::uint8_t>(first_value);
        blocks[block_index].reserved = 0;
        blocks[block_index].uncompressed_len = len;
        blocks[block_index].encoded_offset = 0;
        blocks[block_index].encoded_len = mismatch == 0 ? 0 : len;
    }
}

// Purpose: Decode fill/raw block metadata into output bytes on the AMD GPU.
// Inputs: `payload`, `blocks`, `block_count`, `block_size`, `output`, and `output_len` are device pointers/counts validated by the host path.
// Outputs: Writes decoded bytes into `output`.
__global__ void materialize_blocks_kernel(
    const std::byte* payload,
    const DeviceBlock* blocks,
    std::uint32_t block_count,
    std::uint32_t block_size,
    std::byte* output,
    std::size_t output_len) {
    const std::size_t index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index >= output_len) {
        return;
    }
    const auto block_index = static_cast<std::uint32_t>(index / block_size);
    if (block_index >= block_count) {
        return;
    }
    const auto& block = blocks[block_index];
    const std::size_t in_block = index - static_cast<std::size_t>(block_index) * block_size;
    if (in_block >= block.uncompressed_len) {
        return;
    }
    if (block.kind == 1) {
        output[index] = static_cast<std::byte>(block.fill_value);
    } else {
        output[index] = payload[block.encoded_offset + in_block];
    }
}

}  // namespace

EncodedChunk encode_chunk_hip(std::span<const std::byte> input, const GpuCodecOptions& options) {
    if (input.empty()) {
        return {};
    }
    const auto info = query_gpu_info_cpu_only();
    if (!info.available) {
        throw GpuError(info.status);
    }
    const auto block_size = std::max<std::uint32_t>(1, options.block_size);
    const auto block_count = static_cast<std::uint32_t>((input.size() + block_size - 1) / block_size);

    std::byte* device_input = nullptr;
    DeviceBlock* device_blocks = nullptr;
    check_hip(hipMalloc(&device_input, input.size()), "hipMalloc input");
    check_hip(hipMalloc(&device_blocks, sizeof(DeviceBlock) * block_count), "hipMalloc block table");
    try {
        check_hip(hipMemcpy(device_input, input.data(), input.size(), hipMemcpyHostToDevice), "hipMemcpy input");
        analyze_blocks_kernel<<<block_count, 256>>>(device_input, input.size(), block_size, device_blocks, block_count);
        check_hip(hipGetLastError(), "launch analyze_blocks_kernel");
        check_hip(hipDeviceSynchronize(), "synchronize analyze_blocks_kernel");

        std::vector<DeviceBlock> device_results(block_count);
        check_hip(
            hipMemcpy(device_results.data(), device_blocks, sizeof(DeviceBlock) * block_count, hipMemcpyDeviceToHost),
            "hipMemcpy block table");

        EncodedChunk out;
        out.blocks.reserve(block_count);
        std::uint64_t encoded_offset = 0;
        for (std::uint32_t i = 0; i < block_count; ++i) {
            const auto start = static_cast<std::size_t>(i) * block_size;
            const auto len = static_cast<std::size_t>(device_results[i].uncompressed_len);
            if (device_results[i].kind == 1) {
                out.blocks.push_back(BlockDescriptor{
                    .kind = BlockKind::Fill,
                    .fill_value = device_results[i].fill_value,
                    .uncompressed_len = static_cast<std::uint32_t>(len),
                    .encoded_offset = encoded_offset,
                    .encoded_len = 0,
                });
            } else {
                out.blocks.push_back(BlockDescriptor{
                    .kind = BlockKind::Raw,
                    .fill_value = 0,
                    .uncompressed_len = static_cast<std::uint32_t>(len),
                    .encoded_offset = encoded_offset,
                    .encoded_len = static_cast<std::uint32_t>(len),
                });
                out.payload.insert(out.payload.end(), input.begin() + start, input.begin() + start + len);
                encoded_offset += len;
            }
        }
        check_hip(hipFree(device_input), "hipFree input");
        check_hip(hipFree(device_blocks), "hipFree block table");
        return out;
    } catch (...) {
        (void)hipFree(device_input);
        (void)hipFree(device_blocks);
        throw;
    }
}

void decode_chunk_hip(
    std::span<const std::byte> payload,
    std::span<const BlockDescriptor> blocks,
    std::span<std::byte> output,
    const GpuCodecOptions& options) {
    if (output.empty()) {
        return;
    }
    const auto info = query_gpu_info_cpu_only();
    if (!info.available) {
        throw GpuError(info.status);
    }
    const auto block_size = std::max<std::uint32_t>(1, options.block_size);
    std::vector<DeviceBlock> host_blocks;
    host_blocks.reserve(blocks.size());
    for (const auto& block : blocks) {
        host_blocks.push_back(DeviceBlock{
            .kind = block.kind == BlockKind::Fill ? static_cast<std::uint8_t>(1) : static_cast<std::uint8_t>(0),
            .fill_value = block.fill_value,
            .reserved = 0,
            .uncompressed_len = block.uncompressed_len,
            .encoded_offset = block.encoded_offset,
            .encoded_len = block.encoded_len,
        });
    }

    std::byte* device_payload = nullptr;
    std::byte* device_output = nullptr;
    DeviceBlock* device_blocks = nullptr;
    check_hip(hipMalloc(&device_payload, std::max<std::size_t>(payload.size(), 1)), "hipMalloc payload");
    check_hip(hipMalloc(&device_output, output.size()), "hipMalloc output");
    check_hip(hipMalloc(&device_blocks, sizeof(DeviceBlock) * host_blocks.size()), "hipMalloc decode blocks");
    try {
        if (!payload.empty()) {
            check_hip(hipMemcpy(device_payload, payload.data(), payload.size(), hipMemcpyHostToDevice), "hipMemcpy payload");
        }
        check_hip(
            hipMemcpy(device_blocks, host_blocks.data(), sizeof(DeviceBlock) * host_blocks.size(), hipMemcpyHostToDevice),
            "hipMemcpy decode blocks");
        constexpr int threads = 256;
        const auto grid = static_cast<unsigned int>((output.size() + threads - 1) / threads);
        materialize_blocks_kernel<<<grid, threads>>>(
            device_payload,
            device_blocks,
            static_cast<std::uint32_t>(host_blocks.size()),
            block_size,
            device_output,
            output.size());
        check_hip(hipGetLastError(), "launch materialize_blocks_kernel");
        check_hip(hipDeviceSynchronize(), "synchronize materialize_blocks_kernel");
        check_hip(hipMemcpy(output.data(), device_output, output.size(), hipMemcpyDeviceToHost), "hipMemcpy output");
        check_hip(hipFree(device_payload), "hipFree payload");
        check_hip(hipFree(device_output), "hipFree output");
        check_hip(hipFree(device_blocks), "hipFree decode blocks");
    } catch (...) {
        (void)hipFree(device_payload);
        (void)hipFree(device_output);
        (void)hipFree(device_blocks);
        throw;
    }
}

}  // namespace superzip
