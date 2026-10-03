#include "test_util.hpp"

#include "cli/memory_benchmark_source.hpp"
#include "core/checksum.hpp"
#include "core/decoded_chunk.hpp"
#include "core/result.hpp"
#include "gpu/gpu_codec.hpp"

#include <array>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {
constexpr std::array<std::string_view, 7> profiles{"Mixed",           "Compressible", "Incompressible",
                                                   "RepeatedRecord",  "SparseRecord", "LongSparseRecord",
                                                   "SegmentedRecords"};

// Purpose: Distinguish expected integrity rejection from unrelated exceptions.
// Inputs: Bytes, claimed geometry/profile and optional immutable exact-size source.
// Outputs: Returns true only for ArchiveError from the bytewise source validator.
bool validation_rejected(std::span<const std::byte> bytes, std::uint64_t offset, std::uint64_t total,
                         std::string_view profile, std::span<const std::byte> source = {}) {
    std::vector<std::byte> scratch;
    try {
        superzip::cli::validate_memory_benchmark_bytes(bytes, offset, total, profile, scratch, source);
    } catch (const superzip::ArchiveError&) {
        return true;
    }
    return false;
}

// Purpose: Inspect parallel integrity failures without accepting unrelated exceptions.
// Inputs: Borrowed bytes, geometry/profile, worker limit and optional stable source snapshot.
// Outputs: Returns the ArchiveError message, or an empty string when the complete comparison succeeds.
std::string parallel_validation_error(std::span<const std::byte> bytes, std::uint64_t offset, std::uint64_t total,
                                      std::string_view profile, std::uint32_t workers,
                                      std::span<const std::byte> source = {}) {
    try {
        superzip::cli::validate_memory_benchmark_bytes_parallel(bytes, offset, total, profile, workers, source);
    } catch (const superzip::ArchiveError& error) {
        return error.what();
    }
    return {};
}
}  // namespace

// Purpose: Check source consistency across profile boundaries, unaligned chunks and reference-buffer tails.
// Inputs: All seven profiles and a complete source exceeding the long-record motif length.
// Outputs: Requires full equality for sliced/regenerated sources and rejects corruption in the final tail byte.
TEST_CASE(memory_benchmark_source_offsets_and_tails) {
    constexpr std::size_t total = 2U * 1024U * 1024U + 123U;
    std::vector<std::byte> full(total), scratch;
    for (const auto profile : profiles) {
        superzip::cli::fill_memory_benchmark_chunk(full, 0, total, profile);
        for (const auto offset : {0U, 1023U, 16381U, 65533U, 1048573U}) {
            const auto view = std::span(full).subspan(offset, 65537U);
            superzip::cli::validate_memory_benchmark_bytes(view, offset, total, profile, scratch);
            REQUIRE_TRUE(scratch.capacity() <= superzip::cli::kMemoryBenchmarkReferenceBytes);
            std::vector<std::byte> slice(view.begin(), view.end());
            slice.back() ^= std::byte{1};
            REQUIRE_TRUE(validation_rejected(slice, offset, total, profile));
        }
        superzip::cli::validate_memory_benchmark_bytes(std::span(full).last(123U), total - 123U, total, profile,
                                                       scratch);
    }
    REQUIRE_TRUE(validation_rejected(std::span(full).first(2U), total - 1U, total, "Mixed"));
    REQUIRE_TRUE(validation_rejected({}, total + 1U, total, "Mixed"));
}

// Purpose: Prove source equality cannot be satisfied by a second CRC-only check.
// Inputs: A zero-filled source and unequal same-length bytes with the same IEEE CRC32.
// Outputs: Requires equal known CRCs and explicit rejection of the collision by bytewise validation.
TEST_CASE(memory_benchmark_source_rejects_crc_collision) {
    std::vector<std::byte> original(64U), collision(64U);
    collision[0] = std::byte{1};
    collision[60] = std::byte{0x8f};
    collision[61] = std::byte{0x8e};
    collision[62] = std::byte{0xd3};
    collision[63] = std::byte{0xca};
    REQUIRE_EQ(superzip::crc32(original), 0x758d6336U);
    REQUIRE_EQ(superzip::crc32(collision), superzip::crc32(original));
    std::vector<std::byte> scratch;
    superzip::cli::validate_memory_benchmark_bytes(original, 0, 640U, "Compressible", scratch);
    REQUIRE_TRUE(validation_rejected(collision, 0, 640U, "Compressible"));
    REQUIRE_TRUE(!parallel_validation_error(collision, 0, 640U, "Compressible", 64U).empty());
}

