#pragma once

#include "core/archive_blocks.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace superzip {

struct GpuTelemetry;

namespace sparse_pattern {

using SparseReplacements = std::vector<std::vector<std::byte>>;

// Purpose: Evaluate version-five sparse payloads against every existing GPU-native block.
// Inputs: `input` and `device_input` mirror one chunk; `blocks` cover it in order; `telemetry` is optional.
// Outputs: Returns only complete byte-saving payloads, one optional replacement per block.
SparseReplacements select_replacements(std::span<const std::byte> input, const std::byte* device_input,
                                       std::span<const BlockDescriptor> blocks, GpuTelemetry* telemetry);

// Purpose: Publish only byte-saving sparse blocks while preserving unrelated encoded bytes and source CRC.
// Inputs: `baseline` is a complete encoded chunk and `replacements` has one entry per descriptor.
// Outputs: Returns a dense mixed chunk or throws `GpuError` on inconsistent extents.
EncodedChunk apply_replacements(EncodedChunk baseline, const SparseReplacements& replacements);

}  // namespace sparse_pattern
}  // namespace superzip
