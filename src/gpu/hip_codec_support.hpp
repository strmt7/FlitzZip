#pragma once

#include "gpu/gpu_codec.hpp"

#include "core/result.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <hip/hip_runtime.h>

namespace superzip::hip_detail {

constexpr std::uint32_t kPatternSampleBytes = 4096U;
constexpr std::uint32_t kAnalyzeSegmentBytes = 64U * 1024U;
constexpr std::uint32_t kCrcSegmentBytes = 64U * 1024U;
constexpr std::uint32_t kMaterializeSegmentBytes = 64U * 1024U;

struct DeviceBlock {
    std::uint8_t kind;
    std::uint8_t fill_value;
    std::uint16_t reserved;
    std::uint32_t uncompressed_len;
    std::uint64_t encoded_offset;
    std::uint64_t output_offset;
    std::uint32_t encoded_len;
};

struct DeviceCrcSegment {
    std::uint32_t crc32;
    std::uint32_t length;
};

struct HipEventPair {
    hipEvent_t start = nullptr;
    hipEvent_t stop = nullptr;

    HipEventPair() = default;
    HipEventPair(const HipEventPair&) = delete;
    HipEventPair& operator=(const HipEventPair&) = delete;

    HipEventPair(HipEventPair&& other) noexcept : start(other.start), stop(other.stop) {
        other.start = nullptr;
        other.stop = nullptr;
    }

    HipEventPair& operator=(HipEventPair&& other) noexcept {
        if (this != &other) {
            if (start != nullptr) {
                (void)hipEventDestroy(start);
            }
            if (stop != nullptr) {
                (void)hipEventDestroy(stop);
            }
            start = other.start;
            stop = other.stop;
            other.start = nullptr;
            other.stop = nullptr;
        }
        return *this;
    }

    ~HipEventPair() {
        if (start != nullptr) {
            (void)hipEventDestroy(start);
        }
        if (stop != nullptr) {
            (void)hipEventDestroy(stop);
        }
    }
};

// Purpose: Convert a HIP status into a SuperZip GPU exception.
// Inputs: `status` is a HIP API result and `action` labels the failing operation.
// Outputs: Returns on success; throws `GpuError` with the HIP error string on failure.
inline void check_hip(hipError_t status, const char* action) {
    if (status != hipSuccess) {
        throw GpuError(std::string(action) + ": " + hipGetErrorString(status));
    }
}

// Purpose: Create a start/stop HIP event pair for measuring device-side kernel time.
// Inputs: `action` labels diagnostics if event creation fails.
// Outputs: Returns owned HIP events that are destroyed automatically.
inline HipEventPair make_hip_event_pair(const char* action) {
    HipEventPair events;
    check_hip(hipEventCreate(&events.start), action);
    check_hip(hipEventCreate(&events.stop), action);
    return events;
}

// Purpose: Finish a measured HIP kernel interval and record telemetry.
// Inputs: `telemetry` is optional operation-owned counters, `events` contains recorded start/stop events, and `action`
// labels diagnostics. Outputs: Synchronizes the stop event, records one launch plus elapsed device milliseconds, and
// throws on HIP errors.
inline void finish_measured_kernel(GpuTelemetry* telemetry, const HipEventPair& events, const char* action) {
    check_hip(hipEventSynchronize(events.stop), action);
    float milliseconds = 0.0F;
    check_hip(hipEventElapsedTime(&milliseconds, events.start, events.stop), action);
    record_gpu_kernel_launch(telemetry, static_cast<double>(milliseconds));
}

// Purpose: Add two allocation byte counts with overflow detection.
// Inputs: `lhs` and `rhs` are host/device allocation sizes; `action` labels the failing GPU operation.
// Outputs: Returns the sum or throws `GpuError` before wraparound.
inline std::size_t checked_add_bytes(std::size_t lhs, std::size_t rhs, const char* action) {
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        throw GpuError(std::string(action) + ": allocation size overflow");
    }
    return lhs + rhs;
}

// Purpose: Multiply allocation element count by element size with overflow detection.
// Inputs: `count`, `bytes_per_item`, and `action` describe a device allocation.
// Outputs: Returns byte size or throws `GpuError` before wraparound.
inline std::size_t checked_multiply_bytes(std::size_t count, std::size_t bytes_per_item, const char* action) {
    if (bytes_per_item != 0 && count > std::numeric_limits<std::size_t>::max() / bytes_per_item) {
        throw GpuError(std::string(action) + ": allocation size overflow");
    }
    return count * bytes_per_item;
}

