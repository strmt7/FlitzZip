#include "test_util.hpp"

#include "cli/benchmark_suite_policy.hpp"
#include "cli/memory_benchmark.hpp"
#include "cli/memory_benchmark_source.hpp"
#include "core/checksum.hpp"
#include "core/decoded_chunk.hpp"
#include "core/integrity.hpp"
#include "core/result.hpp"
#include "gpu/gpu_codec.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
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
        flitzzip::cli::validate_memory_benchmark_bytes(bytes, offset, total, profile, scratch, source);
    } catch (const flitzzip::ArchiveError&) {
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
        flitzzip::cli::validate_memory_benchmark_bytes_parallel(bytes, offset, total, profile, workers, source);
    } catch (const flitzzip::ArchiveError& error) {
        return error.what();
    }
    return {};
}

// Purpose: Invoke benchmark planning and retain its exact production diagnostic for admission-contract assertions.
// Inputs: One benchmark option set; no codec or source allocation is performed by this helper.
// Outputs: Returns an ArchiveError/GpuError diagnostic, or an empty string when planning succeeds.
std::string benchmark_plan_error(const flitzzip::cli::MemoryBenchmarkOptions& options) {
    try {
        (void)flitzzip::cli::plan_memory_benchmark(options);
    } catch (const flitzzip::Error& error) {
        return error.what();
    }
    return {};
}
}  // namespace

// Purpose: Reject malformed RAM-benchmark geometry before allocation or codec execution.
// Inputs: Generated/corpus boundary cases for size, identity, backend, workers, block size and queue depth.
// Outputs: Requires the specific production diagnostic for each independently invalid contract.
TEST_CASE(memory_benchmark_plan_rejects_invalid_geometry_before_execution) {
    using Options = flitzzip::cli::MemoryBenchmarkOptions;
    constexpr auto lowercase_sha = "0000000000000000000000000000000000000000000000000000000000000000";

    struct InvalidCase {
        Options options;
        std::string_view diagnostic;
    };
    std::vector<InvalidCase> cases;
    {
        Options options;
        options.size_mib = 1U;
        options.force_cpu = true;
        cases.push_back({options, "memory benchmark workload must be at least 10240 MiB (10 GiB)"});
    }
    {
        Options options;
        options.size_mib = 10240U;
        options.profile = "Unknown";
        options.force_cpu = true;
        cases.push_back({options, "unknown memory benchmark profile: Unknown"});
    }
    {
        Options options;
        options.size_mib = std::numeric_limits<std::uint64_t>::max();
        options.force_cpu = true;
        cases.push_back({options, "memory benchmark size overflows"});
    }
    {
        Options options;
        options.size_mib = 10240U;
        options.expected_source_sha256 = lowercase_sha;
        options.force_cpu = true;
        cases.push_back({options, "source SHA-256 requires a preloaded corpus"});
    }
    {
        Options options;
        options.corpus_bytes = 1U;
        options.profile = "Corpus";
        options.expected_source_sha256 = lowercase_sha;
        options.require_gpu = true;
        options.force_cpu = true;
        cases.push_back({options, "--require-gpu and --force-cpu are mutually exclusive"});
    }
    {
        Options options;
        options.corpus_bytes = 1U;
        options.profile = "Corpus";
        options.expected_source_sha256 = lowercase_sha;
        options.force_cpu = true;
        options.block_size = flitzzip::kMinArchiveBlockBytes - 1U;
        cases.push_back({options, "memory benchmark block size is outside FlitzZip resource limits"});
    }
    {
        Options options;
        options.corpus_bytes = 1U;
        options.profile = "Corpus";
        options.expected_source_sha256 = lowercase_sha;
        options.force_cpu = true;
        options.block_size = flitzzip::kMinArchiveBlockBytes + 1U;
        cases.push_back({options, "memory benchmark block size must divide the 128 MiB chunk size"});
    }
    {
        Options options;
        options.corpus_bytes = 1U;
        options.profile = "Corpus";
        options.expected_source_sha256 = lowercase_sha;
        options.force_cpu = true;
        options.inflight_chunks = flitzzip::kMaxInflightArchiveChunks + 1U;
        cases.push_back({options, "benchmark inflight depth exceeds FlitzZip resource limit"});
    }
    {
        Options options;
        options.corpus_bytes = 1U;
        options.profile = "Corpus";
        options.expected_source_sha256 = lowercase_sha;
        options.force_cpu = true;
        options.workers = flitzzip::kMaxArchiveWorkers + 1U;
        cases.push_back({options, "worker count exceeds FlitzZip resource limit"});
    }
    {
        Options options;
        options.corpus_bytes = 1U;
        options.profile = "Corpus";
        options.expected_source_sha256 = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
        options.force_cpu = true;
        cases.push_back({options, "corpus requires a 32-bit extent and a lowercase SHA-256 identity"});
    }

    for (const auto& invalid : cases) {
        REQUIRE_EQ(benchmark_plan_error(invalid.options), invalid.diagnostic);
    }
}

