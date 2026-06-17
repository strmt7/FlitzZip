#include "test_util.hpp"

#include "core/checksum.hpp"
#include "core/defender_scan.hpp"
#include "core/integrity.hpp"
#include "core/result.hpp"

#include <array>
#include <cstddef>
#include <fstream>
#include <span>
#include <string_view>
#include <vector>

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
    constexpr std::array<std::string_view, 8> expected_parts{
        "ba7816bf", "8f01cfea", "414140de", "5dae2223", "b00361a3", "96177a9c", "b410ff61", "f20015ad",
    };
    std::string expected_digest;
    for (const auto part : expected_parts) {
        expected_digest += part;
    }
    REQUIRE_EQ(result.hex_digest, expected_digest);
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
