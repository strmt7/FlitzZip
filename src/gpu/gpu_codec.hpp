#pragma once

#include "core/archive_blocks.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace superzip {

struct GpuInfo {
    bool hip_compiled = false;
    bool hip_runtime_loadable = false;
    bool available = false;
    int device_count = 0;
    int selected_device = -1;
    std::uint64_t vram_total_bytes = 0;
    std::uint64_t vram_free_bytes = 0;
    std::string runtime_name;
    std::string device_name;
    std::string gcn_arch;
    std::string status;
};

struct GpuRuntimeStats {
    std::uint64_t encode_chunks = 0;
    std::uint64_t decode_chunks = 0;
    std::uint64_t kernel_launches = 0;
    std::uint64_t h2d_bytes = 0;
    std::uint64_t d2h_bytes = 0;
    std::uint64_t device_allocation_bytes = 0;
    std::uint64_t pattern_blocks = 0;
    double kernel_ms = 0.0;
};

struct GpuTelemetry {
    std::atomic<std::uint64_t> encode_chunks{0};
    std::atomic<std::uint64_t> decode_chunks{0};
    std::atomic<std::uint64_t> kernel_launches{0};
    std::atomic<std::uint64_t> h2d_bytes{0};
    std::atomic<std::uint64_t> d2h_bytes{0};
    std::atomic<std::uint64_t> device_allocation_bytes{0};
    std::atomic<std::uint64_t> pattern_blocks{0};
    std::atomic<std::uint64_t> kernel_microseconds{0};
};

struct GpuCodecOptions {
    bool require_gpu = true;
    bool force_cpu = false;
    std::uint32_t block_size = kDefaultArchiveBlockBytes;
    std::uint32_t worker_count = 1;
    int compression_level = kDefaultCompressionLevel;
    std::shared_ptr<GpuTelemetry> telemetry;
};

struct GpuDiagnosticOptions {
    double seconds = 5.0;
    std::uint32_t buffer_mib = 128;
    std::uint32_t inner_iterations = 256;
};

struct GpuDiagnosticResult {
    GpuInfo info;
    std::uint64_t bytes = 0;
    std::uint64_t kernel_launches = 0;
    std::uint64_t h2d_bytes = 0;
    std::uint64_t d2h_bytes = 0;
    std::uint64_t device_allocation_bytes = 0;
    std::uint64_t checksum = 0;
    double kernel_ms = 0.0;
    double wall_seconds = 0.0;
};

struct DecodedChunkCrc {
    std::uint32_t crc32 = 0;
    bool gpu_used = false;
};

// Purpose: Snapshot per-operation AMD HIP telemetry into plain counters.
// Inputs: `telemetry` is the operation-owned counter set shared with codec tasks.
// Outputs: Returns stable numeric counters suitable for operation statistics.
GpuRuntimeStats snapshot_gpu_telemetry(const GpuTelemetry& telemetry);

// Purpose: Record that one archive chunk entered the AMD HIP encode backend.
// Inputs: `telemetry` is optional operation-owned telemetry.
// Outputs: Atomically increments the encode chunk count when telemetry is present.
void record_gpu_encode_chunk(GpuTelemetry* telemetry);

// Purpose: Record that one archive chunk entered the AMD HIP decode backend.
// Inputs: `telemetry` is optional operation-owned telemetry.
// Outputs: Atomically increments the decode chunk count when telemetry is present.
void record_gpu_decode_chunk(GpuTelemetry* telemetry);

// Purpose: Record host-to-device transfer bytes submitted through AMD HIP.
// Inputs: `telemetry` is optional operation-owned telemetry and `bytes` is the transfer size.
// Outputs: Atomically adds the byte count when telemetry is present.
void record_gpu_h2d_bytes(GpuTelemetry* telemetry, std::uint64_t bytes);