// Purpose: Exercise the complete RAM-only CPU benchmark pipeline against a resident, authenticated corpus.
// Inputs: A small unaligned source, bounded queues/workers and production encode/verify/extract implementations.
// Outputs: Requires byte-exact validation, CPU-only identity, native-container accounting and explicit no-disk output.
TEST_CASE(memory_benchmark_cpu_pipeline_is_byte_exact_and_ram_only) {
    std::vector<std::byte> source(1024U * 1024U + 37U);
    flitzzip::cli::fill_memory_benchmark_chunk(source, 0U, source.size(), "RepeatedRecord");
    const auto sha256 = flitzzip::sha256_bytes(source);

    flitzzip::cli::MemoryBenchmarkOptions options;
    options.source = source;
    options.corpus_bytes = source.size();
    options.expected_source_sha256 = sha256;
    options.profile = "Corpus";
    options.force_cpu = true;
    options.workers = 2U;
    options.inflight_chunks = 1U;
    options.decode_inflight_chunks = 1U;
    options.block_size = 256U * 1024U;
    options.compression_level = 3;

    const auto result = flitzzip::cli::run_memory_benchmark(options);
    REQUIRE_EQ(result.source_sha256, sha256);
    REQUIRE_EQ(result.stats.entries, 1U);
    REQUIRE_EQ(result.stats.input_bytes, source.size());
    REQUIRE_EQ(result.validated_bytes, source.size());
    REQUIRE_EQ(result.stats.workers, 2U);
    REQUIRE_EQ(result.stats.inflight_chunks, 1U);
    REQUIRE_EQ(result.decode_inflight_chunks, 1U);
    REQUIRE_TRUE(!result.stats.gpu_used);
    REQUIRE_TRUE(result.stats.output_bytes > 0U);
    REQUIRE_TRUE(result.archive_bytes > result.stats.output_bytes);
    REQUIRE_TRUE(result.wall_seconds >= result.validation_seconds);

    std::ostringstream output;
    auto* original = std::cout.rdbuf(output.rdbuf());
    flitzzip::cli::print_memory_benchmark_stats(result);
    std::cout.rdbuf(original);
    const auto text = output.str();
    REQUIRE_TRUE(text.find("source_sha256=" + sha256) != std::string::npos);
    REQUIRE_TRUE(text.find("gpu_used=false") != std::string::npos);
    REQUIRE_TRUE(text.find("validated_bytes=" + std::to_string(source.size())) != std::string::npos);
    REQUIRE_TRUE(text.find("memory_only=true disk_write_bytes=0") != std::string::npos);
}

