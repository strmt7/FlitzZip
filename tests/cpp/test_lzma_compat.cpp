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

constexpr std::array<unsigned char, 68> kSingleFileLzmaFixture{{
    0x5DU, 0x00U, 0x00U, 0x80U, 0x00U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0x00U, 0x29U, 0x9DU, 0x4AU, 0x06U, 0x67U, 0x91U, 0xC2U, 0xBFU, 0xC5U, 0x03U,
    0x48U, 0x70U, 0x03U, 0x6CU, 0xFDU, 0xF9U, 0x68U, 0xEDU, 0x8BU, 0x91U, 0xC4U, 0x45U,
    0x84U, 0x1EU, 0x3CU, 0xC6U, 0xD9U, 0xC4U, 0xA4U, 0x44U, 0xCEU, 0xB9U, 0xB4U, 0xB1U,
    0xC6U, 0x33U, 0xF0U, 0xDEU, 0x9AU, 0x42U, 0x96U, 0x4FU, 0xFFU, 0xD3U, 0xA8U, 0xCDU,
    0xD9U, 0xD9U, 0x83U, 0xFFU, 0xE1U, 0xDFU, 0x80U, 0x00U,
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
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Lzma);

    const auto output = root / "out";
    const auto stats = superzip::extract_lzma_file(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(read_text_file(output / "payload.txt"), "SuperZip LZMA fixture payload.\nSecond line.\n");
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
        (void)superzip::extract_lzma_file(archive, output, false);
    } catch (const superzip::SecurityError&) {
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
        (void)superzip::extract_lzma_file(archive, output, false);
    } catch (const superzip::ArchiveError&) {
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
        0x5DU, 0x00U, 0x00U, 0x80U, 0x00U,
        0x05U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U,
    });

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_lzma_file(archive, output, false);
    } catch (const superzip::ArchiveError&) {
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
        (void)superzip::extract_lzma_file(archive, output, false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}

// Purpose: Verify hostile dictionary declarations are rejected before decoder allocation.
// Inputs: A valid LZMA property byte with a dictionary larger than SuperZip policy.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(lzma_rejects_oversized_dictionary_without_output) {
    const auto root = test_temp_dir("lzma-oversized-dict");
    const auto archive = root / "huge.lzma";
    write_binary_file(archive, {
        0x5DU, 0xFFU, 0xFFU, 0xFFU, 0x7FU,
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
        0x00U,
    });

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_lzma_file(archive, output, false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}
