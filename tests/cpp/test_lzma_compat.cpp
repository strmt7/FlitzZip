#include "test_compat_fixture.hpp"
#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "lzma/lzma_adapter.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

// Independent fixture: Python 3.13 liblzma output, verified with 7-Zip 26.03.
constexpr std::array<unsigned char, 65> kSingleFileLzmaFixture{{
    0x5DU, 0x00U, 0x00U, 0x80U, 0x00U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0x00U, 0x22U, 0x9EU, 0x08U, 0x26U, 0xFFU, 0x96U, 0x34U, 0x91U, 0xE7U, 0x71U, 0xDDU, 0x2EU,
    0xADU, 0x49U, 0x72U, 0xC3U, 0xDEU, 0xC5U, 0x9DU, 0x9CU, 0x34U, 0x17U, 0xBDU, 0x12U, 0xFFU,
    0xA7U, 0xD0U, 0x2FU, 0x0EU, 0x27U, 0xBBU, 0x66U, 0xB0U, 0x91U, 0xAAU, 0x58U, 0xCEU, 0x40U,
    0x4EU, 0x29U, 0xF8U, 0x26U, 0x46U, 0x88U, 0x0EU, 0x3DU, 0x1FU, 0xFEU, 0x0BU, 0x40U, 0x00U,
}};

// Purpose: Write exact fixture bytes to disk.
// Inputs: `path` is the destination and `bytes` contains a complete `.lzma` stream.
// Outputs: Creates or replaces the file.
template <std::size_t Size>
void write_fixture(const std::filesystem::path& path, const std::array<unsigned char, Size>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Purpose: Write exact unsigned bytes to disk.
// Inputs: `path` is the destination and `bytes` is the exact payload.
// Outputs: Creates or replaces the file.
void write_binary_file(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Purpose: Read a full text file for equality checks.
// Inputs: `path` is the file to read.
// Outputs: Returns the complete file payload.
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

// Purpose: Verify native `.lzma` extraction over a deterministic LZMA-Alone fixture.
// Inputs: A small LZMA-Alone stream generated from a single-file payload.
// Outputs: Throws if format detection, extraction, or restored bytes regress.
TEST_CASE(lzma_extracts_single_file_fixture) {
    const auto root = test_temp_dir("lzma-single");
    const auto archive = root / "payload.txt.lzma";
    write_fixture(archive, kSingleFileLzmaFixture);
    REQUIRE_EQ(flitzzip::detect_archive_format(archive), flitzzip::ArchiveFormat::Lzma);

    const auto output = root / "out";
    const auto stats = flitzzip::extract_lzma_file(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(read_text_file(output / "payload.txt"), "Example LZMA fixture payload.\nSecond line.\n");
    flitzzip_test::export_compat_fixture(archive, output);
}

// Purpose: Preserve native Unicode filenames while extracting independent lzma fixtures.
// Inputs: A bounded upstream-compatible fixture with an uppercase suffix and a Unicode host filename.
// Outputs: Requires exact restored bytes and overwrite refusal without encoding errors.
TEST_CASE(lzma_unicode_archive_filename_extracts) {
    const auto root = test_temp_dir("lzma-unicode-name");
    const auto filename = std::filesystem::path(u8"\u6E2C\u8A66\U0001F4E6.txt");
    auto archive = root / filename;
    archive += ".LZMA";
    write_fixture(archive, kSingleFileLzmaFixture);
    const auto output = root / "out";
    (void)flitzzip::extract_lzma_file(archive, output, false);
    REQUIRE_EQ(read_text_file(output / filename), "Example LZMA fixture payload.\nSecond line.\n");
    bool refused = false;
    try {
        (void)flitzzip::extract_lzma_file(archive, output, false);
    } catch (const flitzzip::SecurityError&) {
        refused = true;
    }
    REQUIRE_TRUE(refused);
    REQUIRE_EQ(read_text_file(output / filename), "Example LZMA fixture payload.\nSecond line.\n");
}

// Purpose: Verify `.lzma` extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A valid `.lzma` stream and a preexisting destination file.
// Outputs: Throws if extraction overwrites while `overwrite` is false.
TEST_CASE(lzma_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("lzma-overwrite");
    const auto archive = root / "payload.txt.lzma";
    write_fixture(archive, kSingleFileLzmaFixture);
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    {
        std::ofstream(output / "payload.txt") << "old";
    }

    bool rejected = false;
    try {
        (void)flitzzip::extract_lzma_file(archive, output, false);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "payload.txt"), "old");
}

// Purpose: Verify malformed `.lzma` streams do not publish output.
// Inputs: A file with a valid-looking suffix but incomplete LZMA-Alone header.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(lzma_rejects_truncated_header_without_output) {
    const auto root = test_temp_dir("lzma-truncated-header");
    const auto archive = root / "bad.lzma";
    write_binary_file(archive, {0x5DU, 0x00U, 0x00U, 0x80U});

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_lzma_file(archive, output, false);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}

// Purpose: Verify corrupt LZMA payloads do not publish partial output.
// Inputs: A complete header followed by invalid compressed payload bytes.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(lzma_rejects_corrupt_payload_without_output) {
    const auto root = test_temp_dir("lzma-corrupt-payload");
    const auto archive = root / "payload.bin.lzma";
    write_binary_file(archive, {
                                   0x5DU,
                                   0x00U,
                                   0x00U,
                                   0x80U,
                                   0x00U,
                                   0x05U,
                                   0x00U,
                                   0x00U,
                                   0x00U,
                                   0x00U,
                                   0x00U,
                                   0x00U,
                                   0x00U,
                                   0x00U,
                                   0x00U,
                                   0x00U,
                               });

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_lzma_file(archive, output, false);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}

// Purpose: Verify `.lzma` streams with trailing data after the end marker fail closed.
// Inputs: A valid LZMA-Alone stream with one extra byte appended.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(lzma_rejects_trailing_data_after_end_marker) {
    const auto root = test_temp_dir("lzma-trailing-data");
    const auto archive = root / "payload.txt.lzma";
    std::vector<unsigned char> bytes(kSingleFileLzmaFixture.begin(), kSingleFileLzmaFixture.end());
    bytes.push_back(0x00U);
    write_binary_file(archive, bytes);

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_lzma_file(archive, output, false);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}

// Purpose: Verify hostile dictionary declarations are rejected before decoder allocation.
// Inputs: A valid LZMA property byte with a dictionary larger than FlitzZip policy.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(lzma_rejects_oversized_dictionary_without_output) {
    const auto root = test_temp_dir("lzma-oversized-dict");
    const auto archive = root / "huge.lzma";
    write_binary_file(archive, {
                                   0x5DU,
                                   0xFFU,
                                   0xFFU,
                                   0xFFU,
                                   0x7FU,
                                   0xFFU,
                                   0xFFU,
                                   0xFFU,
                                   0xFFU,
                                   0xFFU,
                                   0xFFU,
                                   0xFFU,
                                   0xFFU,
                                   0x00U,
                               });

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_lzma_file(archive, output, false);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}