// Purpose: Exercise real parallel partitions, reference tails and deterministic first-error reporting.
// Inputs: All profiles with an unaligned three-task extent, serial/two/max worker limits and corrupt boundaries.
// Outputs: Requires byte equality, rejects each corrupted partition and preserves the earliest failing offset.
TEST_CASE(memory_benchmark_source_parallel_partitions) {
    constexpr std::size_t extent = 24U * 1024U * 1024U + 123U;
    constexpr std::uint64_t offset = 1048559U;
    constexpr auto total = offset + extent + 17U;
    const auto stride = extent / 3U;
    std::vector<std::byte> bytes(extent);
    for (const auto profile : profiles) {
        superzip::cli::fill_memory_benchmark_chunk(bytes, offset, total, profile);
        for (const auto workers : {1U, 2U, 64U}) {
            REQUIRE_TRUE(parallel_validation_error(bytes, offset, total, profile, workers).empty());
        }
        for (const auto corrupt_at : {std::size_t{0}, stride, 2U * stride, extent - 1U}) {
            bytes[corrupt_at] ^= std::byte{1};
            REQUIRE_EQ(parallel_validation_error(bytes, offset, total, profile, 64U),
                       "memory benchmark byte validation mismatch at virtual offset " +
                           std::to_string(offset + corrupt_at));
            bytes[corrupt_at] ^= std::byte{1};
        }
        bytes[stride] ^= std::byte{1};
        bytes.back() ^= std::byte{1};
        REQUIRE_EQ(parallel_validation_error(bytes, offset, total, profile, 64U),
                   "memory benchmark byte validation mismatch at virtual offset " + std::to_string(offset + stride));
    }
}

// Purpose: Reject invalid parallel budgets and overflowing ranges before spawning borrowed-buffer readers.
// Inputs: Empty/tiny sources, invalid worker counts, overflowing geometry and an unsupported nonempty profile.
// Outputs: Requires explicit errors; valid empty input and the one-byte tail remain accepted.
TEST_CASE(memory_benchmark_source_parallel_admission) {
    const std::array<std::byte, 1> zero{};
    REQUIRE_TRUE(parallel_validation_error({}, 0, 0, "Mixed", 64U).empty());
    REQUIRE_TRUE(parallel_validation_error(zero, 0, 10U, "Compressible", 1U).empty());
    REQUIRE_TRUE(!parallel_validation_error(zero, 0, 10U, "Mixed", 0U).empty());
    REQUIRE_TRUE(!parallel_validation_error(zero, 0, 10U, "Mixed", 65U).empty());
    REQUIRE_TRUE(!parallel_validation_error(zero, 0, 0, "Mixed", 1U).empty());
    REQUIRE_TRUE(!parallel_validation_error(zero, std::numeric_limits<std::uint64_t>::max(),
                                            std::numeric_limits<std::uint64_t>::max(), "Mixed", 1U)
                      .empty());
    REQUIRE_TRUE(!parallel_validation_error(zero, 0, 10U, "Unknown", 1U).empty());
}

// Purpose: Exercise source regeneration against actual production owned CPU decoding for every profile.
// Inputs: Small unaligned virtual windows encoded with the production CPU policy.
// Outputs: Requires full decoded equality, exact lengths and CPU backend identity without filesystem payloads.
TEST_CASE(memory_benchmark_source_owned_cpu_roundtrip) {
    const superzip::GpuCodecOptions options{
        .require_gpu = false, .force_cpu = true, .block_size = 256U * 1024U, .worker_count = 2};
    std::vector<std::byte> scratch;
    for (const auto profile : profiles) {
        std::vector<std::byte> input(256U * 1024U + 37U);
        superzip::cli::fill_memory_benchmark_chunk(input, 1048559U, 4U * 1024U * 1024U, profile);
        auto encoded = superzip::encode_owned_chunk(std::move(input), options);
        auto decoded = superzip::decode_owned_chunk(encoded.payload, encoded.blocks, 256U * 1024U + 37U, options);
        REQUIRE_TRUE(!decoded.gpu_used);
        REQUIRE_EQ(decoded.bytes().size(), 256U * 1024U + 37U);
        superzip::cli::validate_memory_benchmark_bytes(decoded.bytes(), 1048559U, 4U * 1024U * 1024U, profile, scratch);
    }
}

