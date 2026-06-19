#include "gpu/gpu_codec.hpp"

#include "core/archive_blocks.hpp"
#include "core/checksum.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace superzip {
namespace {

// Purpose: Convert backend-selection options to CPU archive codec options.
// Inputs: `options` contains GPU requirement flags plus shared codec tuning.
// Outputs: Returns the CPU codec subset used for forced-CPU operations and optional CPU fallback.
ArchiveCodecOptions archive_codec_options(const GpuCodecOptions& options) {
    return ArchiveCodecOptions{
        .block_size = options.block_size,
        .worker_count = options.worker_count,
        .compression_level = options.compression_level,
    };
}

// Purpose: Validate backend-selection options before dispatching to CPU or AMD HIP.
// Inputs: `options` supplies shared block size and compression effort.
// Outputs: Returns normally for bounded settings; throws `ArchiveError` otherwise.
void validate_gpu_codec_options(const GpuCodecOptions& options) {
    if (options.block_size < kMinArchiveBlockBytes || options.block_size > kMaxArchiveBlockBytes) {
        throw ArchiveError("codec block size is outside SuperZip resource limits");
    }
    if (options.compression_level < kMinCompressionLevel || options.compression_level > kMaxCompressionLevel) {
        throw ArchiveError("codec compression level must be between 1 and 9");
    }
}

// Purpose: Reject direct codec spans that exceed the bounded archive chunk contract.
// Inputs: `size` is a caller-provided span length and `label` names the span.
// Outputs: Returns normally while bounded; throws `ArchiveError` before large allocations.
void reject_oversized_codec_span(std::size_t size, const char* label) {
    if (size > kMaxArchiveChunkBytes) {
        throw ArchiveError(std::string(label) + " exceeds SuperZip codec resource limit");
    }
}

#if SUPERZIP_ENABLE_HIP
// Purpose: Reject CPU-deflate block tables before entering the AMD HIP-only decode path.
// Inputs: `blocks` is the archive chunk block table and `action` labels the failing operation.
// Outputs: Returns for HIP-supported block kinds; throws `GpuError` for CPU-deflate data.
void reject_deflate_blocks_for_hip(std::span<const BlockDescriptor> blocks, const char* action) {
    if (block_table_contains_deflate(blocks)) {
        throw GpuError(
            std::string("AMD HIP ") + action +
            " cannot process CPU-deflate SUZIP blocks; use the CPU codec or recreate the archive with the HIP codec");
    }
}

// Purpose: Add one telemetry counter into another without losing concurrent atomic updates.
// Inputs: `counter` is the destination telemetry field and `value` is the source count.
// Outputs: Atomically adds nonzero source values to the operation telemetry.
void merge_gpu_counter(std::atomic<std::uint64_t>& counter, std::uint64_t value) {
    if (value != 0U) {
        counter.fetch_add(value, std::memory_order_relaxed);
    }
}

// Purpose: Merge a successful isolated HIP attempt into operation-level telemetry.
// Inputs: `target` is the caller-visible telemetry; `source` is an attempt-local telemetry set.
// Outputs: Adds only successful HIP execution counters to final operation statistics.
void merge_successful_gpu_attempt(GpuTelemetry* target, const GpuTelemetry& source) {
    if (!target) {
        return;
    }
    merge_gpu_counter(target->encode_chunks, source.encode_chunks.load(std::memory_order_relaxed));
    merge_gpu_counter(target->decode_chunks, source.decode_chunks.load(std::memory_order_relaxed));
    merge_gpu_counter(target->kernel_launches, source.kernel_launches.load(std::memory_order_relaxed));
    merge_gpu_counter(target->h2d_bytes, source.h2d_bytes.load(std::memory_order_relaxed));
    merge_gpu_counter(target->d2h_bytes, source.d2h_bytes.load(std::memory_order_relaxed));
    merge_gpu_counter(target->device_allocation_bytes, source.device_allocation_bytes.load(std::memory_order_relaxed));
    merge_gpu_counter(target->pattern_blocks, source.pattern_blocks.load(std::memory_order_relaxed));
    merge_gpu_counter(target->prefix_blocks, source.prefix_blocks.load(std::memory_order_relaxed));
    merge_gpu_counter(target->kernel_microseconds, source.kernel_microseconds.load(std::memory_order_relaxed));
}

// Purpose: Route optional HIP work through temporary telemetry so failed attempts do not pollute CPU fallback stats.
// Inputs: `options` is the requested codec configuration; `attempt_telemetry` receives temporary counters when needed.
// Outputs: Returns codec options for the HIP attempt.
GpuCodecOptions gpu_attempt_options(const GpuCodecOptions& options, std::shared_ptr<GpuTelemetry>& attempt_telemetry) {
    if (options.require_gpu || !options.telemetry) {
        return options;
    }
    attempt_telemetry = std::make_shared<GpuTelemetry>();
    GpuCodecOptions attempt = options;
    attempt.telemetry = attempt_telemetry;
    return attempt;
}

// Purpose: Publish isolated HIP telemetry after the attempt succeeds as a complete HIP codec operation.
// Inputs: `target` is the final operation telemetry; `attempt_telemetry` is null for required-GPU or untracked
// operations. Outputs: Merges temporary counters only for successful optional HIP work.
void publish_successful_gpu_attempt(GpuTelemetry* target, const std::shared_ptr<GpuTelemetry>& attempt_telemetry) {
    if (attempt_telemetry) {
        merge_successful_gpu_attempt(target, *attempt_telemetry);
    }
}
#endif

}  // namespace

