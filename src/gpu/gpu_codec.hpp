#pragma once

#include "core/archive_blocks.hpp"
#include "gpu/hip_allocation_policy.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace superzip {

class HipHostOutputPool;

enum class GpuEncodeStage : std::size_t {
    Readiness,
    HostAnalysis,
    DeviceClassification,
    Prefix,
    Sparse,
    Dictionary,
    Publication,
    Count,
};

inline constexpr std::size_t kGpuEncodeStageCount = static_cast<std::size_t>(GpuEncodeStage::Count);

struct GpuInfo {
    bool hip_compiled = false;
    bool hip_runtime_loadable = false;
    bool available = false;
    int device_count = 0;
    int selected_device = -1;
    std::optional<std::uint64_t> adapter_luid;
    std::uint64_t vram_total_bytes = 0;
    std::uint64_t vram_free_bytes = 0;
    std::string runtime_name;
    std::optional<HipRuntimeVersion> runtime_version;
    // Runtime/device/pool eligibility only; production allocation may still use legacy HIP APIs.
    bool stream_ordered_allocator_supported = false;
    std::optional<std::uint64_t> pool_used_bytes;
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
    std::uint64_t host_pinned_allocation_bytes = 0;  // Cumulative completed decode allocations, not live RAM or VRAM.
    std::uint64_t host_pinned_output_bytes = 0;      // Completed output using pinned storage, including reuse.
    std::uint64_t pattern_blocks = 0;
    std::uint64_t prefix_blocks = 0;
    std::uint64_t dictionary_blocks = 0;
    std::uint64_t sparse_pattern_blocks = 0;
    double kernel_ms = 0.0;  // NaN means at least one event time or its accumulated total was invalid.
    // Summed concurrent worker time, not elapsed wall time or HIP device time.
    std::array<double, kGpuEncodeStageCount> encode_stage_worker_seconds{};
};

struct GpuTelemetry {
    std::atomic<std::uint64_t> encode_chunks{0};
    std::atomic<std::uint64_t> decode_chunks{0};
    std::atomic<std::uint64_t> kernel_launches{0};
    std::atomic<std::uint64_t> h2d_bytes{0};
    std::atomic<std::uint64_t> d2h_bytes{0};
    std::atomic<std::uint64_t> device_allocation_bytes{0};
    std::atomic<std::uint64_t> host_pinned_allocation_bytes{0};
    std::atomic<std::uint64_t> host_pinned_output_bytes{0};
    std::atomic<std::uint64_t> pattern_blocks{0};
    std::atomic<std::uint64_t> prefix_blocks{0};
    std::atomic<std::uint64_t> dictionary_blocks{0};
    std::atomic<std::uint64_t> sparse_pattern_blocks{0};
    std::atomic<std::uint64_t> kernel_microseconds{0};  // UINT64_MAX permanently marks unavailable timing.
    std::array<std::atomic<std::uint64_t>, kGpuEncodeStageCount> encode_stage_worker_microseconds{};
};

struct GpuCodecOptions {
    bool require_gpu = true;
    bool force_cpu = false;
    std::uint32_t block_size = kDefaultArchiveBlockBytes;
    std::uint32_t worker_count = 1;
    int compression_level = kDefaultCompressionLevel;
    std::shared_ptr<GpuTelemetry> telemetry;
    std::shared_ptr<HipHostOutputPool> host_output_pool;
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
    double kernel_ms = 0.0;  // NaN means event timing is unavailable; execution counters remain valid.
    double wall_seconds = 0.0;
};

struct DecodedChunkCrc {
    std::uint32_t crc32 = 0;
    bool gpu_used = false;
};

// Purpose: Snapshot per-operation AMD HIP telemetry into plain counters.
// Inputs: `telemetry` is the operation-owned counter set shared with codec tasks.
// Outputs: Returns operation counters; kernel_ms is NaN when any event time or its total was invalid.
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

// Purpose: Record operation-owned pinned output used by a successfully completed HIP decode.
// Inputs: telemetry is optional operation state; bytes is the allocation extent, not a live-memory measurement.
// Outputs: Adds cumulative successful pinned decode allocation bytes without altering device allocation counters.
void record_gpu_host_pinned_allocation_bytes(GpuTelemetry* telemetry, std::uint64_t bytes);

