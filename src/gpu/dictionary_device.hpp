#pragma once

#include "core/dictionary_block.hpp"
#include "gpu/dictionary_matcher.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace superzip {

struct GpuTelemetry;

namespace dictionary {

// Production batches keep downloaded segments contiguous until archive block framing.
struct PackedEncodedBatch {
    std::vector<std::byte> payload;
    std::vector<std::uint32_t> segment_sizes;
    EncodedBatch telemetry;
};

// Purpose: Encode a bounded dictionary batch using source bytes already resident on the HIP device.
// Inputs: `input` mirrors `device_input`, effort is validated, and optional distances cover every segment;
// distance-admitted batches may be at most 16 MiB, while the general index remains capped at 4 MiB.
// Outputs: Returns contiguous independent LZ4 block payloads without another source upload.
PackedEncodedBatch encode_segments_from_device_hip(std::span<const std::byte> input, const std::byte* device_input,
                                                   const Effort& effort,
                                                   std::span<const std::uint16_t> periodic_distances = {});

// Purpose: Execute minimum-byte parsing on a bounded source already resident on HIP.
// Inputs: At most 1 MiB host/device mirrors and an optional cancellation checkpoint.
// Outputs: Returns complete LZ4 segments with actual launch, allocation and transfer counts; no CPU encoding.
PackedEncodedBatch encode_neutron_segments_from_device_hip(std::span<const std::byte> input,
                                                           const std::byte* device_input,
                                                           const EncodeCheckpoint& checkpoint = {});

// Purpose: Decode already admitted independent LZ4 segments into caller-owned HIP output memory.
// Inputs: `encoded` and `decoded` are live device pointers; `spans` are validated absolute device-buffer extents.
// Outputs: Writes all segments and records HIP activity, or throws before any partial output reaches the host.
void decode_segments_device(const std::byte* encoded, std::span<const DictionarySegmentSpan> spans, std::byte* decoded,
                            GpuTelemetry* telemetry);

}  // namespace dictionary
}  // namespace superzip
