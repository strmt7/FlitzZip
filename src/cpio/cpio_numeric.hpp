#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace flitzzip::detail {

// Purpose: Encode one SVR4 new ASCII header field without locale or varargs conversion.
// Inputs: value is the complete unsigned 32-bit field, including zero and UINT32_MAX.
// Outputs: Returns exactly eight uppercase ASCII hex digits, including leading zeroes.
inline std::array<char, 8> encode_cpio_hex_field(std::uint32_t value) {
    constexpr std::array<char, 16> alphabet{'0', '1', '2', '3', '4', '5', '6', '7',
                                            '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    std::array<char, 8> field{};
    for (std::size_t offset = 0; offset < field.size(); ++offset) {
        field[field.size() - 1U - offset] = alphabet[value & 0x0fU];
        value >>= 4U;
    }
    return field;
}

}  // namespace flitzzip::detail
