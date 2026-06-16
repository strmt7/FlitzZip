#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "tar/tar_adapter.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

// Purpose: Write exact text bytes to a fixture file.
// Inputs: `path` is the destination and `text` is the payload.
// Outputs: Creates parent directories and writes the file.
void write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// Purpose: Read a full file as text bytes for equality checks.
// Inputs: `path` is the file to read.
// Outputs: Returns the complete payload.
std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Purpose: Count regular files below a directory for no-output assertions.
// Inputs: `root` is the directory tree to inspect.
// Outputs: Returns the number of regular files; missing roots count as zero.
std::uint64_t count_regular_files(const std::filesystem::path& root) {
    if (!std::filesystem::exists(root)) {
        return 0;
    }
    std::uint64_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

}  // namespace

// Purpose: Verify `.tar.bz2` archives preserve nested regular files and directories.
// Inputs: A small directory tree compressed through the TAR+Bzip2 adapter.
// Outputs: Throws if detection, extraction, or restored payloads regress.
TEST_CASE(tar_bzip2_roundtrip_nested_tree) {
    const auto root = test_temp_dir("tar-bzip2-roundtrip");
    const auto input = root / "input";
    write_text_file(input / "alpha.txt", "alpha payload");
    write_text_file(input / "nested" / "beta.txt", "beta payload");
    std::filesystem::create_directories(input / "empty-dir");

    const auto archive = root / "archive.tar.bz2";
    const auto compress_stats = superzip::compress_tar_bzip2({input}, archive);
    REQUIRE_TRUE(compress_stats.entries >= static_cast<std::uint64_t>(4));
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::TarBzip2);

    const auto output = root / "out";
    const auto extract_stats = superzip::extract_tar_bzip2(archive, output, false);
    REQUIRE_TRUE(extract_stats.entries >= static_cast<std::uint64_t>(4));
    REQUIRE_EQ(read_text_file(output / "input" / "alpha.txt"), "alpha payload");
    REQUIRE_EQ(read_text_file(output / "input" / "nested" / "beta.txt"), "beta payload");
    REQUIRE_TRUE(std::filesystem::is_directory(output / "input" / "empty-dir"));
}

// Purpose: Verify `.tar.bz2` extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A valid archive and a preexisting destination file.
// Outputs: Throws if extraction overwrites while `overwrite` is false.
TEST_CASE(tar_bzip2_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("tar-bzip2-overwrite");
    const auto input = root / "input";
    write_text_file(input / "alpha.txt", "new payload");
    const auto archive = root / "archive.tbz2";
    (void)superzip::compress_tar_bzip2({input}, archive);

    const auto output = root / "out";
    write_text_file(output / "input" / "alpha.txt", "old payload");

    bool rejected = false;
    try {
        (void)superzip::extract_tar_bzip2(archive, output, false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "input" / "alpha.txt"), "old payload");
}

// Purpose: Verify malformed `.tar.bz2` input does not publish files.
// Inputs: A file with TAR+Bzip2 extension but invalid Bzip2 content.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(tar_bzip2_rejects_bad_stream_without_output) {
    const auto root = test_temp_dir("tar-bzip2-bad-stream");
    const auto archive = root / "bad.tar.bz2";
    write_text_file(archive, "not bzip2");

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_tar_bzip2(archive, output, false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}