// Purpose: Exercise Neutron through the same authenticated RAM-only benchmark boundary used by the CLI.
// Inputs: A bounded corpus and a genuinely admitted AMD HIP device; CPU-only hosts report the unexercised scope.
// Outputs: Requires required-HIP encode/decode use and exact independent byte validation without filesystem writes.
TEST_CASE(memory_benchmark_neutron_pipeline_requires_real_hip_and_exact_bytes) {
    if (!flitzzip::query_gpu_info().available) {
        std::cout << "Neutron RAM-only benchmark pipeline not exercised: HIP unavailable\n";
        return;
    }
    std::vector<std::byte> source(1024U * 1024U + 37U);
    flitzzip::cli::fill_memory_benchmark_chunk(source, 0U, source.size(), "SegmentedRecords");

    flitzzip::cli::MemoryBenchmarkOptions options;
    options.source = source;
    options.corpus_bytes = source.size();
    options.expected_source_sha256 = flitzzip::sha256_bytes(source);
    options.profile = "Corpus";
    options.require_gpu = true;
    options.workers = 2U;
    options.inflight_chunks = 1U;
    options.decode_inflight_chunks = 1U;
    options.block_size = 256U * 1024U;
    options.compression_level = flitzzip::kMaxCompressionLevel;
    options.compression_mode = flitzzip::NativeCompressionMode::NeutronStar;

    const auto result = flitzzip::cli::run_memory_benchmark(options);
    REQUIRE_TRUE(result.stats.gpu_used);
    REQUIRE_EQ(result.stats.input_bytes, source.size());
    REQUIRE_EQ(result.validated_bytes, source.size());
    REQUIRE_EQ(result.compression_mode, flitzzip::NativeCompressionMode::NeutronStar);
    REQUIRE_TRUE(result.stats.gpu_runtime.encode_chunks > 0U);
    REQUIRE_TRUE(result.stats.gpu_runtime.decode_chunks > 0U);
}

// Purpose: Check source admission at exact physical-memory boundaries without allocating or pressuring the host.
// Inputs: Independent even/odd capacity fixtures, at/above-target snapshots, invalid counters and a wrapping-size
// probe. Outputs: Admits exactly half the available growth and rejects unsafe residency before transport or allocation.
TEST_CASE(memory_benchmark_source_headroom_admission_is_allocation_free) {
    struct Fixture {
        flitzzip::HostMemorySnapshot memory;
        std::size_t size;
        bool admitted;
    };
    const std::array cases{
        Fixture{{200U, 200U}, 80U, true},
        Fixture{{200U, 200U}, 81U, false},
        Fixture{{103U, 103U}, 41U, true},
        Fixture{{103U, 103U}, 42U, false},
        Fixture{{1000U, 201U}, 1U, false},
        Fixture{{1000U, 202U}, 1U, true},
        Fixture{{1000U, 200U}, 1U, false},
        Fixture{{1000U, 199U}, 1U, false},
        Fixture{{0U, 0U}, 1U, false},
        Fixture{{1000U, 1001U}, 1U, false},
        Fixture{{200U, 200U}, std::numeric_limits<std::size_t>::max(), false},
    };
    for (const auto& fixture : cases) {
        bool admitted = true;
        try {
            flitzzip::cli::validate_memory_benchmark_source_headroom(fixture.size, fixture.memory);
        } catch (const flitzzip::ArchiveError&) {
            admitted = false;
        }
        REQUIRE_EQ(admitted, fixture.admitted);
    }
}

// Purpose: Keep a fixed nondefault block-size sweep anchored to balanced compression, not its first weak level.
// Inputs: Unit observations with fast level-one output twice as large as level five; no timing claim is made.
// Outputs: Requires the fastest size-eligible case even when level one precedes the balanced observation.
TEST_CASE(memory_benchmark_suite_nondefault_block_preserves_balanced_size) {
    constexpr std::uint32_t block = 256U * 1024U;
    const std::array<flitzzip::cli::BenchmarkSuiteObservation, 3> observations{
        {{1, block, 1000.0, 0.8}, {5, block, 500.0, 0.4}, {9, block, 200.0, 0.3}}};
    REQUIRE_EQ(flitzzip::cli::select_benchmark_suite_recommendation(observations, true), 1U);
}

