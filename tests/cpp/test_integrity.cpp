#include "test_util.hpp"

#include "core/checksum.hpp"
#include "core/defender_scan.hpp"
#include "core/integrity.hpp"
#include "core/result.hpp"
#include "core/trusted_runtime.hpp"

#include "7zCrc.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

#include <array>
#include <barrier>
#include <cstddef>
#include <fstream>
#include <future>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Purpose: Provide an independent bitwise IEEE CRC oracle without production or SDK lookup tables.
// Inputs: Readable bytes and a finalized incremental seed; neither is retained or modified.
// Outputs: Returns the finalized polynomial-0xEDB88320 checksum, including unchanged empty seeds.
std::uint32_t bitwise_crc32(std::span<const std::byte> bytes, std::uint32_t seed) {
    auto state = seed ^ 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        state ^= static_cast<std::uint8_t>(byte);
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            state = (state >> 1U) ^ ((0U - (state & 1U)) & 0xEDB88320U);
        }
    }
    return state ^ 0xFFFFFFFFU;
}

}  // namespace

// Purpose: Verify shared checksums and direct SDK callers publish the same tables under concurrent first use.
// Inputs: Eight synchronized workers hashing the standard check string through shared, SDK, and Miniz paths.
// Outputs: Requires exact checksums in a fresh filtered process and idempotent SDK initialization.
TEST_CASE(crc32_parallel_first_use_matches_known_digest) {
    constexpr std::string_view input = "123456789";
    std::barrier ready(8);
    std::array<std::future<std::uint32_t>, 8> workers;
    for (std::size_t index = 0U; index < workers.size(); ++index) {
        workers[index] = std::async(std::launch::async, [&ready, input, index] {
            ready.arrive_and_wait();
            if (index % 3U == 1U) {
                superzip::initialize_crc32_backend();
                return static_cast<std::uint32_t>(CrcCalc(input.data(), input.size()));
            }
            if (index % 3U == 2U) {
                return static_cast<std::uint32_t>(
                    mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(input.data()), input.size()));
            }
            return superzip::crc32(std::as_bytes(std::span(input.data(), input.size())));
        });
    }
    for (auto& worker : workers) {
        REQUIRE_EQ(worker.get(), 0xCBF43926U);
    }
}

// Purpose: Verify IEEE CRC seeds, unaligned input, table-word boundaries, and partial tails independently.
// Inputs: Deterministic nonperiodic bytes at sixteen alignments, thirty-four lengths, and four finalized seeds.
// Outputs: Requires both shared and Miniz CRCs to equal the bitwise oracle and preserve non-null empty seeds.
TEST_CASE(crc32_seeded_alignment_and_tail_oracle) {
    constexpr std::array<std::size_t, 34> lengths{
        0U,  1U,  2U,  3U,  4U,  7U,  8U,  9U,  11U, 12U,  13U,  15U,  16U,   17U,   23U,   24U,   25U,
        31U, 32U, 33U, 47U, 48U, 49U, 63U, 64U, 65U, 255U, 256U, 257U, 4095U, 4096U, 4097U, 8191U, 8193U,
    };
    constexpr std::array<std::uint32_t, 4> seeds{0U, 1U, 0x12345678U, 0xFFFFFFFFU};
    std::array<std::byte, 8193U + 15U> input{};
    std::uint32_t state = 0x75BA3210U;
    for (auto& byte : input) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        byte = static_cast<std::byte>(state & 255U);
    }
    for (const auto seed : seeds) {
        REQUIRE_EQ(superzip::crc32({}, seed), seed);
        for (std::size_t alignment = 0U; alignment < 16U; ++alignment) {
            for (const auto length : lengths) {
                const auto bytes = std::span(input).subspan(alignment, length);
                const auto expected = bitwise_crc32(bytes, seed);
                REQUIRE_EQ(superzip::crc32(bytes, seed), expected);
                REQUIRE_EQ(mz_crc32(seed, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()),
                           static_cast<mz_ulong>(expected));
            }
        }
    }
}

