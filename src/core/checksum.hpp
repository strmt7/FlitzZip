#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace superzip {

// Purpose: Initialize the process-wide vendored IEEE CRC backend before direct 7-Zip SDK CRC use.
// Inputs: None; safe and idempotent across concurrent checksum and SDK-reader callers.
// Outputs: Publishes immutable tables and supported-host capability selection without allocation or exceptions.
void initialize_crc32_backend() noexcept;

// Purpose: Compute a CRC-32 checksum compatible with ZIP block integrity checks.
// Inputs: `bytes` is the memory region to hash and `seed` is a finalized prior CRC for incremental hashing.
// Outputs: Returns the finalized CRC-32 value; does not allocate or throw.
std::uint32_t crc32(std::span<const std::byte> bytes, std::uint32_t seed = 0) noexcept;

// Purpose: Compute the same IEEE CRC using a bounded worker budget for sufficiently large borrowed output.
// Inputs: bytes stays valid until return; workers is the per-window CPU budget, capped at the archive worker limit.
// Outputs: Returns the serial CRC result; joins every task before return/unwind and propagates allocation/task
// failures.
std::uint32_t crc32_parallel(std::span<const std::byte> bytes, std::uint32_t workers);

// Purpose: Combine two finalized CRC-32 values without rereading the first byte range.
// Inputs: Ordered finalized CRCs and `second_len`, the following range's full 64-bit byte length.
// Outputs: Returns the concatenated CRC without allocation or exceptions; zero length preserves `first_crc`.
std::uint32_t crc32_combine(std::uint32_t first_crc, std::uint32_t second_crc, std::uint64_t second_len);

}  // namespace superzip
