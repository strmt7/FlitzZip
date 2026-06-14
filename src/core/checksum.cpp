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

// Purpose: Multiply a GF(2) matrix by a vector for CRC combination.
// Inputs: `matrix` is a 32-row operator and `vector` is the CRC state.
// Outputs: Returns the transformed CRC state.
std::uint32_t gf2_matrix_times(const std::array<std::uint32_t, 32>& matrix, std::uint32_t vector) {
    std::uint32_t sum = 0;
    std::size_t index = 0;
    while (vector != 0U) {
        if ((vector & 1U) != 0U) {
            sum ^= matrix[index];
        }
        vector >>= 1U;
        ++index;
    }
    return sum;
}

// Purpose: Square a GF(2) matrix operator for CRC combination.
// Inputs: `matrix` is the operator to square.
// Outputs: Returns an operator for twice as many zero bits.
std::array<std::uint32_t, 32> gf2_matrix_square(const std::array<std::uint32_t, 32>& matrix) {
    std::array<std::uint32_t, 32> square{};
    for (std::size_t i = 0; i < square.size(); ++i) {
        square[i] = gf2_matrix_times(matrix, matrix[i]);
    }
    return square;
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

std::uint32_t crc32_combine(std::uint32_t first_crc, std::uint32_t second_crc, std::uint64_t second_len) {
    if (second_len == 0) {
        return first_crc;
    }

    std::array<std::uint32_t, 32> odd{};
    odd[0] = 0xEDB88320U;
    std::uint32_t row = 1U;
    for (std::size_t n = 1; n < odd.size(); ++n) {
        odd[n] = row;
        row <<= 1U;
    }

    auto even = gf2_matrix_square(odd);
    odd = gf2_matrix_square(even);

    auto crc = first_crc;
    do {
        even = gf2_matrix_square(odd);
        if ((second_len & 1U) != 0U) {
            crc = gf2_matrix_times(even, crc);
        }
        second_len >>= 1U;
        if (second_len == 0) {
            break;
        }
        odd = gf2_matrix_square(even);
        if ((second_len & 1U) != 0U) {
            crc = gf2_matrix_times(odd, crc);
        }
        second_len >>= 1U;
    } while (second_len != 0U);

    return crc ^ second_crc;
}

}  // namespace superzip
