#include "test_util.hpp"
#include "test_path_diagnostic.hpp"

#include "base64/base64_adapter.hpp"
#include "core/defender_scan.hpp"
#include "core/result.hpp"
#include "uue/uue_adapter.hpp"
#include "xxe/xxe_adapter.hpp"

#include <fstream>
#include <iterator>
#include <string_view>

namespace {

// Purpose: Require a boundary's intended error class and complete native filename, independent of Windows aliases.
// Inputs: Action, exact diagnostic prefix and the caller-selected path; only equivalent native aliases are allowed.
// Outputs: Rejects unrelated exceptions, incomplete names and arbitrary diagnostic changes.
template <typename Error, typename Action>
void require_path_error(Action&& action, std::string_view prefix, const std::filesystem::path& path) {
    bool rejected = false;
    try {
        action();
    } catch (const Error& error) {
        REQUIRE_TRUE(flitzzip_test::path_diagnostic_matches(error.what(), prefix, path));
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Read a bounded byte fixture independently of the encoder or decoder.
// Inputs: An owned small regular file. Outputs: Exact binary contents, including zero and high-bit bytes.
std::string diagnostic_fixture_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE_TRUE(input.is_open());
    return std::string(std::istreambuf_iterator<char>(input), {});
}

// Purpose: Derive expected UTF-8 directly from the native fixture, independently of product diagnostic helpers.
// Inputs: Owned native fixture path. Outputs: Complete byte-length-preserving UTF-8 text.
std::string diagnostic_fixture_path_text(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

using TextWriter = flitzzip::OperationStats (*)(const std::filesystem::path&, const std::filesystem::path&,
                                                const flitzzip::ProgressCallback&);
using TextReader = flitzzip::OperationStats (*)(const std::filesystem::path&, const std::filesystem::path&, bool,
                                                const flitzzip::ProgressCallback&);

// Purpose: Exercise invalid sources, source overlap and overwrite refusal through one text codec's public API.
// Inputs: The codec's fixed writer/reader and diagnostic name; all bytes live in one tiny owned fixture tree.
// Outputs: Preserves original bytes and exact error classes, proves successful reuse and leaves no staging debris.
void require_text_codec_unicode_contract(const std::string& name, TextWriter write, TextReader read) {
    const auto root = test_temp_dir("path-diagnostic-" + name);
    const auto directory = root / std::filesystem::path(L"\u65e5\u672c-\U0001F4C1");
    std::filesystem::create_directory(directory);
    const auto source = directory / "payload.bin";
    const auto archive = root / "input.txt";
    const std::string payload("text\0payload\xff", 13);
    require_path_error<flitzzip::ArchiveError>([&] { (void)write(directory, archive, {}); },
                                               name + " compression requires one regular file: ", directory);
    REQUIRE_TRUE(!std::filesystem::exists(archive));
    {
        std::ofstream output(source, std::ios::binary);
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        REQUIRE_TRUE(output.good());
    }
    require_path_error<flitzzip::SecurityError>([&] { (void)write(source, source, {}); },
                                                "refusing to overwrite the " + name + " source file: ", source);
    REQUIRE_EQ(diagnostic_fixture_bytes(source), payload);
    const auto written = write(source, archive, {});
    REQUIRE_EQ(written.entries, 1U);
    REQUIRE_EQ(written.input_bytes, payload.size());
    require_path_error<flitzzip::SecurityError>(
        [&] { (void)read(archive, directory, false, {}); },
        "refusing to overwrite existing " + name + " extraction target: ", source);
    REQUIRE_EQ(diagnostic_fixture_bytes(source), payload);
    const auto restored = read(archive, directory, true, {});
    REQUIRE_EQ(restored.entries, 1U);
    REQUIRE_EQ(restored.output_bytes, payload.size());
    REQUIRE_EQ(diagnostic_fixture_bytes(source), payload);
    REQUIRE_EQ(static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(directory),
                                                      std::filesystem::directory_iterator{})),
               1U);
    REQUIRE_EQ(static_cast<std::size_t>(
                   std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{})),
               2U);
}

}  // namespace

