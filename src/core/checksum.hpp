#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace superzip {

// Purpose: Compute a CRC-32 checksum compatible with ZIP block integrity checks.
// Inputs: `bytes` is the memory region to hash and `seed` is the prior CRC state for incremental hashing.
// Outputs: Returns the finalized CRC-32 value; does not allocate or throw.
std::uint32_t crc32(std::span<const std::byte> bytes, std::uint32_t seed = 0);

// Purpose: Combine two finalized CRC-32 values without rereading the first byte range.
// Inputs: `first_crc` is the CRC of the first range, `second_crc` is the CRC of the following range, and `second_len` is the byte length of the second range.
// Outputs: Returns the finalized CRC for the concatenated byte ranges; does not allocate or throw.
std::uint32_t crc32_combine(std::uint32_t first_crc, std::uint32_t second_crc, std::uint64_t second_len);

}  // namespace superzip
