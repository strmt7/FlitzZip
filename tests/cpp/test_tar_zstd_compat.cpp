#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "tar/tar_adapter.hpp"
#include "zstd/zstd_adapter.hpp"
#include "zstd/zstd_stream.hpp"

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

// Purpose: Verify `.tar.zst` archives preserve nested regular files and directories.
// Inputs: A small directory tree compressed through the TAR+Zstandard adapter.
// Outputs: Throws if detection, extraction, or restored payloads regress.
TEST_CASE(tar_zstd_roundtrip_nested_tree) {
    const auto root = test_temp_dir("tar-zstd-roundtrip");
    const auto input = root / "input";
    write_text_file(input / "alpha.txt", "alpha payload");
    write_text_file(input / "nested" / "beta.txt", "beta payload");
    std::filesystem::create_directories(input / "empty-dir");

    const auto archive = root / "archive.tar.zst";
    const auto compress_stats = superzip::compress_tar_zstd({input}, archive);
    REQUIRE_TRUE(compress_stats.entries >= static_cast<std::uint64_t>(4));
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::TarZstd);

    const auto output = root / "out";
    const auto extract_stats = superzip::extract_tar_zstd(archive, output, false);
    REQUIRE_TRUE(extract_stats.entries >= static_cast<std::uint64_t>(4));
    REQUIRE_EQ(read_text_file(output / "input" / "alpha.txt"), "alpha payload");
    REQUIRE_EQ(read_text_file(output / "input" / "nested" / "beta.txt"), "beta payload");
    REQUIRE_TRUE(std::filesystem::is_directory(output / "input" / "empty-dir"));
}

// Purpose: Verify `.tzst` extension aliases route to TAR+Zstandard handling.
// Inputs: A small directory tree compressed to `.tzst`.
// Outputs: Throws if alias detection or extraction regresses.
TEST_CASE(tar_zstd_tzst_alias_roundtrip) {
    const auto root = test_temp_dir("tar-zstd-alias");
    const auto input = root / "input";
    write_text_file(input / "sample.txt", "tzst payload");

    const auto archive = root / "archive.tzst";
    (void)superzip::compress_tar_zstd({input}, archive);
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::TarZstd);

    const auto output = root / "out";
    (void)superzip::extract_tar_zstd(archive, output, false);
    REQUIRE_EQ(read_text_file(output / "input" / "sample.txt"), "tzst payload");
}

// Purpose: Verify `.tar.zst` extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A valid archive and a preexisting destination file.
// Outputs: Throws if extraction overwrites while `overwrite` is false.
TEST_CASE(tar_zstd_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("tar-zstd-overwrite");
    const auto input = root / "input";
    write_text_file(input / "alpha.txt", "new payload");
    const auto archive = root / "archive.tar.zst";
    (void)superzip::compress_tar_zstd({input}, archive);

    const auto output = root / "out";
    write_text_file(output / "input" / "alpha.txt", "old payload");

    bool rejected = false;
    try {
        (void)superzip::extract_tar_zstd(archive, output, false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "input" / "alpha.txt"), "old payload");
}

// Purpose: Verify malformed `.tar.zst` input does not publish files.
// Inputs: A file with TAR+Zstandard extension but invalid Zstandard content.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(tar_zstd_rejects_bad_stream_without_output) {
    const auto root = test_temp_dir("tar-zstd-bad-stream");
    const auto archive = root / "bad.tar.zst";
    write_text_file(archive, "not zstandard");

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_tar_zstd(archive, output, false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}

// Purpose: Prove TAR.ZST admits the exact serialized TAR size, including PAX records and block padding.
// Inputs: Empty-directory and mixed boundary trees, long ASCII/UTF-8 paths, and all nine compression efforts.
// Outputs: Requires wire parity with exact-size standalone compression of the same TAR and exact extracted files.
TEST_CASE(tar_zstd_exact_serialized_size_parity) {
    const auto root = test_temp_dir("tar-zstd-size-parity");
    const auto input = root / "input";
    std::filesystem::create_directories(input / "empty-dir");
    for (const bool populated : {false, true}) {
        if (populated) {
            for (const auto size : {0U, 1U, 511U, 512U, 513U, 65537U}) {
                write_text_file(input / ("size-" + std::to_string(size)), std::string(size, 'A'));
            }
            write_text_file(input / std::string(100U, 'n'), "USTAR name boundary");
            write_text_file(input / std::string(101U, 'p'), "PAX path payload");
            std::u8string unicode_name;
            for (unsigned int index = 0; index < 51U; ++index) {
                unicode_name += u8"\u00e9";
            }
            write_text_file(input / std::filesystem::path(unicode_name), "UTF-8 PAX path payload");
            write_text_file(input / std::filesystem::path(u8"\u65e5\u672c\U0001f4c1") /
                                std::filesystem::path(u8"caf\u00e9.txt"),
                            "short Unicode path payload");
        }
        const auto plain = root / "reference.tar";
        const auto plain_stats = superzip::compress_tar({input}, plain);
        const auto serialized = read_text_file(plain);
        REQUIRE_EQ(serialized.size() % 512U, 0U);
        for (int level = 1; level <= 9; ++level) {
            const auto direct = root / "direct.tar.zst";
            const auto expected = root / "reference.tar.zst";
            const auto direct_stats = superzip::compress_tar_zstd({input}, direct, level);
            (void)superzip::compress_zstd({plain}, expected, level);
            REQUIRE_EQ(read_text_file(direct), read_text_file(expected));
            REQUIRE_EQ(direct_stats.input_bytes, plain_stats.input_bytes);
            REQUIRE_EQ(direct_stats.entries, plain_stats.entries);
            REQUIRE_EQ(direct_stats.output_bytes, std::filesystem::file_size(direct));
            superzip::ZstdInputStream decoded(direct);
            const std::string restored{std::istreambuf_iterator<char>(decoded), std::istreambuf_iterator<char>()};
            decoded.finish();
            REQUIRE_EQ(restored, serialized);
        }
        const auto destination = root / (populated ? "populated-output" : "empty-output");
        const auto extracted = superzip::extract_tar_zstd(root / "direct.tar.zst", destination, false);
        REQUIRE_EQ(extracted.output_bytes, plain_stats.input_bytes);
        REQUIRE_TRUE(std::filesystem::is_directory(destination / "input" / "empty-dir"));
        for (const auto& entry : std::filesystem::recursive_directory_iterator(input)) {
            if (entry.is_regular_file()) {
                REQUIRE_EQ(read_text_file(destination / "input" / entry.path().lexically_relative(input)),
                           read_text_file(entry.path()));
            }
        }
    }
    std::filesystem::remove_all(root);
}
