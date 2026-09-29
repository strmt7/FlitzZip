#pragma once

#include "core/resource_limits.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace superzip {

// Purpose: Admit only complete, prefix-consistent version-eight Huffman lookup tables.
// Inputs: Exactly 4096 little-endian symbol/width entries from one encoded block.
// Outputs: Returns true only when every lookup slot is covered and each present symbol fills its prefix range.
inline bool huffman_lookup_is_complete(std::span<const std::byte> lookup) {
    if (lookup.size() != kGpuHuffmanLookupBytes) {
        return false;
    }
    std::array<std::uint8_t, 256> widths{};
    std::array<std::uint16_t, 256> prefixes{};
    std::array<std::uint16_t, 256> counts{};
    for (std::uint32_t slot = 0U; slot < kGpuHuffmanLookupEntries; ++slot) {
        const auto offset = slot * sizeof(std::uint16_t);
        const auto symbol = static_cast<std::uint8_t>(lookup[offset]);
        const auto width = static_cast<std::uint8_t>(lookup[offset + 1U]);
        if (width == 0U || width > kGpuHuffmanLookupBits) {
            return false;
        }
        const auto prefix = static_cast<std::uint16_t>(slot & ((1U << width) - 1U));
        if (widths[symbol] == 0U) {
            widths[symbol] = width;
            prefixes[symbol] = prefix;
        } else if (widths[symbol] != width || prefixes[symbol] != prefix) {
            return false;
        }
        ++counts[symbol];
    }
    for (std::size_t symbol = 0U; symbol < widths.size(); ++symbol) {
        if (widths[symbol] != 0U && counts[symbol] != kGpuHuffmanLookupEntries / (1U << widths[symbol])) {
            return false;
        }
    }
    return true;
}

}  // namespace superzip