// Purpose: Record completed HIP output materialized into pinned host storage, including reused allocations.
// Inputs: telemetry is optional operation state; bytes is the exact completed decoded extent.
// Outputs: Adds output traffic without claiming a new allocation or live-memory measurement.
void record_gpu_host_pinned_output_bytes(GpuTelemetry* telemetry, std::uint64_t bytes);

// Purpose: Record GPU-compressed periodic pattern blocks emitted by the AMD HIP encoder.
// Inputs: `telemetry` is optional operation-owned telemetry and `count` is the number of compact pattern blocks.
// Outputs: Atomically adds the block count when telemetry is present.
void record_gpu_pattern_blocks(GpuTelemetry* telemetry, std::uint64_t count);

// Purpose: Record GPU-compressed static/adaptive prefix blocks emitted by the AMD HIP encoder.
// Inputs: `telemetry` is optional operation-owned telemetry and `count` is the number of compact prefix blocks.
// Outputs: Atomically adds the block count when telemetry is present.
void record_gpu_prefix_blocks(GpuTelemetry* telemetry, std::uint64_t count);

// Purpose: Record version-four dictionary blocks actually selected by the AMD HIP encoder.
// Inputs: Optional operation telemetry and the number of emitted dictionary blocks.
// Outputs: Atomically adds the selected block count when telemetry is present.
void record_gpu_dictionary_blocks(GpuTelemetry* telemetry, std::uint64_t count);

// Purpose: Record version-five sparse blocks selected by the AMD HIP encoder.
// Inputs: Optional operation telemetry and the number of emitted sparse blocks.
// Outputs: Atomically adds the selected block count when telemetry is present.
void record_gpu_sparse_pattern_blocks(GpuTelemetry* telemetry, std::uint64_t count);

// Purpose: Record one AMD HIP kernel launch and its device-event elapsed time.
// Inputs: `telemetry` is optional operation-owned telemetry and `milliseconds` is measured with HIP events.
// Outputs: Counts the launch even if timing is invalid; negative/non-finite/overflowing times mark the total
// unavailable.
void record_gpu_kernel_launch(GpuTelemetry* telemetry, double milliseconds);

// Purpose: Account for a measured multi-kernel HIP stage without inventing timings for each launch.
// Inputs: Optional operation telemetry, explicit SuperZip launch count, and complete device-stage milliseconds.
// Outputs: Adds the known launches and stage time; invalid timing marks the aggregate unavailable.
void record_gpu_kernel_work(GpuTelemetry* telemetry, std::uint32_t launches, double milliseconds);

// Purpose: Accumulate one HIP encode phase's host-observed worker time.
// Inputs: Optional operation telemetry, a named phase, and a nonnegative steady-clock duration.
// Outputs: Adds worker microseconds; concurrent phases may overlap and do not represent device time.
void record_gpu_encode_stage_time(GpuTelemetry* telemetry, GpuEncodeStage stage,
                                  std::chrono::steady_clock::duration elapsed);

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

inline constexpr std::size_t kMaxEncodeBatchBlocks = 256;

// Purpose: Keep independently checksummed input blocks together for one bounded codec submission.
// Inputs: Created by encode_owned_block_batch from a validated dense block layout.
// Outputs: Encoded descriptors and payload in input order, plus one source CRC per block.
struct EncodedBlockBatch {
    EncodedChunk encoded;
    std::vector<std::uint32_t> block_crc32;
};

// Purpose: Encode independent, variably sized blocks without padding or crossing their boundaries.
// Inputs: input owns dense bytes; positive block_lengths sum exactly to input size, with at most 256 blocks,
// each no larger than options.block_size. Options preserve the normal CPU/HIP requirement policy.
// Outputs: Returns one descriptor and CRC per input block; throws before dispatch for invalid resource/layout bounds.
EncodedBlockBatch encode_owned_block_batch(std::vector<std::byte> input, std::span<const std::uint32_t> block_lengths,
                                           const GpuCodecOptions& options);

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
