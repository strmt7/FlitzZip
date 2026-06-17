#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "lzip/lzip_adapter.hpp"
#include "tar/tar_adapter.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr std::array<unsigned char, 79> kSingleFileLzipFixture{{
    0x4CU, 0x5AU, 0x49U, 0x50U, 0x01U, 0x14U, 0x00U, 0x29U, 0x9DU, 0x4AU, 0x06U, 0x67U,
    0x91U, 0xC2U, 0xBFU, 0xC5U, 0x03U, 0x60U, 0x46U, 0xBDU, 0xEEU, 0x82U, 0x94U, 0xECU,
    0xC2U, 0xB8U, 0xF3U, 0x54U, 0xADU, 0x7FU, 0xE6U, 0x87U, 0xEAU, 0xACU, 0x72U, 0x70U,
    0xD0U, 0xE5U, 0xB9U, 0x5CU, 0x71U, 0xB8U, 0x10U, 0x02U, 0xDFU, 0x74U, 0xD6U, 0xCEU,
    0x62U, 0xE3U, 0xAEU, 0xD4U, 0xCEU, 0xF7U, 0xFFU, 0xFEU, 0xEFU, 0x4CU, 0x00U, 0x6AU,
    0x72U, 0xF8U, 0xD6U, 0x2CU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x4FU,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
}};

constexpr std::array<unsigned char, 172> kTarLzipFixture{{
    0x4CU, 0x5AU, 0x49U, 0x50U, 0x01U, 0x14U, 0x00U, 0x32U, 0x1AU, 0x4AU, 0xA4U, 0x14U,
    0xD4U, 0x01U, 0xC9U, 0x6AU, 0xAEU, 0x77U, 0x6EU, 0x91U, 0xBBU, 0x40U, 0x50U, 0xF6U,
    0xF9U, 0x33U, 0xEFU, 0xE6U, 0xA6U, 0x45U, 0x06U, 0x00U, 0xA4U, 0xFBU, 0x0DU, 0xF1U,
    0xCBU, 0xB5U, 0xD8U, 0xFDU, 0x3EU, 0x76U, 0x36U, 0xBDU, 0x6FU, 0xF8U, 0x21U, 0x34U,
    0x57U, 0x24U, 0x56U, 0x19U, 0x6EU, 0x95U, 0xE7U, 0x20U, 0xDBU, 0xD4U, 0x9BU, 0xFCU,
    0x81U, 0x35U, 0x16U, 0xFEU, 0xE9U, 0xDDU, 0x2EU, 0x8EU, 0xF1U, 0xC3U, 0x68U, 0xECU,
    0xCBU, 0x9AU, 0x9EU, 0x99U, 0x8EU, 0xFEU, 0x68U, 0x1CU, 0xEAU, 0x74U, 0x49U, 0xD0U,
    0xE1U, 0xACU, 0x09U, 0x6FU, 0xB9U, 0x13U, 0x1AU, 0xDEU, 0x66U, 0xF7U, 0x2DU, 0x2BU,
    0x1BU, 0xDAU, 0x9CU, 0xDDU, 0x3FU, 0x3BU, 0x04U, 0xC9U, 0x6EU, 0xBFU, 0x3BU, 0x20U,
    0xB5U, 0xD6U, 0xD7U, 0x62U, 0x29U, 0x34U, 0x22U, 0x1AU, 0x05U, 0x09U, 0xC8U, 0x91U,
    0x93U, 0xB8U, 0x75U, 0xE5U, 0x56U, 0x55U, 0xD4U, 0xF6U, 0xD6U, 0xBEU, 0x7DU, 0x05U,
    0x36U, 0x33U, 0xFEU, 0xB6U, 0xCCU, 0x65U, 0x2BU, 0xF9U, 0x43U, 0xCAU, 0xA2U, 0xC2U,
    0x05U, 0x90U, 0xFFU, 0xFFU, 0xDDU, 0xF3U, 0xF9U, 0x1BU, 0x80U, 0x60U, 0xD5U, 0xD3U,
    0x00U, 0x0EU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xACU, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U,
}};

constexpr std::array<unsigned char, 104> kUnsafeTarLzipFixture{{
    0x4CU, 0x5AU, 0x49U, 0x50U, 0x01U, 0x14U, 0x00U, 0x17U, 0x60U, 0xB8U, 0xCAU, 0x73U,
    0x30U, 0x94U, 0xF6U, 0x84U, 0x5EU, 0x11U, 0x4EU, 0xE8U, 0x52U, 0xFDU, 0xE4U, 0xA5U,
    0x5CU, 0x7CU, 0x19U, 0x45U, 0x22U, 0x67U, 0x88U, 0xA1U, 0xAEU, 0xE9U, 0x75U, 0x6FU,
    0x9AU, 0x2FU, 0x98U, 0xADU, 0xC1U, 0x5FU, 0x5EU, 0xADU, 0x8AU, 0xDBU, 0x1FU, 0x37U,
    0xF5U, 0x8DU, 0x15U, 0x67U, 0x1FU, 0xCCU, 0xE1U, 0x07U, 0x02U, 0xB3U, 0x52U, 0x32U,
    0x19U, 0xB8U, 0xBCU, 0xB6U, 0x1CU, 0x81U, 0x26U, 0xD5U, 0xF9U, 0xE8U, 0x60U, 0x94U,
    0x11U, 0x6CU, 0x36U, 0x98U, 0x94U, 0x39U, 0xDFU, 0xFFU, 0xFBU, 0xD0U, 0x46U, 0x00U,
    0x1AU, 0x36U, 0x71U, 0x7DU, 0x00U, 0x08U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x68U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
}};