// Purpose: Verify arbitrary nonzero CRC seeds retain their meaning across fragmented streaming calls.
// Inputs: A fixed UTF-8 byte sequence, four seeds, and every possible ordered split including empty halves.
// Outputs: Requires shared and Miniz fragmented results to match the independent contiguous oracle.
TEST_CASE(crc32_seeded_fragmentation_matches_independent_oracle) {
    constexpr std::string_view input = "CRC streaming seeds must remain finalized across calls.";
    const auto bytes = std::as_bytes(std::span(input.data(), input.size()));
    for (const auto seed : {0U, 1U, 0x12345678U, 0xFFFFFFFFU}) {
        const auto expected = bitwise_crc32(bytes, seed);
        for (std::size_t split = 0U; split <= bytes.size(); ++split) {
            const auto first = superzip::crc32(bytes.first(split), seed);
            REQUIRE_EQ(superzip::crc32(bytes.subspan(split), first), expected);
            const auto miniz_first = mz_crc32(seed, reinterpret_cast<const unsigned char*>(bytes.data()), split);
            REQUIRE_EQ(mz_crc32(miniz_first, reinterpret_cast<const unsigned char*>(bytes.data() + split),
                                bytes.size() - split),
                       static_cast<mz_ulong>(expected));
        }
    }
}

// Purpose: Verify the external hook's documented null initialization and platform-width seed semantics.
// Inputs: Null initialization requests, a non-null empty range, and the widest mz_ulong finalized seed.
// Outputs: Requires null to reset to zero and non-null calls to retain only the low 32 seed bits.
TEST_CASE(crc32_miniz_null_initialization_and_seed_width) {
    constexpr unsigned char byte = 0xA5U;
    constexpr auto seed = std::numeric_limits<mz_ulong>::max();
    REQUIRE_EQ(mz_crc32(seed, nullptr, 0U), static_cast<mz_ulong>(MZ_CRC32_INIT));
    REQUIRE_EQ(mz_crc32(seed, nullptr, 1U), static_cast<mz_ulong>(MZ_CRC32_INIT));
    REQUIRE_EQ(mz_crc32(seed, &byte, 0U), static_cast<mz_ulong>(0xFFFFFFFFU));
    const auto expected = bitwise_crc32(std::as_bytes(std::span(&byte, 1U)), 0xFFFFFFFFU);
    REQUIRE_EQ(mz_crc32(seed, &byte, 1U), static_cast<mz_ulong>(expected));
}

// Purpose: Prove bounded parallel checksum composition matches an independent IEEE oracle and serial backend.
// Inputs: Unaligned irregular output, small/empty spans, zero/large worker requests, and real multi-task extents.
// Outputs: Requires exact CRC equality for every admitted split without changing serial or seeded CRC semantics.
TEST_CASE(crc32_parallel_output_matches_oracle_and_worker_bounds) {
    std::vector<std::byte> storage(32U * 1024U * 1024U + 37U);
    for (std::size_t index = 0; index < storage.size(); ++index) {
        storage[index] = static_cast<std::byte>((index * 31U + index / 997U) & 255U);
    }
    const auto bytes = std::span(storage).subspan(3U);
    const auto expected = bitwise_crc32(bytes, 0U);
    for (const auto workers : {0U, 1U, 2U, 3U, 8U, 64U, std::numeric_limits<std::uint32_t>::max()}) {
        REQUIRE_EQ(superzip::crc32_parallel(bytes, workers), expected);
    }
    for (const auto size : {0U, 1U, 1024U, 8U * 1024U * 1024U + 1U}) {
        REQUIRE_EQ(superzip::crc32_parallel(bytes.first(size), 64U), superzip::crc32(bytes.first(size)));
    }
}

// Purpose: Verify disabled integrity mode performs no hashing work.
// Inputs: A temporary sample file and `IntegrityMode::Disabled`.
// Outputs: Throws if hashing is attempted or a digest is emitted.
TEST_CASE(integrity_disabled_is_noop) {
    const auto root = test_temp_dir("integrity-disabled");
    const auto path = root / "sample.bin";
    {
        std::ofstream out(path, std::ios::binary);
        out << "abc";
    }

    const auto result = superzip::hash_file(path, superzip::IntegrityMode::Disabled);
    REQUIRE_TRUE(!result.attempted);
    REQUIRE_TRUE(result.hex_digest.empty());
}