// Purpose: Inspect the AMD HIP runtime/device from the HIP translation unit.
// Inputs: None.
// Outputs: Returns runtime and device status without throwing for ordinary absence.
GpuInfo query_hip_gpu_info();

// Purpose: Run the standalone AMD HIP diagnostic from the HIP translation unit.
// Inputs: `options` controls duration, buffer size, and arithmetic intensity.
// Outputs: Returns HIP telemetry and checksum data; throws `GpuError` if HIP cannot run.
GpuDiagnosticResult run_gpu_diagnostic_hip(const GpuDiagnosticOptions& options);

// Purpose: Encode one archive chunk through the AMD HIP classifier.
// Inputs: `input` is uncompressed bytes and `options` supplies GPU flags, block size, and telemetry.
// Outputs: Returns HIP-classified blocks/payload and source CRC; throws `GpuError` when HIP is required but
// unavailable.
EncodedChunk encode_chunk_hip(std::span<const std::byte> input, const GpuCodecOptions& options);

// Purpose: Encode an owned archive chunk through AMD HIP with zero-copy raw payload publication when possible.
// Inputs: `input` owns the uncompressed bytes and remains usable if HIP throws before a successful result.
// Outputs: Returns HIP-classified blocks/payload and source CRC; may move from `input` only after success.
EncodedChunk encode_owned_chunk_hip(std::vector<std::byte>& input, const GpuCodecOptions& options);

// Purpose: Materialize one encoded chunk through the AMD HIP decoder.
// Inputs: `payload`/`blocks` describe encoded bytes, `output` is the exact decoded buffer, and `options` supplies
// telemetry. Outputs: Writes decoded bytes into `output`; throws on invalid layout or unavailable HIP.
void decode_chunk_hip(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                      std::span<std::byte> output, const GpuCodecOptions& options);

// Purpose: Compute decoded chunk CRC through the AMD HIP materialize-and-CRC path.
// Inputs: `payload`/`blocks` describe encoded bytes, `output_size` is decoded length, and `options` supplies telemetry.
// Outputs: Returns ZIP-compatible CRC-32; throws when the GPU CRC-only path cannot process the block table.
std::uint32_t crc_decoded_chunk_hip(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                                    std::uint64_t output_size, const GpuCodecOptions& options);

GpuRuntimeStats snapshot_gpu_telemetry(const GpuTelemetry& telemetry) {
    return GpuRuntimeStats{
        .encode_chunks = telemetry.encode_chunks.load(std::memory_order_relaxed),
        .decode_chunks = telemetry.decode_chunks.load(std::memory_order_relaxed),
        .kernel_launches = telemetry.kernel_launches.load(std::memory_order_relaxed),
        .h2d_bytes = telemetry.h2d_bytes.load(std::memory_order_relaxed),
        .d2h_bytes = telemetry.d2h_bytes.load(std::memory_order_relaxed),
        .device_allocation_bytes = telemetry.device_allocation_bytes.load(std::memory_order_relaxed),
        .pattern_blocks = telemetry.pattern_blocks.load(std::memory_order_relaxed),
        .prefix_blocks = telemetry.prefix_blocks.load(std::memory_order_relaxed),
        .kernel_ms = static_cast<double>(telemetry.kernel_microseconds.load(std::memory_order_relaxed)) / 1000.0,
    };
}

void record_gpu_encode_chunk(GpuTelemetry* telemetry) {
    if (telemetry) {
        telemetry->encode_chunks.fetch_add(1, std::memory_order_relaxed);
    }
}

void record_gpu_decode_chunk(GpuTelemetry* telemetry) {
    if (telemetry) {
        telemetry->decode_chunks.fetch_add(1, std::memory_order_relaxed);
    }
}

