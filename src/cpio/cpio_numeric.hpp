#pragma once

#include "core/result.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>

namespace superzip::detail {

// Purpose: Encode one SVR4 new ASCII header field without locale or varargs conversion.
// Inputs: value is the complete unsigned 32-bit field, including zero and UINT32_MAX.
// Outputs: Returns exactly eight zero-padded uppercase ASCII hex digits; throws on conversion failure.
inline std::array<char, 8> encode_cpio_hex_field(std::uint32_t value) {
    std::array<char, 8> digits{};
    const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), value, 16);
    if (result.ec != std::errc{}) {
        throw ArchiveError("failed to encode CPIO header field");
    }
    const auto count = static_cast<std::size_t>(result.ptr - digits.data());
    std::array<char, 8> field{};
    field.fill('0');
    for (std::size_t i = 0; i < count; ++i) {
        const auto digit = digits[i];
        field[field.size() - count + i] = digit >= 'a' && digit <= 'f' ? static_cast<char>('A' + digit - 'a') : digit;
    }
    return field;
}

}  // namespace superzip::detail
