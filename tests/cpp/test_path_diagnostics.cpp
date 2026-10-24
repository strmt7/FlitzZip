#include "test_util.hpp"

#include "base64/base64_adapter.hpp"
#include "core/defender_scan.hpp"
#include "core/result.hpp"
#include "uue/uue_adapter.hpp"
#include "xxe/xxe_adapter.hpp"

#include <fstream>
#include <iterator>
#include <string_view>

namespace {

// Purpose: Require a boundary's intended error class and complete native filename, independent of Windows prefix.
// Inputs: Action, exact diagnostic prefix and the caller-selected path; only equivalent native aliases are allowed.
// Outputs: Rejects unrelated exceptions, incomplete names and arbitrary diagnostic changes.
template <typename Error, typename Action>
void require_path_error(Action&& action, std::string_view prefix, const std::filesystem::path& path) {
    bool rejected = false;
    try {
        action();
    } catch (const Error& error) {
        const auto extended = std::string(prefix) + flitzzip::path_diagnostic_utf8(path);
        const auto regular = std::string(prefix) + flitzzip::path_diagnostic_utf8(flitzzip::windows_regular_path(path));
        REQUIRE_TRUE(error.what() == extended || error.what() == regular);
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
