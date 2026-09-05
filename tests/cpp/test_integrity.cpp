#include "test_util.hpp"

#include "core/checksum.hpp"
#include "core/defender_scan.hpp"
#include "core/integrity.hpp"
#include "core/result.hpp"
#include "core/trusted_runtime.hpp"

#include <array>
#include <cstddef>
#include <fstream>
#include <span>
#include <string>
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