// Purpose: Preserve the documented default geometry and include exactly the two-percent size boundary.
// Inputs: Unit observations on both sides of the guard, a stronger alternate baseline and equal-score candidates.
// Outputs: Requires the first fastest eligible case, default-baseline preference and a stricter alternate baseline.
TEST_CASE(memory_benchmark_suite_ratio_boundary_and_baseline_preference) {
    using Observation = flitzzip::cli::BenchmarkSuiteObservation;
    const auto block = flitzzip::kDefaultArchiveBlockBytes;
    const double limit = 0.5 * 1.02;
    std::array<Observation, 5> observations{{{1, block, 3000.0, std::nextafter(limit, 1.0)},
                                             {3, block, 2000.0, limit},
                                             {5, block, 1000.0, 0.5},
                                             {5, 256U * 1024U, 500.0, 0.4},
                                             {9, block, 2000.0, 0.3}}};
    REQUIRE_EQ(flitzzip::cli::select_benchmark_suite_recommendation(observations, true), 1U);
    REQUIRE_EQ(flitzzip::cli::select_benchmark_suite_recommendation(observations, false), 0U);
    std::swap(observations[2], observations[3]);
    REQUIRE_EQ(flitzzip::cli::select_benchmark_suite_recommendation(observations, true), 1U);
    observations[3].block_size = 512U * 1024U;
    REQUIRE_EQ(flitzzip::cli::select_benchmark_suite_recommendation(observations, true), 4U);
}

// Purpose: Keep recommendation independent of enumeration order when a unique fastest eligible case exists.
// Inputs: Every permutation of a fixed nondefault sweep, including weaker first candidates and slower smaller output.
// Outputs: Requires the balanced case by its data identity, not by an assumed candidate index.
TEST_CASE(memory_benchmark_suite_permutations_preserve_size_guard) {
    constexpr std::uint32_t block = 512U * 1024U;
    const std::array<flitzzip::cli::BenchmarkSuiteObservation, 4> original{
        {{1, block, 1000.0, 0.8}, {3, block, 700.0, 0.42}, {5, block, 500.0, 0.4}, {9, block, 200.0, 0.3}}};
    std::array<std::size_t, 4> order{0U, 1U, 2U, 3U};
    do {
        auto observations = original;
        for (std::size_t index = 0U; index < order.size(); ++index) {
            observations[index] = original[order[index]];
        }
        const auto selected = flitzzip::cli::select_benchmark_suite_recommendation(observations, true);
        REQUIRE_EQ(observations[selected].compression_level, 5);
    } while (std::next_permutation(order.begin(), order.end()));
}

