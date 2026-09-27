#pragma once

#include "core/archive_blocks.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace superzip {

struct GpuTelemetry;

namespace dictionary {

using DictionaryReplacements = std::vector<std::vector<std::byte>>;

// Purpose: Compare bounded HIP dictionary payloads with existing GPU-native block costs.
// Inputs: Host/device mirrors, baseline descriptors, effort level, and optional operation telemetry.
// Outputs: Returns one optional smaller complete dictionary payload per source block.
DictionaryReplacements select_dictionary_replacements(std::span<const std::byte> input, const std::byte* device_input,
                                                      std::span<const BlockDescriptor> blocks, int level,
                                                      GpuTelemetry* telemetry);

// Purpose: Replace only proven-smaller blocks while preserving source CRC and block order.
// Inputs: A complete baseline chunk and corresponding optional dictionary payloads.
// Outputs: Returns a complete mixed encoded chunk with dense payload offsets or throws on inconsistent inputs.
EncodedChunk apply_dictionary_replacements(EncodedChunk baseline, const DictionaryReplacements& replacements);

}  // namespace dictionary
}  // namespace superzip