// Purpose: Exercise regenerated-byte comparison with actual required-HIP decoding for every profile.
// Inputs: Small virtual windows on a HIP-capable host; hosted CPU-only validation reports explicit absence.
// Outputs: Requires GPU encode/decode identity and full byte equality; unavailable HIP is visibly untested.
TEST_CASE(memory_benchmark_source_owned_hip_roundtrip) {
    if (!superzip::query_gpu_info().available) {
        std::cout << "HIP bytewise source roundtrip not exercised: HIP unavailable\n";
        return;
    }
    const superzip::GpuCodecOptions options{.require_gpu = true, .block_size = 256U * 1024U, .worker_count = 2};
    std::vector<std::byte> scratch;
    for (const auto profile : profiles) {
        std::vector<std::byte> input(256U * 1024U + 37U);
        superzip::cli::fill_memory_benchmark_chunk(input, 1048559U, 4U * 1024U * 1024U, profile);
        auto encoded = superzip::encode_owned_chunk(std::move(input), options);
        REQUIRE_TRUE(encoded.gpu_used);
        auto decoded = superzip::decode_owned_chunk(encoded.payload, encoded.blocks, 256U * 1024U + 37U, options);
        REQUIRE_TRUE(decoded.gpu_used);
        REQUIRE_EQ(decoded.bytes().size(), 256U * 1024U + 37U);
        superzip::cli::validate_memory_benchmark_bytes(decoded.bytes(), 1048559U, 4U * 1024U * 1024U, profile, scratch);
    }
}

// Purpose: Validate exact borrowed source ranges without cyclic expansion or regeneration.
// Inputs: An unaligned 17 MiB snapshot, sliced copies, invalid geometry and two separated corrupt bytes.
// Outputs: Requires exact fill/comparison, zero reference scratch and deterministic parallel corruption rejection.
TEST_CASE(memory_benchmark_preloaded_source_ranges_and_corruption) {
    std::vector<std::byte> source(17U * 1024U * 1024U + 37U), scratch;
    superzip::cli::fill_memory_benchmark_chunk(source, 0, source.size(), "Mixed");
    for (const auto offset : {0U, 65533U, 8388607U}) {
        std::vector<std::byte> copy(65537U);
        superzip::cli::fill_memory_benchmark_chunk(copy, offset, source.size(), "Corpus", source);
        REQUIRE_TRUE(std::equal(copy.begin(), copy.end(), source.begin() + offset));
        superzip::cli::validate_memory_benchmark_bytes(copy, offset, source.size(), "Corpus", scratch, source);
    }
    REQUIRE_TRUE(scratch.empty());
    REQUIRE_TRUE(parallel_validation_error(source, 0, source.size(), "Corpus", 4U, source).empty());
    REQUIRE_TRUE(validation_rejected(std::span(source).first(1), 0, source.size() - 1U, "Corpus", source));
    REQUIRE_TRUE(validation_rejected(std::span(source).first(1), source.size(), source.size(), "Corpus", source));
    REQUIRE_TRUE(!parallel_validation_error(source, 0, source.size() - 1U, "Corpus", 4U, source).empty());
    auto decoded = source;
    decoded[65536U] ^= std::byte{1};
    decoded.back() ^= std::byte{1};
    const auto error = parallel_validation_error(decoded, 0, source.size(), "Corpus", 4U, source);
    REQUIRE_TRUE(error.find("virtual offset 65536") != std::string::npos);
    std::vector<std::byte> too_long(2U);
    bool rejected = false;
    try {
        superzip::cli::fill_memory_benchmark_chunk(too_long, source.size() - 1U, source.size(), "Corpus", source);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}