// Purpose: Reject missing or malformed evidence rather than selecting a case from invalid comparisons.
// Inputs: Empty input, absent balanced level and each nonfinite/negative score or size ratio in either tuning mode.
// Outputs: Requires ArchiveError for every invalid observation, including values after the default baseline.
TEST_CASE(memory_benchmark_suite_rejects_invalid_observations) {
    using Observation = flitzzip::cli::BenchmarkSuiteObservation;
    for (const bool tune : {false, true}) {
        bool empty_rejected = false;
        try {
            (void)flitzzip::cli::select_benchmark_suite_recommendation({}, tune);
        } catch (const flitzzip::ArchiveError&) {
            empty_rejected = true;
        }
        REQUIRE_TRUE(empty_rejected);
        for (const double bad : {-1.0, std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            for (const bool bad_ratio : {false, true}) {
                const std::array<Observation, 2> observations{
                    {{5, flitzzip::kDefaultArchiveBlockBytes, 1.0, 0.5},
                     {9, flitzzip::kDefaultArchiveBlockBytes, bad_ratio ? 1.0 : bad, bad_ratio ? bad : 0.4}}};
                bool rejected = false;
                try {
                    (void)flitzzip::cli::select_benchmark_suite_recommendation(observations, tune);
                } catch (const flitzzip::ArchiveError&) {
                    rejected = true;
                }
                REQUIRE_TRUE(rejected);
            }
        }
    }
    const std::array<Observation, 1> missing{{{1, flitzzip::kDefaultArchiveBlockBytes, 1.0, 0.5}}};
    bool rejected = false;
    try {
        (void)flitzzip::cli::select_benchmark_suite_recommendation(missing, true);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(flitzzip::cli::select_benchmark_suite_recommendation(missing, false), 0U);
}

// Purpose: Admit legitimate zero-byte compressed payloads and zero rounded scores without weakening the size guard.
// Inputs: Zero-ratio balanced output, a positive-ratio fast case and a tied zero-score case.
// Outputs: Requires the first zero-ratio case with tuning and the fastest case without tuning.
TEST_CASE(memory_benchmark_suite_zero_metrics_remain_valid) {
    const std::array<flitzzip::cli::BenchmarkSuiteObservation, 3> observations{
        {{1, flitzzip::kDefaultArchiveBlockBytes, 100.0, 0.1},
         {5, flitzzip::kDefaultArchiveBlockBytes, 0.0, 0.0},
         {9, flitzzip::kDefaultArchiveBlockBytes, 0.0, 0.0}}};
    REQUIRE_EQ(flitzzip::cli::select_benchmark_suite_recommendation(observations, true), 1U);
    REQUIRE_EQ(flitzzip::cli::select_benchmark_suite_recommendation(observations, false), 0U);
}

// Purpose: Preserve all binary stream bytes instead of applying text or EOF-marker translation.
// Inputs: Every byte value, including NUL, CRLF and Ctrl-Z, through an exact nonempty source stream.
// Outputs: Requires exact extent and byte equality before any codec is involved.
TEST_CASE(memory_benchmark_stream_preserves_binary_bytes) {
    std::string source;
    for (unsigned int value = 0U; value < 256U; ++value) {
        source.push_back(static_cast<char>(value));
    }
    std::istringstream input(source, std::ios::binary);
    const auto bytes = flitzzip::cli::load_memory_benchmark_stream(input, source.size());
    REQUIRE_EQ(bytes.size(), source.size());
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        REQUIRE_EQ(std::to_integer<unsigned int>(bytes[index]), static_cast<unsigned int>(index));
    }
}

// Purpose: Reject undeclared, missing or failed input without accepting an incomplete snapshot.
// Inputs: Zero/oversized declarations, truncated/trailing payloads and a stream with a read error.
// Outputs: Every invalid source raises ArchiveError; no oversized fixture allocation is needed.
TEST_CASE(memory_benchmark_stream_rejects_invalid_extents_and_reads) {
    for (const auto size :
         {std::size_t{0}, std::size_t{1}, std::size_t{3}, flitzzip::cli::kMemoryBenchmarkCorpusMaxBytes + 1U}) {
        std::istringstream input("ab", std::ios::binary);
        bool rejected = false;
        try {
            (void)flitzzip::cli::load_memory_benchmark_stream(input, size);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    std::istringstream failed("ab", std::ios::binary);
    failed.setstate(std::ios::badbit);
    bool rejected = false;
    try {
        (void)flitzzip::cli::load_memory_benchmark_stream(failed, 2U);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Check source consistency across profile boundaries, unaligned chunks and reference-buffer tails.
// Inputs: All seven profiles and a complete source exceeding the long-record motif length.
// Outputs: Requires full equality for sliced/regenerated sources and rejects corruption in the final tail byte.
TEST_CASE(memory_benchmark_source_offsets_and_tails) {
    constexpr std::size_t total = 2U * 1024U * 1024U + 123U;
    std::vector<std::byte> full(total), scratch;
    for (const auto profile : profiles) {
        flitzzip::cli::fill_memory_benchmark_chunk(full, 0, total, profile);
        for (const auto offset : {0U, 1023U, 16381U, 65533U, 1048573U}) {
            const auto view = std::span(full).subspan(offset, 65537U);
            flitzzip::cli::validate_memory_benchmark_bytes(view, offset, total, profile, scratch);
            REQUIRE_TRUE(scratch.capacity() <= flitzzip::cli::kMemoryBenchmarkReferenceBytes);
            std::vector<std::byte> slice(view.begin(), view.end());
            slice.back() ^= std::byte{1};
            REQUIRE_TRUE(validation_rejected(slice, offset, total, profile));
        }
        flitzzip::cli::validate_memory_benchmark_bytes(std::span(full).last(123U), total - 123U, total, profile,
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
    REQUIRE_EQ(flitzzip::crc32(original), 0x758d6336U);
    REQUIRE_EQ(flitzzip::crc32(collision), flitzzip::crc32(original));
    std::vector<std::byte> scratch;
    flitzzip::cli::validate_memory_benchmark_bytes(original, 0, 640U, "Compressible", scratch);
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
        flitzzip::cli::fill_memory_benchmark_chunk(bytes, offset, total, profile);
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
    const flitzzip::GpuCodecOptions options{
        .require_gpu = false, .force_cpu = true, .block_size = 256U * 1024U, .worker_count = 2};
    std::vector<std::byte> scratch;
    for (const auto profile : profiles) {
        std::vector<std::byte> input(256U * 1024U + 37U);
        flitzzip::cli::fill_memory_benchmark_chunk(input, 1048559U, 4U * 1024U * 1024U, profile);
        auto encoded = flitzzip::encode_owned_chunk(std::move(input), options);
        auto decoded = flitzzip::decode_owned_chunk(encoded.payload, encoded.blocks, 256U * 1024U + 37U, options);
        REQUIRE_TRUE(!decoded.gpu_used);
        REQUIRE_EQ(decoded.bytes().size(), 256U * 1024U + 37U);
        flitzzip::cli::validate_memory_benchmark_bytes(decoded.bytes(), 1048559U, 4U * 1024U * 1024U, profile, scratch);
    }
}

// Purpose: Exercise regenerated-byte comparison with actual required-HIP decoding for every profile.
// Inputs: Small virtual windows on a HIP-capable host; hosted CPU-only validation reports explicit absence.
// Outputs: Requires GPU encode/decode identity and full byte equality; unavailable HIP is visibly untested.
TEST_CASE(memory_benchmark_source_owned_hip_roundtrip) {
    if (!flitzzip::query_gpu_info().available) {
        std::cout << "HIP bytewise source roundtrip not exercised: HIP unavailable\n";
        return;
    }
    const flitzzip::GpuCodecOptions options{.require_gpu = true, .block_size = 256U * 1024U, .worker_count = 2};
    std::vector<std::byte> scratch;
    for (const auto profile : profiles) {
        std::vector<std::byte> input(256U * 1024U + 37U);
        flitzzip::cli::fill_memory_benchmark_chunk(input, 1048559U, 4U * 1024U * 1024U, profile);
        auto encoded = flitzzip::encode_owned_chunk(std::move(input), options);
        REQUIRE_TRUE(encoded.gpu_used);
        auto decoded = flitzzip::decode_owned_chunk(encoded.payload, encoded.blocks, 256U * 1024U + 37U, options);
        REQUIRE_TRUE(decoded.gpu_used);
        REQUIRE_EQ(decoded.bytes().size(), 256U * 1024U + 37U);
        flitzzip::cli::validate_memory_benchmark_bytes(decoded.bytes(), 1048559U, 4U * 1024U * 1024U, profile, scratch);
    }
}

// Purpose: Validate exact borrowed source ranges without cyclic expansion or regeneration.
// Inputs: An unaligned 17 MiB snapshot, sliced copies, invalid geometry and two separated corrupt bytes.
// Outputs: Requires exact fill/comparison, zero reference scratch and deterministic parallel corruption rejection.
TEST_CASE(memory_benchmark_preloaded_source_ranges_and_corruption) {
    std::vector<std::byte> source(17U * 1024U * 1024U + 37U), scratch;
    flitzzip::cli::fill_memory_benchmark_chunk(source, 0, source.size(), "Mixed");
    for (const auto offset : {0U, 65533U, 8388607U}) {
        std::vector<std::byte> copy(65537U);
        flitzzip::cli::fill_memory_benchmark_chunk(copy, offset, source.size(), "Corpus", source);
        REQUIRE_TRUE(std::equal(copy.begin(), copy.end(), source.begin() + offset));
        flitzzip::cli::validate_memory_benchmark_bytes(copy, offset, source.size(), "Corpus", scratch, source);
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
        flitzzip::cli::fill_memory_benchmark_chunk(too_long, source.size() - 1U, source.size(), "Corpus", source);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}