// Purpose: Verify Windows CNG SHA-256 produces a known digest.
// Inputs: A temporary file containing `abc`.
// Outputs: Throws if the digest differs from the standard SHA-256 test vector.
TEST_CASE(integrity_sha256_matches_known_digest) {
    const auto root = test_temp_dir("integrity-sha256");
    const auto path = root / "sample.bin";
    {
        std::ofstream out(path, std::ios::binary);
        out << "abc";
    }

    const auto result = superzip::hash_file(path, superzip::IntegrityMode::Sha256);
    REQUIRE_TRUE(result.attempted);
    REQUIRE_EQ(result.algorithm, std::string("SHA-256"));
    REQUIRE_EQ(result.target, std::string("file"));
    REQUIRE_EQ(result.bytes_hashed, 3ULL);
    REQUIRE_EQ(result.files_hashed, 1ULL);
    REQUIRE_EQ(result.directories_hashed, 0ULL);
    constexpr std::array<std::string_view, 8> expected_parts{
        "ba7816bf", "8f01cfea", "414140de", "5dae2223", "b00361a3", "96177a9c", "b410ff61", "f20015ad",
    };
    std::string expected_digest;
    for (const auto part : expected_parts) {
        expected_digest += part;
    }
    REQUIRE_EQ(result.hex_digest, expected_digest);
}

// Purpose: Verify path hashing supports deterministic directory-tree digests.
// Inputs: A temporary directory with nested regular files.
// Outputs: Throws if ordering, counters, byte totals, or content sensitivity are wrong.
TEST_CASE(integrity_hash_path_directory_is_deterministic_and_content_sensitive) {
    const auto root = test_temp_dir("integrity-directory");
    const auto tree = root / "tree";
    const auto nested = tree / "nested";
    std::filesystem::create_directories(nested);
    {
        std::ofstream out(tree / "b.txt", std::ios::binary);
        out << "bravo";
    }
    {
        std::ofstream out(nested / "a.txt", std::ios::binary);
        out << "alpha";
    }

    const auto first = superzip::hash_path(tree, superzip::IntegrityMode::Sha256);
    const auto second = superzip::hash_path(tree, superzip::IntegrityMode::Sha256);
    REQUIRE_TRUE(first.attempted);
    REQUIRE_EQ(first.algorithm, std::string("SHA-256"));
    REQUIRE_EQ(first.target, std::string("directory"));
    REQUIRE_EQ(first.bytes_hashed, 10ULL);
    REQUIRE_EQ(first.files_hashed, 2ULL);
    REQUIRE_EQ(first.directories_hashed, 2ULL);
    REQUIRE_EQ(first.hex_digest, second.hex_digest);

    {
        std::ofstream out(nested / "a.txt", std::ios::binary | std::ios::trunc);
        out << "alpha!";
    }
    const auto changed = superzip::hash_path(tree, superzip::IntegrityMode::Sha256);
    REQUIRE_TRUE(first.hex_digest != changed.hex_digest);
    REQUIRE_EQ(changed.bytes_hashed, 11ULL);
}