// Purpose: Ensure a HIP operation leaves reserved VRAM for the OS, display, and other processes.
// Inputs: `required_bytes` is the total planned device allocation and `action` labels the operation.
// Outputs: Returns normally when free VRAM can cover the allocation plus reserve; throws `GpuError` otherwise.
inline void ensure_device_memory_budget(std::size_t required_bytes, const char* action) {
    std::size_t free_bytes = 0;
    std::size_t total_bytes = 0;
    check_hip(hipMemGetInfo(&free_bytes, &total_bytes), "hipMemGetInfo");
    const auto reserve_target =
        std::max<std::size_t>(static_cast<std::size_t>(kDeviceMemoryReserveFloorBytes), total_bytes / 20U);
    const auto reserve = std::min<std::size_t>(free_bytes / 4U, reserve_target);
    const auto usable_bytes = free_bytes - reserve;
    if (required_bytes > usable_bytes) {
        throw GpuError(std::string(action) + ": insufficient AMD GPU VRAM for bounded chunk");
    }
}

// Purpose: Validate block layout before launching the HIP decode kernel.
// Inputs: `payload`, `blocks`, `output`, and `block_size` are caller-provided decode spans.
// Outputs: Returns normally for a dense fill/raw layout; throws `ArchiveError` before any kernel can read out of
// bounds.
inline void validate_decode_layout(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                                   std::size_t output_len, std::uint32_t /*block_size*/) {
    if (blocks.empty() && output_len != 0) {
        throw ArchiveError("decode block table is empty");
    }
    std::size_t out_pos = 0;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const auto& block = blocks[i];
        const auto len = static_cast<std::size_t>(block.uncompressed_len);
        if (len == 0) {
            throw ArchiveError("decode block has zero length");
        }
        if (len > kMaxArchiveBlockBytes) {
            throw ArchiveError("decode block exceeds SuperZip block size limit");
        }
        if (out_pos > output_len || len > output_len - out_pos) {
            throw ArchiveError("decode block exceeds output buffer");
        }
        if (block.kind == BlockKind::Fill) {
            if (block.encoded_len != 0) {
                throw ArchiveError("fill block contains encoded payload bytes");
            }
        } else if (block.kind == BlockKind::Raw) {
            if (block.encoded_len != block.uncompressed_len || block.encoded_offset > payload.size()) {
                throw ArchiveError("raw decode block metadata is invalid");
            }
            const auto offset = static_cast<std::size_t>(block.encoded_offset);
            const auto encoded_len = static_cast<std::size_t>(block.encoded_len);
            if (encoded_len > payload.size() - offset) {
                throw ArchiveError("raw decode block exceeds payload buffer");
            }
        } else if (block.kind == BlockKind::Deflate) {
            if (block.encoded_len == 0 || block.encoded_offset > payload.size()) {
                throw ArchiveError("deflate decode block metadata is invalid");
            }
            const auto offset = static_cast<std::size_t>(block.encoded_offset);
            const auto encoded_len = static_cast<std::size_t>(block.encoded_len);
            if (encoded_len > payload.size() - offset) {
                throw ArchiveError("deflate decode block exceeds payload buffer");
            }
        } else if (block.kind == BlockKind::Pattern) {
            if (block.encoded_len < 2 || block.encoded_len > kMaxGpuPatternBytes ||
                block.encoded_len >= block.uncompressed_len || block.encoded_offset > payload.size()) {
                throw ArchiveError("GPU pattern decode block metadata is invalid");
            }
            const auto offset = static_cast<std::size_t>(block.encoded_offset);
            const auto encoded_len = static_cast<std::size_t>(block.encoded_len);
            if (encoded_len > payload.size() - offset) {
                throw ArchiveError("GPU pattern decode block exceeds payload buffer");
            }
        } else {
            throw ArchiveError("decode block has unknown encoding kind");
        }
        out_pos += len;
    }
    if (out_pos != output_len) {
        throw ArchiveError("decode block sizes do not match output buffer");
    }
}

// Purpose: Detect decoded streams that can be checksummed directly from the encoded raw payload.
// Inputs: `payload`/`blocks` are validated archive metadata and `output_size` is the decoded byte count.
// Outputs: Returns true only when the first `output_size` payload bytes are exactly the decoded stream.
inline bool decoded_crc_uses_contiguous_raw_payload(std::span<const std::byte> payload,
                                                    std::span<const BlockDescriptor> blocks,
                                                    std::uint64_t output_size) {
    if (payload.size() < output_size) {
        return false;
    }
    std::uint64_t decoded_offset = 0;
    for (const auto& block : blocks) {
        if (block.kind != BlockKind::Raw || block.encoded_offset != decoded_offset ||
            block.encoded_len != block.uncompressed_len) {
            return false;
        }
        if (block.uncompressed_len > output_size - decoded_offset) {
            return false;
        }
        decoded_offset += block.uncompressed_len;
    }
    return decoded_offset == output_size;
}

