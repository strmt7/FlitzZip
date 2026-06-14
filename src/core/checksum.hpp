#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace superzip {

// Purpose: Compute a CRC-32 checksum compatible with ZIP block integrity checks.
// Inputs: `bytes` is the memory region to hash and `seed` is the prior CRC state for incremental hashing.
// Outputs: Returns the finalized CRC-32 value; does not allocate or throw.
std::uint32_t crc32(std::span<const std::byte> bytes, std::uint32_t seed = 0);

}  // namespace superzip
