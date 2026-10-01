#include "core/checksum.hpp"
#include "core/resource_limits.hpp"

#include <algorithm>
#include <future>
#include <vector>

#include <array>
#include <limits>

#include "7zCrc.h"

namespace superzip {
namespace {

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

// Purpose: Reuse CRC zero-byte operators for every bit of the supported 64-bit concatenated length.
// Inputs: None; the fixed ZIP polynomial determines all operators and initialization is thread-safe.
// Outputs: Returns an immutable 8 KiB table; no per-call allocation or mutable length cache is used.
const auto& crc_byte_operators() {
    static const auto operators = [] {
        std::array<std::array<std::uint32_t, 32>, std::numeric_limits<std::uint64_t>::digits> powers{};
        std::array<std::uint32_t, 32> bit_operator{};
        bit_operator[0] = 0xEDB88320U;
        std::uint32_t row = 1U;
        for (std::size_t index = 1U; index < bit_operator.size(); ++index) {
            bit_operator[index] = row;
            row <<= 1U;
        }
        powers[0] = gf2_matrix_square(gf2_matrix_square(gf2_matrix_square(bit_operator)));
        for (std::size_t index = 1U; index < powers.size(); ++index) {
            powers[index] = gf2_matrix_square(powers[index - 1U]);
        }
        return powers;
    }();
    return operators;
}

}  // namespace

// Purpose: Initialize the vendored IEEE CRC backend exactly once for checksums and 7-Zip SDK readers.
// Inputs: None; concurrent callers wait for complete table generation before any SDK CRC operation.
// Outputs: Publishes immutable SDK tables and capability selection without allocating or throwing.
void initialize_crc32_backend() noexcept {
    static const bool initialized = [] {
        CrcGenerateTable();
        return true;
    }();
    (void)initialized;
}

// Purpose: Compute or continue the ZIP-compatible CRC-32 of a borrowed byte range.
// Inputs: bytes remains readable for this call; seed is a finalized prior CRC, or zero for a new range.
// Outputs: Returns a finalized CRC without retaining input, allocating per call, or changing bytes.
std::uint32_t crc32(std::span<const std::byte> bytes, std::uint32_t seed) {
    if (bytes.empty()) {
        return seed;
    }
    initialize_crc32_backend();
    return CrcUpdate(seed ^ CRC_INIT_VAL, bytes.data(), bytes.size()) ^ CRC_INIT_VAL;
}

// Purpose: Combine finalized ZIP CRCs using precomputed zero-byte operators rather than rebuilding matrices.
// Inputs: Ordered CRC values and the full 64-bit byte length of the second range; zero length preserves first_crc.
// Outputs: Returns the concatenated CRC without allocation, throwing, or changing either caller value.
std::uint32_t crc32_combine(std::uint32_t first_crc, std::uint32_t second_crc, std::uint64_t second_len) {
    if (second_len == 0) {
        return first_crc;
    }
    if (first_crc == 0U) {
        return second_crc;
    }
    const auto& operators = crc_byte_operators();
    auto crc = first_crc;
    for (std::size_t index = 0U; second_len != 0U; ++index) {
        if ((second_len & 1U) != 0U) {
            crc = gf2_matrix_times(operators[index], crc);
        }
        second_len >>= 1U;
    }
    return crc ^ second_crc;
}

// Purpose: Hash independent large ranges and combine their finalized CRCs in byte order.
// Inputs: bytes is stable borrowed output; workers bounds CPU tasks, with at least 8 MiB per admitted task.
// Outputs: Returns the exact serial IEEE CRC; all asynchronous readers finish before borrowed storage can be released.
std::uint32_t crc32_parallel(std::span<const std::byte> bytes, std::uint32_t workers) {
    constexpr std::size_t minimum_task_bytes = 8U * 1024U * 1024U;
    const auto count = std::min<std::size_t>({workers, kMaxArchiveWorkers, bytes.size() / minimum_task_bytes});
    if (count <= 1U) {
        return crc32(bytes);
    }
    const auto stride = bytes.size() / count;
    std::vector<std::future<std::uint32_t>> pending;
    pending.reserve(count - 1U);
    for (std::size_t index = 1U; index < count; ++index) {
        const auto offset = stride * index;
        const auto extent = index + 1U == count ? bytes.size() - offset : stride;
        pending.push_back(
            std::async(std::launch::async, [part = bytes.subspan(offset, extent)] { return crc32(part); }));
    }
    auto checksum = crc32(bytes.first(stride));
    for (std::size_t index = 1U; index < count; ++index) {
        const auto extent = index + 1U == count ? bytes.size() - stride * index : stride;
        checksum = crc32_combine(checksum, pending[index - 1U].get(), extent);
    }
    return checksum;
}

}  // namespace superzip
