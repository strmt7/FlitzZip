#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "wim/wim_adapter.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

#ifndef SUPERZIP_TEST_FIXTURES_DIR
#define SUPERZIP_TEST_FIXTURES_DIR "tests/fixtures"
#endif

// Purpose: Resolve a pinned WIM fixture path for tests.
// Inputs: `name` is the fixture filename under `tests/fixtures/wim`.
// Outputs: Returns the absolute or source-relative fixture path.
std::filesystem::path wim_fixture_path(const std::string& name) {
    return std::filesystem::path(SUPERZIP_TEST_FIXTURES_DIR) / "wim" / name;
}

// Purpose: Read a small text fixture result.
// Inputs: `path` is an extracted text file.
// Outputs: Returns the full text payload or throws on I/O failure.
std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open text file: " + path.string());
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE(wim_fixture_extracts_with_native_adapter) {
    const auto root = test_temp_dir("wim-extract");
    const auto destination = root / "out";
    const auto fixture = wim_fixture_path("basic.wim");

    REQUIRE_EQ(superzip::detect_archive_format(fixture), superzip::ArchiveFormat::Wim);
    const auto& info = superzip::archive_format_info(superzip::ArchiveFormat::Wim);
    REQUIRE_TRUE(info.can_extract);
    REQUIRE_TRUE(info.bundled_native);

    const auto stats = superzip::extract_wim(fixture, destination, false);
    REQUIRE_TRUE(stats.entries >= 2U);
    REQUIRE_EQ(read_text(destination / "root.txt"), std::string("wim-root"));
    REQUIRE_EQ(read_text(destination / "nested" / "hello.txt"), std::string("hello wim"));
}

TEST_CASE(wim_extraction_refuses_to_overwrite_existing_files) {
    const auto root = test_temp_dir("wim-overwrite");
    const auto destination = root / "out";
    std::filesystem::create_directories(destination);
    {
        std::ofstream existing(destination / "root.txt", std::ios::binary);
        existing << "existing";
    }

    bool refused = false;
    try {
        (void)superzip::extract_wim(wim_fixture_path("basic.wim"), destination, false);
    } catch (const superzip::SecurityError&) {
        refused = true;
    }
    REQUIRE_TRUE(refused);
    REQUIRE_EQ(read_text(destination / "root.txt"), std::string("existing"));
}

TEST_CASE(wim_extraction_rejects_truncated_archives) {
    const auto root = test_temp_dir("wim-truncated");
    const auto corrupt = root / "truncated.wim";
    std::filesystem::copy_file(wim_fixture_path("basic.wim"), corrupt);
    std::filesystem::resize_file(corrupt, 128U);

    bool rejected = false;
    try {
        (void)superzip::extract_wim(corrupt, root / "out", false);
    } catch (const superzip::Error&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "out" / "root.txt"));
}

TEST_CASE(wim_extraction_rejects_split_wim_extension_until_multipart_support_exists) {
    const auto root = test_temp_dir("wim-swm-rejected");
    const auto split_part = root / "part.swm";
    std::filesystem::copy_file(wim_fixture_path("basic.wim"), split_part);

    REQUIRE_EQ(superzip::detect_archive_format(split_part), superzip::ArchiveFormat::SplitWim);
    bool rejected = false;
    try {
        (void)superzip::extract_wim(split_part, root / "out", false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "out" / "root.txt"));
}