// Purpose: Detect a possible fill block from a bounded host sample before GPU verification.
// Inputs: `bytes` is one archive block and `value` receives the sampled repeated byte.
// Outputs: Returns true when the sampled prefix is uniform; the GPU still verifies the full block.
inline bool sampled_block_is_fill(std::span<const std::byte> bytes, std::uint8_t& value) {
    if (bytes.empty()) {
        value = 0;
        return true;
    }
    value = static_cast<std::uint8_t>(bytes.front());
    const auto sample_len = std::min<std::size_t>(bytes.size(), kPatternSampleBytes);
    for (std::size_t index = 1; index < sample_len; ++index) {
        if (static_cast<std::uint8_t>(bytes[index]) != value) {
            return false;
        }
    }
    return true;
}

// Purpose: Check whether a sampled prefix repeats with one candidate period.
// Inputs: `bytes` is a sampled archive block prefix and `period` is the candidate byte period.
// Outputs: Returns true when every sampled byte follows the period.
inline bool sampled_period_matches(std::span<const std::byte> bytes, std::uint32_t period) {
    for (std::size_t index = period; index < bytes.size(); ++index) {
        if (bytes[index] != bytes[index % period]) {
            return false;
        }
    }
    return true;
}

// Purpose: Pick one short repeated-pattern candidate from a bounded host sample.
// Inputs: `bytes` is one non-fill archive block.
// Outputs: Returns a 2..kMaxGpuPatternBytes period for GPU verification, or zero when no sampled period exists.
inline std::uint16_t sampled_pattern_period(std::span<const std::byte> bytes) {
    if (bytes.size() < 4U) {
        return 0;
    }
    const auto sample_len = std::min<std::size_t>(bytes.size(), kPatternSampleBytes);
    const auto sample = bytes.first(sample_len);
    const auto max_period =
        std::min<std::size_t>({kMaxGpuPatternBytes, bytes.size() / 2U, sample_len > 0U ? sample_len - 1U : 0U});
    for (std::uint32_t period = 2U; period <= max_period; ++period) {
        if (sample[period] != sample.front()) {
            continue;
        }
        if (sampled_period_matches(sample, period)) {
            return static_cast<std::uint16_t>(period);
        }
    }
    return 0;
}

// Purpose: Build per-block encode candidates that the GPU can verify in fixed-size parallel tiles.
// Inputs: `input`, `block_size`, and `block_count` describe one bounded archive chunk.
// Outputs: Returns raw/fill/pattern candidates; non-raw candidates are provisional until HIP verification succeeds.
inline std::vector<DeviceBlock> build_encode_analysis_candidates(std::span<const std::byte> input,
                                                                 std::uint32_t block_size, std::uint32_t block_count) {
    std::vector<DeviceBlock> candidates;
    candidates.reserve(block_count);
    for (std::uint32_t block_index = 0; block_index < block_count; ++block_index) {
        const auto start = static_cast<std::size_t>(block_index) * block_size;
        const auto len = static_cast<std::uint32_t>(std::min<std::size_t>(block_size, input.size() - start));
        const auto block = input.subspan(start, len);
        std::uint8_t fill = 0;
        if (sampled_block_is_fill(block, fill)) {
            candidates.push_back(DeviceBlock{
                .kind = static_cast<std::uint8_t>(BlockKind::Fill),
                .fill_value = fill,
                .reserved = 0,
                .uncompressed_len = len,
                .encoded_offset = 0,
                .output_offset = 0,
                .encoded_len = 0,
            });
            continue;
        }
        if (const auto period = sampled_pattern_period(block); period != 0U) {
            candidates.push_back(DeviceBlock{
                .kind = static_cast<std::uint8_t>(BlockKind::Pattern),
                .fill_value = 0,
                .reserved = 0,
                .uncompressed_len = len,
                .encoded_offset = 0,
                .output_offset = 0,
                .encoded_len = period,
            });
            continue;
        }
        candidates.push_back(DeviceBlock{
            .kind = static_cast<std::uint8_t>(BlockKind::Raw),
            .fill_value = 0,
            .reserved = 0,
            .uncompressed_len = len,
            .encoded_offset = 0,
            .output_offset = 0,
            .encoded_len = len,
        });
    }
    return candidates;
}

// Purpose: Determine whether any provisional block needs full HIP verification.
// Inputs: `candidates` is the host-built candidate block table.
// Outputs: Returns true when at least one fill or pattern candidate must be verified on the GPU.
inline bool has_non_raw_analysis_candidate(std::span<const DeviceBlock> candidates) {
    for (const auto& candidate : candidates) {
        if (candidate.kind != static_cast<std::uint8_t>(BlockKind::Raw)) {
            return true;
        }
    }
    return false;
}

}  // namespace superzip::hip_detail
