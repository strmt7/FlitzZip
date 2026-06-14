#include "core/checksum.hpp"

#include <array>

namespace superzip {
namespace {

// Purpose: Build the CRC-32 lookup table.
// Inputs: None.
// Outputs: Returns a 256-entry table for the ZIP-compatible polynomial.
std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < table.size(); ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
        }
        table[i] = crc;
    }
    return table;
}

// Purpose: Return the process-wide CRC-32 lookup table.
// Inputs: None.
// Outputs: Returns a reference to a lazily initialized immutable table.
const std::array<std::uint32_t, 256>& crc_table() {
    static const auto table = make_crc_table();
    return table;
}

}  // namespace

std::uint32_t crc32(std::span<const std::byte> bytes, std::uint32_t seed) {
    std::uint32_t crc = seed ^ 0xFFFFFFFFU;
    const auto& table = crc_table();
    for (const std::byte value : bytes) {
        const auto octet = static_cast<std::uint8_t>(value);
        crc = table[(crc ^ octet) & 0xFFU] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

}  // namespace superzip