// Purpose: Preserve path identity across native spelling changes while rejecting corrupt or unrelated diagnostics.
// Inputs: Unicode fixture names, an existing parent-component alias and a distinct object with identical contents.
// Outputs: Exact-prefix and UTF-8 rejection controls; only the same complete filename and object admit aliases.
TEST_CASE(path_diagnostic_oracle_resolves_aliases_without_accepting_other_objects) {
    const auto root = test_temp_dir("path-diagnostic-alias-oracle");
    const auto directory = root / std::filesystem::path(L"\u65e5\u672c-\U0001F4C1");
    const auto other_directory = root / "other";
    std::filesystem::create_directory(directory);
    std::filesystem::create_directory(other_directory);
    const auto name = std::filesystem::path(L"\u4e2d\U0001F4C1.bin");
    const auto original = directory / name;
    const auto other = other_directory / name;
    for (const auto& file : {original, other}) {
        std::ofstream output(file, std::ios::binary);
        output << "same contents, different identity";
        REQUIRE_TRUE(output.good());
    }
    constexpr std::string_view prefix = "fixture refusal: ";
    const auto diagnostic = std::string(prefix) + diagnostic_fixture_path_text(original);
    // Ordinary Win32 paths normalize '..'; the extended namespace deliberately rejects that syntax.
    const auto alias = flitzzip::windows_regular_path(other_directory) / ".." / directory.filename() / name;
    const auto invalid_extended = other_directory / ".." / directory.filename() / name;
    REQUIRE_TRUE(alias != original);
    REQUIRE_TRUE(flitzzip_test::path_diagnostic_matches(diagnostic, prefix, original));
    REQUIRE_TRUE(flitzzip_test::path_diagnostic_matches(diagnostic, prefix, alias));
    REQUIRE_TRUE(!flitzzip_test::path_diagnostic_matches(diagnostic, prefix, invalid_extended));
    REQUIRE_TRUE(!flitzzip_test::path_diagnostic_matches(diagnostic, prefix, other));
    REQUIRE_TRUE(!flitzzip_test::path_diagnostic_matches(diagnostic, "wrong refusal: ", original));
    REQUIRE_TRUE(!flitzzip_test::path_diagnostic_matches(std::string(prefix), prefix, original));
    REQUIRE_TRUE(!flitzzip_test::path_diagnostic_matches(diagnostic + std::string("\0extra", 6), prefix, original));
    REQUIRE_TRUE(!flitzzip_test::path_diagnostic_matches(diagnostic + static_cast<char>(0xFF), prefix, original));
    REQUIRE_TRUE(!flitzzip_test::path_diagnostic_matches(std::string(prefix) + diagnostic_fixture_path_text(name),
                                                         prefix, original));
    REQUIRE_TRUE(
        !flitzzip_test::path_diagnostic_matches(diagnostic.substr(0, diagnostic.size() - 1U), prefix, original));
    const auto missing = directory / "missing.bin";
    REQUIRE_TRUE(flitzzip_test::path_diagnostic_matches(std::string(prefix) + diagnostic_fixture_path_text(missing),
                                                        prefix, missing));
    REQUIRE_TRUE(!std::filesystem::exists(missing));
    REQUIRE_EQ(diagnostic_fixture_bytes(original), diagnostic_fixture_bytes(other));
}

// Purpose: Preserve Base64's rejection and recovery contracts for Unicode filesystem parents.
// Inputs: Bounded binary fixture and real public codec. Outputs: Exact errors, byte preservation and readback.
TEST_CASE(path_diagnostic_base64_unicode_failures_preserve_state_and_recover) {
    require_text_codec_unicode_contract("Base64", flitzzip::compress_base64_file, flitzzip::extract_base64_file);
}

// Purpose: Preserve UUencode's rejection and recovery contracts for Unicode filesystem parents.
// Inputs: Bounded binary fixture and real public codec. Outputs: Exact errors, byte preservation and readback.
TEST_CASE(path_diagnostic_uue_unicode_failures_preserve_state_and_recover) {
    require_text_codec_unicode_contract("UUE", flitzzip::compress_uue_file, flitzzip::extract_uue_file);
}

// Purpose: Preserve XXencode's rejection and recovery contracts for Unicode filesystem parents.
// Inputs: Bounded binary fixture and real public codec. Outputs: Exact errors, byte preservation and readback.
TEST_CASE(path_diagnostic_xxe_unicode_failures_preserve_state_and_recover) {
    require_text_codec_unicode_contract("XXE", flitzzip::compress_xxe_file, flitzzip::extract_xxe_file);
}

// Purpose: Reject a missing Unicode scan target before locating or launching Defender.
// Inputs: An absent supplementary-Unicode path and both enabled scan modes. Outputs: Exact admission errors, no I/O.
TEST_CASE(path_diagnostic_defender_missing_unicode_target_rejects_before_launch) {
    const auto root = test_temp_dir("path-diagnostic-defender-missing");
    const auto missing = root / std::filesystem::path(L"\u65e5\u672c-\U0001F4C1.bin");
    for (const auto mode : {flitzzip::DefenderScanMode::QuickMetadata, flitzzip::DefenderScanMode::FullPath}) {
        require_path_error<flitzzip::ArchiveError>([&] { (void)flitzzip::scan_with_windows_defender(missing, mode); },
                                                   "Defender scan target does not exist: ", missing);
    }
    const auto disabled = flitzzip::scan_with_windows_defender(missing, flitzzip::DefenderScanMode::Disabled);
    REQUIRE_TRUE(!disabled.attempted);
    REQUIRE_TRUE(std::filesystem::is_empty(root));
}

// Purpose: Keep Defender's fail-closed outcome from being masked by Unicode diagnostic conversion.
// Inputs: Missing, timeout, failed and clean result records; no scanner process is launched.
// Outputs: Every nonclean result rejects with SecurityError and a complete path; only a positive clean result passes.
TEST_CASE(path_diagnostic_defender_unicode_outcomes_remain_fail_closed) {
    const auto root = test_temp_dir("path-diagnostic-defender-outcomes");
    const auto path = root / std::filesystem::path(L"\u65e5\u672c-\U0001F4C1.bin");
    for (const auto result :
         {flitzzip::DefenderScanResult{}, flitzzip::DefenderScanResult{.attempted = true, .clean = false},
          flitzzip::DefenderScanResult{.attempted = true, .clean = true, .timed_out = true},
          flitzzip::DefenderScanResult{.attempted = true, .clean = true, .exit_code = 5}}) {
        require_path_error<flitzzip::SecurityError>(
            [&] { flitzzip::require_clean_defender_scan(result, path); },
            "Microsoft Defender did not positively report the target as clean: ", path);
    }
    flitzzip::require_clean_defender_scan({.attempted = true, .clean = true}, path);
    REQUIRE_TRUE(std::filesystem::is_empty(root));
}
