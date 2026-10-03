#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace superzip::cli {

inline constexpr std::size_t kMemoryBenchmarkReferenceBytes = 64U * 1024U;
inline constexpr std::size_t kMemoryBenchmarkCorpusMaxBytes = 64U * 1024U * 1024U;

// Purpose: Fill generated or preloaded RAM workload bytes without filesystem access.
// Inputs: Destination, offset/total and profile; optional immutable preloaded source must match total exactly.
// Outputs: Copies exact source bytes or generates deterministic bytes; invalid source extents throw ArchiveError.
void fill_memory_benchmark_chunk(std::vector<std::byte>& buffer, std::uint64_t global_offset, std::uint64_t total_bytes,
                                 std::string_view profile, std::span<const std::byte> source = {});

// Purpose: Require bytewise equality with generated or preloaded source, independently of CRC values.
// Inputs: Decoded bytes, geometry/profile, reusable scratch and optional stable preloaded source matching total.
// Outputs: Throws ArchiveError at the first mismatch or invalid extent; compares every byte otherwise.
void validate_memory_benchmark_bytes(std::span<const std::byte> decoded, std::uint64_t global_offset,
                                     std::uint64_t total_bytes, std::string_view profile,
                                     std::vector<std::byte>& scratch, std::span<const std::byte> source = {});

// Purpose: Compare independent source ranges within an explicit CPU worker and scratch-memory budget.
// Inputs: Stable decoded/source bytes, geometry/profile and an admitted worker limit in [1, 64].
// Outputs: Checks every byte or throws; all readers finish before return, including exceptional exits.
void validate_memory_benchmark_bytes_parallel(std::span<const std::byte> decoded, std::uint64_t global_offset,
                                              std::uint64_t total_bytes, std::string_view profile,
                                              std::uint32_t workers, std::span<const std::byte> source = {});

}  // namespace superzip::cli
