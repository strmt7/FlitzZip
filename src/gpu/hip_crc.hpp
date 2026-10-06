#pragma once

#include "gpu/hip_codec_support.hpp"

namespace superzip::hip_detail {

// Purpose: Checksum bounded device input using compact ordered CRC metadata.
// Inputs: Live device bytes, extent, telemetry and diagnostic action.
// Outputs: Returns CRC-32; host code owns all temporary buffers and checked errors.
std::uint32_t compute_crc32_device(const std::byte* device_input, std::uint64_t input_len, GpuTelemetry* telemetry,
                                   const char* action);

// Purpose: Checksum each independent block in a validated device batch.
// Inputs: Live concatenated device bytes, exact block extents and telemetry.
// Outputs: Ordered CRC-32 values after one kernel launch and compact metadata readback.
std::vector<std::uint32_t> compute_block_crc32_device(const std::byte* device_input,
                                                      std::span<const std::uint32_t> lengths, GpuTelemetry* telemetry);

// Purpose: Checksum decoded block semantics directly on the device.
// Inputs: Admitted payload/table, block count, decoded extent, telemetry and diagnostic action.
// Outputs: Combined CRC-32 with only compact segment metadata copied back.
std::uint32_t compute_decoded_crc32_device(const std::byte* device_payload, const DeviceBlock* device_blocks,
                                           std::uint32_t block_count, std::uint64_t output_len, GpuTelemetry* telemetry,
                                           const char* action);

}  // namespace superzip::hip_detail
