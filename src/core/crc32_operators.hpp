#pragma once

#include <cstddef>
#include <cstdint>

namespace superzip::crc_detail {

struct Matrix {
    std::uint32_t rows[32]{};
};

template <std::size_t Count> struct ByteOperators {
    Matrix powers[Count]{};
};

// Purpose: Apply a reflected IEEE CRC polynomial operator to one finalized CRC.
// Inputs: matrix contains 32 basis images; vector is the full unsigned CRC state.
// Outputs: Returns the GF(2) matrix product without allocating or changing inputs.
constexpr std::uint32_t matrix_times(const Matrix& matrix, std::uint32_t vector) noexcept {
    std::uint32_t result = 0U;
    std::size_t index = 0U;
    while (vector != 0U) {
        if ((vector & 1U) != 0U) {
            result ^= matrix.rows[index];
        }
        vector >>= 1U;
        ++index;
    }
    return result;
}

// Purpose: Compose a CRC shift operator with itself.
// Inputs: matrix is a reflected 32-bit polynomial operator.
// Outputs: Returns the operator for twice the zero-bit displacement.
constexpr Matrix matrix_square(const Matrix& matrix) noexcept {
    Matrix result{};
    for (std::size_t index = 0U; index < 32U; ++index) {
        result.rows[index] = matrix_times(matrix, matrix.rows[index]);
    }
    return result;
}

// Purpose: Generate immutable CRC zero-byte operators shared by CPU and HIP reductions.
// Inputs: Count selects 1-64 powers of two bytes; the IEEE polynomial fixes the values.
// Outputs: Returns bounded plain storage usable as host data or a device constant initializer.
template <std::size_t Count> constexpr ByteOperators<Count> make_byte_operators() noexcept {
    static_assert(Count > 0U && Count <= 64U);
    Matrix bit_operator{};
    bit_operator.rows[0] = 0xEDB88320U;
    for (std::size_t index = 1U; index < 32U; ++index) {
        bit_operator.rows[index] = std::uint32_t{1} << (index - 1U);
    }
    ByteOperators<Count> result{};
    result.powers[0] = matrix_square(matrix_square(matrix_square(bit_operator)));
    for (std::size_t index = 1U; index < Count; ++index) {
        result.powers[index] = matrix_square(result.powers[index - 1U]);
    }
    return result;
}

}  // namespace superzip::crc_detail
