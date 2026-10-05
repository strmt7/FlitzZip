#pragma once

#include "core/archive_blocks.hpp"
#include "core/result.hpp"
#include "gpu/dictionary_matcher.hpp"

#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace superzip {

struct GpuTelemetry;

namespace dictionary {

using DictionaryReplacements = std::vector<std::vector<std::byte>>;

// Purpose: Admit complete Neutron source geometry independently of the optional HIP implementation.
// Inputs: Source extent, block descriptors and equally sized existing winner slots; no device access occurs.
// Outputs: Rejects zero, overflowing, incomplete or unrepresentable geometry before replacement mutation.
inline void validate_neutron_layout(std::size_t input_bytes, std::span<const BlockDescriptor> blocks,
                                    std::span<const std::vector<std::byte>> replacements) {
    if (blocks.size() != replacements.size()) {
        throw GpuError("Neutron replacement inputs are inconsistent");
    }
    std::size_t covered = 0U;
    for (std::size_t index = 0U; index < blocks.size(); ++index) {
        const auto bytes = static_cast<std::size_t>(blocks[index].uncompressed_len);
        if (bytes == 0U || covered > input_bytes || bytes > input_bytes - covered) {
            throw GpuError("Neutron replacement block exceeds its source chunk");
        }
        const auto baseline = replacements[index].empty() ? blocks[index].encoded_len : replacements[index].size();
        if (baseline > std::numeric_limits<std::uint32_t>::max()) {
            throw GpuError("Neutron replacement baseline exceeds native block limits");
        }
        covered += bytes;
    }
    if (covered != input_bytes) {
        throw GpuError("Neutron replacement blocks do not cover their source chunk");
    }
}

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
