#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace superzip {

enum class BlockKind : std::uint8_t {
    Raw = 0,
    Fill = 1,
};

struct BlockDescriptor {
    BlockKind kind = BlockKind::Raw;
    std::uint8_t fill_value = 0;
    std::uint32_t uncompressed_len = 0;
    std::uint64_t encoded_offset = 0;
    std::uint32_t encoded_len = 0;
};

struct EncodedChunk {
    std::vector<BlockDescriptor> blocks;
    std::vector<std::byte> payload;
    bool gpu_used = false;
};

struct GpuInfo {
    bool hip_compiled = false;
    bool hip_runtime_loadable = false;
    bool available = false;
    int device_count = 0;
    int selected_device = -1;
    std::string runtime_name;
    std::string device_name;
    std::string gcn_arch;
    std::string status;
};

struct GpuCodecOptions {
    bool require_gpu = true;
    std::uint32_t block_size = 64 * 1024;
};

// Purpose: Inspect the compiled GPU backend and available AMD HIP device.
// Inputs: None.
// Outputs: Returns backend/device status suitable for CLI, GUI, and diagnostics.
GpuInfo query_gpu_info();

// Purpose: Encode a chunk into SuperZip block descriptors and payload bytes.
// Inputs: `input` is the uncompressed chunk and `options` selects block size plus whether AMD GPU acceleration is mandatory.
// Outputs: Returns descriptors, encoded payload, and whether GPU work was used; throws `GpuError` if GPU is required but unavailable.
EncodedChunk encode_chunk(std::span<const std::byte> input, const GpuCodecOptions& options);

// Purpose: Decode encoded SuperZip block payload back into caller-provided output memory.
// Inputs: `payload` and `blocks` come from validated archive metadata, `output` is the exact uncompressed destination buffer, and `options` controls required GPU behavior.
// Outputs: Writes decoded bytes into `output` and returns true when AMD HIP executed; throws `ArchiveError` or `GpuError` on invalid block layout or unavailable required GPU.
bool decode_chunk(
    std::span<const std::byte> payload,
    std::span<const BlockDescriptor> blocks,
    std::span<std::byte> output,
    const GpuCodecOptions& options);

}  // namespace superzip