// Purpose: Verify enabled path hashing rejects absent targets.
// Inputs: A deliberately missing temporary path and `IntegrityMode::Sha256`.
// Outputs: Throws if the missing target is not rejected as an archive error.
TEST_CASE(integrity_hash_path_rejects_missing_target) {
    const auto root = test_temp_dir("integrity-missing-target");
    const auto missing = root / "missing";

    bool rejected = false;
    try {
        (void)superzip::hash_path(missing, superzip::IntegrityMode::Sha256);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify parallel chunk CRCs can be combined into the same value as a single pass.
// Inputs: A deterministic byte vector and several chunk split points.
// Outputs: Throws if `crc32_combine` differs from contiguous CRC-32.
TEST_CASE(crc32_combine_matches_single_pass_crc) {
    std::vector<std::byte> bytes;
    bytes.reserve(257 * 1024);
    for (std::size_t i = 0; i < 257 * 1024; ++i) {
        bytes.push_back(static_cast<std::byte>((i * 131U + 17U) & 0xFFU));
    }

    const auto expected = superzip::crc32(std::span<const std::byte>(bytes.data(), bytes.size()));
    const std::array<std::size_t, 6> splits{
        0U, 1U, 4096U, 65536U, bytes.size() - 1U, bytes.size(),
    };
    for (const std::size_t split : splits) {
        const auto first = superzip::crc32(std::span<const std::byte>(bytes.data(), split));
        const auto second = superzip::crc32(std::span<const std::byte>(bytes.data() + split, bytes.size() - split));
        const auto combined = superzip::crc32_combine(first, second, bytes.size() - split);
        REQUIRE_EQ(combined, expected);
    }
}

// Purpose: Verify every 64-bit length operator against independently built upstream zlib, including unsigned limits.
// Inputs: Finalized CRCs 0x12345678/0x9ABCDEF0 and zlib 1.3.2 golden results for 2^0 through 2^63 bytes.
// Outputs: Requires exact combination values under concurrent calls, mixed length bits, and preserved empty semantics.
TEST_CASE(crc32_combine_full_length_oracle) {
    // zlib v1.3.2, commit da607da739fa6047df13e66a2af6b8bec7c2a498, crc32_combine64.
    // Unsigned lengths beyond INT64_MAX use consecutive signed-range zero-state advances in the oracle.
    constexpr std::array<std::uint32_t, 64> expected{
        0xC47013A8U, 0xFF52CBFBU, 0x1495863EU, 0x20C70901U, 0xFC8B1A58U, 0xCC60D264U, 0xA147EF04U, 0xE68FBBADU,
        0x37290B0EU, 0x733B82FEU, 0x10F6610DU, 0x52DBA75AU, 0x43C8229EU, 0xF3A01BCBU, 0xA517912AU, 0xC88F5BC1U,
        0x82461466U, 0x4D44EE89U, 0xF82AFF67U, 0x911BB6A5U, 0x88971340U, 0xACE65EBAU, 0x88EF8BC5U, 0xD846B98AU,
        0xAEB797EFU, 0x90382D8BU, 0xAD669877U, 0x32F3B171U, 0xABDD0E0FU, 0x93A6F5CCU, 0x9E31CB6EU, 0x762718B7U,
        0xC47013A8U, 0xFF52CBFBU, 0x1495863EU, 0x20C70901U, 0xFC8B1A58U, 0xCC60D264U, 0xA147EF04U, 0xE68FBBADU,
        0x37290B0EU, 0x733B82FEU, 0x10F6610DU, 0x52DBA75AU, 0x43C8229EU, 0xF3A01BCBU, 0xA517912AU, 0xC88F5BC1U,
        0x82461466U, 0x4D44EE89U, 0xF82AFF67U, 0x911BB6A5U, 0x88971340U, 0xACE65EBAU, 0x88EF8BC5U, 0xD846B98AU,
        0xAEB797EFU, 0x90382D8BU, 0xAD669877U, 0x32F3B171U, 0xABDD0E0FU, 0x93A6F5CCU, 0x9E31CB6EU, 0x762718B7U,
    };
    std::array<std::future<void>, 4> workers;
    for (auto& worker : workers) {
        worker = std::async(std::launch::async, [&expected] {
            for (std::size_t bit = 0U; bit < expected.size(); ++bit) {
                REQUIRE_EQ(superzip::crc32_combine(0x12345678U, 0x9ABCDEF0U, std::uint64_t{1} << bit), expected[bit]);
            }
        });
    }
    for (auto& worker : workers) {
        worker.get();
    }
    REQUIRE_EQ(superzip::crc32_combine(0x12345678U, 0x9ABCDEF0U, 0x123456789ABCDEFULL), 0x8BB98EB9U);
    REQUIRE_EQ(superzip::crc32_combine(0x12345678U, 0x9ABCDEF0U, std::numeric_limits<std::uint64_t>::max()),
               0x88888888U);
    REQUIRE_EQ(superzip::crc32_combine(0x12345678U, 0x9ABCDEF0U, 0U), 0x12345678U);
    REQUIRE_EQ(superzip::crc32_combine(0U, 0x9ABCDEF0U, std::numeric_limits<std::uint64_t>::max()), 0x9ABCDEF0U);
}

// Purpose: Verify repeated cached CRC combinations against a direct checksum on uneven and short source segments.
// Inputs: Deterministic bytes partitioned at small, CRC-kernel, and partial-tail boundaries.
// Outputs: Requires the same finalized CRC for each chunking scheme and incremental bytewise hashing.
TEST_CASE(crc32_combine_repeated_segment_boundaries) {
    std::vector<std::byte> bytes(128U * 1024U + 123U);
    std::uint32_t seed = 0x76543210U;
    for (auto& byte : bytes) {
        seed ^= seed << 13U;
        seed ^= seed >> 17U;
        seed ^= seed << 5U;
        byte = static_cast<std::byte>(seed & 255U);
    }
    const auto expected = superzip::crc32(bytes);
    for (const std::size_t segment_bytes : {1U, 13U, 4096U, 8192U, 32768U, 65536U}) {
        std::uint32_t combined = 0U;
        std::uint32_t incremental = 0U;
        for (std::size_t offset = 0U; offset < bytes.size(); offset += segment_bytes) {
            const auto segment = std::span(bytes).subspan(offset, std::min(segment_bytes, bytes.size() - offset));
            combined = superzip::crc32_combine(combined, superzip::crc32(segment), segment.size());
            incremental = superzip::crc32(segment, incremental);
        }
        REQUIRE_EQ(combined, expected);
        REQUIRE_EQ(combined, incremental);
    }
}

// Purpose: Verify disabled Defender mode is a no-op.
// Inputs: A temporary sample file and `DefenderScanMode::Disabled`.
// Outputs: Throws if a Defender scan is reported as attempted.
TEST_CASE(defender_disabled_is_noop) {
    const auto root = test_temp_dir("defender-disabled");
    const auto path = root / "sample.bin";
    {
        std::ofstream out(path, std::ios::binary);
        out << "abc";
    }

    const auto result = superzip::scan_with_windows_defender(path, superzip::DefenderScanMode::Disabled);
    REQUIRE_TRUE(!result.attempted);
    REQUIRE_TRUE(!result.clean);
    REQUIRE_TRUE(!result.timed_out);
}

// Purpose: Verify enabled Defender scans validate the selected target before scanner discovery.
// Inputs: A deliberately missing temporary path and `DefenderScanMode::FullPath`.
// Outputs: Throws if the missing target is not rejected as an archive error.
TEST_CASE(defender_enabled_rejects_missing_target) {
    const auto root = test_temp_dir("defender-missing-target");
    const auto missing = root / "missing.bin";

    bool rejected = false;
    try {
        (void)superzip::scan_with_windows_defender(missing, superzip::DefenderScanMode::FullPath);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify opt-in Defender policy accepts only a positive clean zero-exit result.
// Inputs: Synthetic clean, unavailable, timeout, and detected scan states.
// Outputs: Throws if any ambiguous or failed state passes the fail-closed predicate.
TEST_CASE(defender_policy_is_fail_closed) {
    REQUIRE_TRUE(superzip::defender_scan_passed(
        superzip::DefenderScanResult{.attempted = true, .clean = true, .timed_out = false, .exit_code = 0}));
    REQUIRE_TRUE(!superzip::defender_scan_passed(
        superzip::DefenderScanResult{.attempted = false, .clean = false, .timed_out = false, .exit_code = -1}));
    REQUIRE_TRUE(!superzip::defender_scan_passed(
        superzip::DefenderScanResult{.attempted = true, .clean = false, .timed_out = true, .exit_code = -2}));
    REQUIRE_TRUE(!superzip::defender_scan_passed(
        superzip::DefenderScanResult{.attempted = true, .clean = false, .timed_out = false, .exit_code = 2}));
    REQUIRE_TRUE(!superzip::defender_scan_passed(
        superzip::DefenderScanResult{.attempted = true, .clean = true, .timed_out = false, .exit_code = 1}));
}

#if defined(_WIN32)
// Purpose: Verify runtime loading rejects bytes that do not match build-pinned provenance.
// Inputs: The real app-local Zstandard DLL and a deliberately incorrect SHA-256 digest.
// Outputs: Throws if the loader reaches export lookup instead of failing closed at integrity validation.
TEST_CASE(trusted_runtime_rejects_unpinned_digest_before_export_lookup) {
    bool rejected = false;
    try {
        (void)superzip::load_trusted_app_local_runtime(L"libzstd.dll", std::string(64U, '0'));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify app-local runtime selection cannot escape the executable directory.
// Inputs: A path-bearing DLL name and a syntactically valid dummy SHA-256 digest.
// Outputs: Throws if parent traversal is accepted as a runtime filename.
TEST_CASE(trusted_runtime_rejects_path_bearing_module_names) {
    bool rejected = false;
    try {
        (void)superzip::load_trusted_app_local_runtime(L"..\\libzstd.dll", std::string(64U, '0'));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}
#endif