void record_gpu_h2d_bytes(GpuTelemetry* telemetry, std::uint64_t bytes) {
    if (telemetry) {
        telemetry->h2d_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
}

void record_gpu_d2h_bytes(GpuTelemetry* telemetry, std::uint64_t bytes) {
    if (telemetry) {
        telemetry->d2h_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
}

void record_gpu_device_allocation_bytes(GpuTelemetry* telemetry, std::uint64_t bytes) {
    if (telemetry) {
        telemetry->device_allocation_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
}

void record_gpu_pattern_blocks(GpuTelemetry* telemetry, std::uint64_t count) {
    if (telemetry) {
        telemetry->pattern_blocks.fetch_add(count, std::memory_order_relaxed);
    }
}

// Purpose: Add GPU static-prefix block count telemetry for one archive operation.
// Inputs: `telemetry` may be null and `count` is the number of prefix-compressed blocks emitted.
// Outputs: Atomically updates telemetry when present.
void record_gpu_prefix_blocks(GpuTelemetry* telemetry, std::uint64_t count) {
    if (telemetry) {
        telemetry->prefix_blocks.fetch_add(count, std::memory_order_relaxed);
    }
}

void record_gpu_kernel_launch(GpuTelemetry* telemetry, double milliseconds) {
    if (telemetry) {
        telemetry->kernel_launches.fetch_add(1, std::memory_order_relaxed);
        const auto microseconds = static_cast<std::uint64_t>((milliseconds * 1000.0) + 0.5);
        telemetry->kernel_microseconds.fetch_add(microseconds, std::memory_order_relaxed);
    }
}

GpuInfo query_gpu_info() {
#if SUPERZIP_ENABLE_HIP
    return query_hip_gpu_info();
#else
    return GpuInfo{
        .hip_compiled = false,
        .available = false,
        .status = "Built without HIP acceleration",
    };
#endif
}

GpuDiagnosticResult run_gpu_diagnostic(const GpuDiagnosticOptions& options) {
    if (options.seconds < 1.0 || options.seconds > 30.0) {
        throw ArchiveError("GPU diagnostic seconds must be between 1 and 30");
    }
    if (options.buffer_mib < 16U || options.buffer_mib > 512U) {
        throw ArchiveError("GPU diagnostic buffer must be between 16 and 512 MiB");
    }
    if (options.inner_iterations == 0U || options.inner_iterations > 4096U) {
        throw ArchiveError("GPU diagnostic inner iterations must be between 1 and 4096");
    }
#if SUPERZIP_ENABLE_HIP
    return run_gpu_diagnostic_hip(options);
#else
    throw GpuError("SuperZip was built without HIP acceleration");
#endif
}

EncodedChunk encode_chunk(std::span<const std::byte> input, const GpuCodecOptions& options) {
    validate_gpu_codec_options(options);
    reject_oversized_codec_span(input.size(), "codec input");
    const auto cpu_options = archive_codec_options(options);

#if SUPERZIP_ENABLE_HIP
    if (!options.force_cpu) {
        std::shared_ptr<GpuTelemetry> attempt_telemetry;
        const auto hip_options = gpu_attempt_options(options, attempt_telemetry);
        try {
            auto encoded = encode_chunk_hip(input, hip_options);
            encoded.gpu_used = true;
            publish_successful_gpu_attempt(options.telemetry.get(), attempt_telemetry);
            return encoded;
        } catch (const GpuError&) {
            if (options.require_gpu) {
                throw;
            }
        }
    } else if (options.require_gpu) {
        throw GpuError("cannot require AMD HIP while forcing the CPU codec");
    }
#else
    if (options.require_gpu) {
        throw GpuError("SuperZip was built without HIP acceleration");
    }
#endif
    return encode_chunk_cpu(input, cpu_options);
}

// Purpose: Encode owned chunk memory through HIP when available and preserve CPU fallback semantics.
// Inputs: `input` owns uncompressed bytes and `options` selects backend, block size, workers, and telemetry.
// Outputs: Returns encoded blocks, payload, and source CRC; may move raw HIP payload bytes from `input`.
EncodedChunk encode_owned_chunk(std::vector<std::byte> input, const GpuCodecOptions& options) {
    validate_gpu_codec_options(options);
    reject_oversized_codec_span(input.size(), "codec input");
    const auto cpu_options = archive_codec_options(options);

#if SUPERZIP_ENABLE_HIP
    if (!options.force_cpu) {
        std::shared_ptr<GpuTelemetry> attempt_telemetry;
        const auto hip_options = gpu_attempt_options(options, attempt_telemetry);
        try {
            auto encoded = encode_owned_chunk_hip(input, hip_options);
            encoded.gpu_used = true;
            publish_successful_gpu_attempt(options.telemetry.get(), attempt_telemetry);
            return encoded;
        } catch (const GpuError&) {
            if (options.require_gpu) {
                throw;
            }
        }
    } else if (options.require_gpu) {
        throw GpuError("cannot require AMD HIP while forcing the CPU codec");
    }
#else
    if (options.require_gpu) {
        throw GpuError("SuperZip was built without HIP acceleration");
    }
#endif
    auto encoded = encode_chunk_cpu(std::span<const std::byte>(input.data(), input.size()), cpu_options);
    encoded.source_crc32 = crc32(std::span<const std::byte>(input.data(), input.size()));
    encoded.source_crc32_available = true;
    return encoded;
}

// Purpose: Decode one native SUZIP chunk through HIP when allowed, otherwise through the CPU codec.
// Inputs: `payload`/`blocks` describe encoded bytes, `output` is exact decoded storage, and `options` selects backend.
// Outputs: Writes `output` and returns true for successful HIP execution; throws on malformed metadata or required-GPU
// absence.
bool decode_chunk(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                  std::span<std::byte> output, const GpuCodecOptions& options) {
    validate_gpu_codec_options(options);
    reject_oversized_codec_span(payload.size(), "codec payload");
    reject_oversized_codec_span(output.size(), "codec output");
    if (blocks.size() > kMaxBlocksPerEntry) {
        throw ArchiveError("codec block table exceeds SuperZip resource limit");
    }
    const auto cpu_options = archive_codec_options(options);

#if SUPERZIP_ENABLE_HIP
    if (!options.force_cpu) {
        std::shared_ptr<GpuTelemetry> attempt_telemetry;
        const auto hip_options = gpu_attempt_options(options, attempt_telemetry);
        try {
            reject_deflate_blocks_for_hip(blocks, "decode");
            decode_chunk_hip(payload, blocks, output, hip_options);
            publish_successful_gpu_attempt(options.telemetry.get(), attempt_telemetry);
            return true;
        } catch (const GpuError&) {
            if (options.require_gpu) {
                throw;
            }
        }
    } else if (options.require_gpu) {
        throw GpuError("cannot require AMD HIP while forcing the CPU codec");
    }
#else
    if (options.require_gpu) {
        throw GpuError("SuperZip was built without HIP acceleration");
    }
#endif
    decode_chunk_cpu(payload, blocks, output, cpu_options);
    return false;
}

// Purpose: Compute one decoded SUZIP chunk CRC through HIP when allowed, otherwise through CPU decode and CRC.
// Inputs: `payload`/`blocks` describe encoded bytes, `output_size` is decoded bytes, and `options` selects backend.
// Outputs: Returns CRC plus GPU-use flag; throws on malformed metadata, oversized output, or required-GPU absence.
DecodedChunkCrc crc_decoded_chunk(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                                  std::uint64_t output_size, const GpuCodecOptions& options) {
    validate_gpu_codec_options(options);
    reject_oversized_codec_span(payload.size(), "codec payload");
    if (output_size > kMaxArchiveChunkBytes) {
        throw ArchiveError("codec output exceeds SuperZip resource limit");
    }
    if (blocks.size() > kMaxBlocksPerEntry) {
        throw ArchiveError("codec block table exceeds SuperZip resource limit");
    }
    const auto cpu_options = archive_codec_options(options);

#if SUPERZIP_ENABLE_HIP
    if (!options.force_cpu) {
        std::shared_ptr<GpuTelemetry> attempt_telemetry;
        const auto hip_options = gpu_attempt_options(options, attempt_telemetry);
        try {
            reject_deflate_blocks_for_hip(blocks, "CRC verification");
            auto decoded = DecodedChunkCrc{
                .crc32 = crc_decoded_chunk_hip(payload, blocks, output_size, hip_options),
                .gpu_used = true,
            };
            publish_successful_gpu_attempt(options.telemetry.get(), attempt_telemetry);
            return decoded;
        } catch (const GpuError&) {
            if (options.require_gpu) {
                throw;
            }
        }
    } else if (options.require_gpu) {
        throw GpuError("cannot require AMD HIP while forcing the CPU codec");
    }
#else
    if (options.require_gpu) {
        throw GpuError("SuperZip was built without HIP acceleration");
    }
#endif
    std::vector<std::byte> decoded(static_cast<std::size_t>(output_size));
    decode_chunk_cpu(payload, blocks, decoded, cpu_options);
    return DecodedChunkCrc{
        .crc32 = crc32(std::span<const std::byte>(decoded.data(), decoded.size())),
        .gpu_used = false,
    };
}

}  // namespace superzip
