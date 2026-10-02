#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace superzip::cli {

inline constexpr std::size_t kMemoryBenchmarkReferenceBytes = 64U * 1024U;

// Purpose: Generate deterministic RAM workload bytes without filesystem access.
// Inputs: Destination, virtual offset, complete source size, and one supported data profile.
// Outputs: Fills the destination; profile shapes and seed remain identical across chunk/window boundaries.
void fill_memory_benchmark_chunk(std::vector<std::byte>& buffer, std::uint64_t global_offset, std::uint64_t total_bytes,
                                 std::string_view profile);

// Purpose: Require bytewise equality with regenerated benchmark source, independently of CRC values.
// Inputs: Decoded bytes, virtual source geometry/profile, and reusable bounded scratch storage.
// Outputs: Throws ArchiveError at the first mismatch or invalid extent; compares every byte otherwise.
void validate_memory_benchmark_bytes(std::span<const std::byte> decoded, std::uint64_t global_offset,
                                     std::uint64_t total_bytes, std::string_view profile,
                                     std::vector<std::byte>& scratch);

// Purpose: Compare independent source ranges within an explicit CPU worker and scratch-memory budget.
// Inputs: Stable borrowed decoded bytes, virtual geometry/profile, and an admitted worker limit in [1, 64].
// Outputs: Checks every byte or throws; all readers finish before return, including exceptional exits.
void validate_memory_benchmark_bytes_parallel(std::span<const std::byte> decoded, std::uint64_t global_offset,
                                              std::uint64_t total_bytes, std::string_view profile,
                                              std::uint32_t workers);

}  // namespace superzip::cli