// Purpose: Record device-to-host transfer bytes submitted through AMD HIP.
// Inputs: `telemetry` is optional operation-owned telemetry and `bytes` is the transfer size.
// Outputs: Atomically adds the byte count when telemetry is present.
void record_gpu_d2h_bytes(GpuTelemetry* telemetry, std::uint64_t bytes);

// Purpose: Record bounded AMD HIP device allocation bytes requested by the codec.
// Inputs: `telemetry` is optional operation-owned telemetry and `bytes` is the allocation size.
// Outputs: Atomically adds the allocation byte count when telemetry is present.
void record_gpu_device_allocation_bytes(GpuTelemetry* telemetry, std::uint64_t bytes);

// Purpose: Record GPU-compressed periodic pattern blocks emitted by the AMD HIP encoder.
// Inputs: `telemetry` is optional operation-owned telemetry and `count` is the number of compact pattern blocks.
// Outputs: Atomically adds the block count when telemetry is present.
void record_gpu_pattern_blocks(GpuTelemetry* telemetry, std::uint64_t count);

// Purpose: Record one AMD HIP kernel launch and its device-event elapsed time.
// Inputs: `telemetry` is optional operation-owned telemetry and `milliseconds` is measured with HIP events.
// Outputs: Atomically increments launch count and adds elapsed device time when telemetry is present.
void record_gpu_kernel_launch(GpuTelemetry* telemetry, double milliseconds);

// Purpose: Inspect the compiled GPU backend and available AMD HIP device.
// Inputs: None.
// Outputs: Returns backend/device status suitable for CLI, GUI, and diagnostics.
GpuInfo query_gpu_info();

// Purpose: Run a HIP-only compute diagnostic that is independent of archive I/O.
// Inputs: `options` controls duration, device buffer size, and per-kernel integer work.
// Outputs: Returns HIP event timing, transfer/allocation counters, and a checksum produced from device-written bytes.
GpuDiagnosticResult run_gpu_diagnostic(const GpuDiagnosticOptions& options);

// Purpose: Encode a chunk into SuperZip block descriptors and payload bytes.
// Inputs: `input` is the uncompressed chunk and `options` selects block size plus required-GPU or forced-CPU behavior.
// Outputs: Returns descriptors, encoded payload, and whether GPU work was used; throws `GpuError` if GPU is required
// but unavailable.
EncodedChunk encode_chunk(std::span<const std::byte> input, const GpuCodecOptions& options);

// Purpose: Encode a caller-owned chunk while allowing zero-copy publication of raw HIP payloads.
// Inputs: `input` owns the uncompressed bytes and may be moved from; `options` selects block size plus backend policy.
// Outputs: Returns descriptors, payload, and a source CRC; throws `GpuError` if GPU is required but unavailable.
EncodedChunk encode_owned_chunk(std::vector<std::byte> input, const GpuCodecOptions& options);

// Purpose: Decode encoded SuperZip block payload back into caller-provided output memory.
// Inputs: `payload` and `blocks` come from validated archive metadata, `output` is the exact uncompressed destination
// buffer, and `options` controls required-GPU or forced-CPU behavior. Outputs: Writes decoded bytes into `output` and
// returns true when AMD HIP executed; throws `ArchiveError` or `GpuError` on invalid block layout or unavailable
// required GPU.
bool decode_chunk(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                  std::span<std::byte> output, const GpuCodecOptions& options);

// Purpose: Decode enough chunk content to compute its ZIP-compatible CRC-32.
// Inputs: `payload` and `blocks` come from validated archive metadata, `output_size` is the exact decoded byte count,
// and `options` controls required-GPU or forced-CPU behavior. Outputs: Returns the decoded chunk CRC and whether AMD
// HIP executed; throws `ArchiveError` or `GpuError` on invalid block layout or unavailable required GPU.
DecodedChunkCrc crc_decoded_chunk(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                                  std::uint64_t output_size, const GpuCodecOptions& options);

}  // namespace superzip
