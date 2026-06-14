#include "test_util.hpp"

#include "core/defender_scan.hpp"
#include "core/integrity.hpp"

#include <array>
#include <fstream>
#include <string_view>

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
        "ba7816bf",
        "8f01cfea",
        "414140de",
        "5dae2223",
        "b00361a3",
        "96177a9c",
        "b410ff61",
        "f20015ad",
    };
    std::string expected_digest;
    for (const auto part : expected_parts) {
        expected_digest += part;
    }
    REQUIRE_EQ(result.hex_digest, expected_digest);
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
}