// Purpose: Write exact fixture bytes to disk.
// Inputs: `path` is the destination and `bytes` contains a complete lzip stream.
// Outputs: Creates or replaces the file.
template <std::size_t Size>
void write_fixture(const std::filesystem::path& path, const std::array<unsigned char, Size>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Purpose: Write exact binary bytes to disk.
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

// Purpose: Convert the single lzip fixture to a mutable byte vector.
// Inputs: None.
// Outputs: Returns a copy suitable for corruption tests.
std::vector<unsigned char> single_lzip_bytes() {
    return std::vector<unsigned char>(kSingleFileLzipFixture.begin(), kSingleFileLzipFixture.end());
}

}  // namespace

// Purpose: Verify native `.lz` extraction over a deterministic lzip fixture.
// Inputs: A small lzip stream generated from one payload.
// Outputs: Throws if format detection, extraction, or restored bytes regress.
TEST_CASE(lzip_extracts_single_file_fixture) {
    const auto root = test_temp_dir("lzip-single");
    const auto archive = root / "payload.txt.lz";
    write_fixture(archive, kSingleFileLzipFixture);
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Lzip);

    const auto output = root / "out";
    const auto stats = superzip::extract_lzip_file(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(read_text_file(output / "payload.txt"), "SuperZip lzip fixture payload.\nSecond line.\n");
}

// Purpose: Verify concatenated lzip members decode as one continuous single-file payload.
// Inputs: Two valid lzip members written back-to-back.
// Outputs: Throws if the decoder fails to validate both members or drops member data.
TEST_CASE(lzip_extracts_concatenated_members) {
    const auto root = test_temp_dir("lzip-concat");
    const auto archive = root / "payload.txt.lz";
    auto bytes = single_lzip_bytes();
    bytes.insert(bytes.end(), kSingleFileLzipFixture.begin(), kSingleFileLzipFixture.end());
    write_binary_file(archive, bytes);

    const auto output = root / "out";
    const auto stats = superzip::extract_lzip_file(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(
        read_text_file(output / "payload.txt"),
        "SuperZip lzip fixture payload.\nSecond line.\nSuperZip lzip fixture payload.\nSecond line.\n");
}

// Purpose: Verify `.lz` extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A valid lzip stream and a preexisting destination file.
// Outputs: Throws if extraction overwrites while `overwrite` is false.
TEST_CASE(lzip_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("lzip-overwrite");
    const auto archive = root / "payload.txt.lz";
    write_fixture(archive, kSingleFileLzipFixture);
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    {
        std::ofstream(output / "payload.txt") << "old";
    }

    bool rejected = false;
    try {
        (void)superzip::extract_lzip_file(archive, output, false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "payload.txt"), "old");
}

// Purpose: Verify lzip CRC mismatches do not publish partial output.
// Inputs: A valid lzip stream with one trailer CRC byte corrupted.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(lzip_rejects_crc_mismatch_without_output) {
    const auto root = test_temp_dir("lzip-crc");
    const auto archive = root / "payload.txt.lz";
    auto bytes = single_lzip_bytes();
    REQUIRE_TRUE(bytes.size() > 20U);
    bytes[bytes.size() - 20U] ^= 0xFFU;
    write_binary_file(archive, bytes);

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_lzip_file(archive, output, false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}

// Purpose: Verify lzip member-size mismatches fail closed.
// Inputs: A valid lzip stream with the trailer member-size field corrupted.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(lzip_rejects_member_size_mismatch_without_output) {
    const auto root = test_temp_dir("lzip-member-size");
    const auto archive = root / "payload.txt.lz";
    auto bytes = single_lzip_bytes();
    REQUIRE_TRUE(bytes.size() > 8U);
    bytes[bytes.size() - 1U] ^= 0x01U;
    write_binary_file(archive, bytes);

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_lzip_file(archive, output, false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}

// Purpose: Verify hostile lzip dictionary-size codes are rejected before decoder allocation.
// Inputs: A lzip header with DS below the specification's minimum base logarithm.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(lzip_rejects_invalid_dictionary_code_without_output) {
    const auto root = test_temp_dir("lzip-dictionary");
    const auto archive = root / "bad.lz";
    write_binary_file(archive, {'L', 'Z', 'I', 'P', 1U, 0U, 0U});

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_lzip_file(archive, output, false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}

// Purpose: Verify in-process `.tar.lz` compatibility extraction for files and directories.
// Inputs: A deterministic lzip-compressed TAR fixture.
// Outputs: Throws if extraction fails, format detection is wrong, or restored contents differ.
TEST_CASE(tar_lzip_extracts_files_and_directories) {
    const auto root = test_temp_dir("tar-lzip-extract");
    const auto archive = root / "sample.tar.lz";
    write_fixture(archive, kTarLzipFixture);
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::TarLzip);

    const auto output = root / "out";
    const auto stats = superzip::extract_tar_lzip(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(3));
    REQUIRE_EQ(read_text_file(output / "dir" / "hello.txt"), "hello from tar.lz\n");
    REQUIRE_EQ(read_text_file(output / "root.txt"), "root tar lzip payload");
}

// Purpose: Verify `.tar.lz` extraction rejects traversal metadata during the validation pass.
// Inputs: A deterministic TAR.LZ fixture containing `../escape.txt`.
// Outputs: Throws if extraction accepts the unsafe path or writes any output.
TEST_CASE(tar_lzip_extraction_rejects_traversal_before_output) {
    const auto root = test_temp_dir("tar-lzip-traversal");
    const auto archive = root / "bad.tlz";
    write_fixture(archive, kUnsafeTarLzipFixture);
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::TarLzip);

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_tar_lzip(archive, root / "out", false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}
