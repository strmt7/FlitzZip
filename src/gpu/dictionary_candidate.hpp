#pragma once

#include "core/archive_blocks.hpp"
#include "gpu/dictionary_matcher.hpp"

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

// Purpose: Improve the complete level-nine GPU portfolio using bounded minimum-byte dictionary trials.
// Inputs: Exact host/device mirrors, competitive block costs, original dictionary winners and a throwing checkpoint.
// Outputs: Replaces only proven-smaller complete payloads; preserves every baseline winner and all block CRC semantics.
void improve_neutron_replacements(std::span<const std::byte> input, const std::byte* device_input,
                                  std::span<const BlockDescriptor> blocks, DictionaryReplacements& replacements,
                                  GpuTelemetry* telemetry, const EncodeCheckpoint& checkpoint);

// Purpose: Replace only proven-smaller blocks while preserving source CRC and block order.
// Inputs: A complete baseline chunk and corresponding optional dictionary payloads.
// Outputs: Returns a complete mixed encoded chunk with dense payload offsets or throws on inconsistent inputs.
EncodedChunk apply_dictionary_replacements(EncodedChunk baseline, const DictionaryReplacements& replacements);

}  // namespace dictionary
}  // namespace superzip
