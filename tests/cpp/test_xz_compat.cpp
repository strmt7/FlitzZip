#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "xz/xz_adapter.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr std::array<unsigned char, 100> kSingleFileXzFixture{{
    0xFDU, 0x37U, 0x7AU, 0x58U, 0x5AU, 0x00U, 0x00U, 0x04U, 0xE6U, 0xD6U, 0xB4U, 0x46U,
    0x02U, 0x00U, 0x21U, 0x01U, 0x16U, 0x00U, 0x00U, 0x00U, 0x74U, 0x2FU, 0xE5U, 0xA3U,
    0x01U, 0x00U, 0x29U, 0x53U, 0x75U, 0x70U, 0x65U, 0x72U, 0x5AU, 0x69U, 0x70U, 0x20U,
    0x58U, 0x5AU, 0x20U, 0x66U, 0x69U, 0x78U, 0x74U, 0x75U, 0x72U, 0x65U, 0x20U, 0x70U,
    0x61U, 0x79U, 0x6CU, 0x6FU, 0x61U, 0x64U, 0x2EU, 0x0AU, 0x53U, 0x65U, 0x63U, 0x6FU,
    0x6EU, 0x64U, 0x20U, 0x6CU, 0x69U, 0x6EU, 0x65U, 0x2EU, 0x0AU, 0x00U, 0x00U, 0x00U,
    0x69U, 0x30U, 0x60U, 0x81U, 0xBFU, 0x14U, 0x46U, 0x9EU, 0x00U, 0x01U, 0x42U, 0x2AU,
    0x7AU, 0x51U, 0x72U, 0x39U, 0x1FU, 0xB6U, 0xF3U, 0x7DU, 0x01U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x04U, 0x59U, 0x5AU,
}};

// Purpose: Write exact fixture bytes to disk.
// Inputs: `path` is the destination and `bytes` contains a complete `.xz` stream.
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

// Purpose: Verify native `.xz` extraction over a CRC64 fixture.
// Inputs: A deterministic `.xz` file generated from a small single-file payload.
// Outputs: Throws if format detection, extraction, or restored bytes regress.
TEST_CASE(xz_extracts_single_file_fixture) {
    const auto root = test_temp_dir("xz-single");
    const auto archive = root / "payload.txt.xz";
    write_fixture(archive, kSingleFileXzFixture);
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Xz);

    const auto output = root / "out";
    const auto stats = superzip::extract_xz_file(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(read_text_file(output / "payload.txt"), "SuperZip XZ fixture payload.\nSecond line.\n");
}

// Purpose: Verify `.xz` extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A valid `.xz` stream and a preexisting destination file.
// Outputs: Throws if extraction overwrites while `overwrite` is false.
TEST_CASE(xz_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("xz-overwrite");
    const auto archive = root / "payload.txt.xz";
    write_fixture(archive, kSingleFileXzFixture);
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    {
        std::ofstream(output / "payload.txt") << "old";
    }

    bool rejected = false;
    try {
        (void)superzip::extract_xz_file(archive, output, false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "payload.txt"), "old");
}

// Purpose: Verify malformed `.xz` streams do not publish output.
// Inputs: A file with the `.xz` extension but invalid XZ magic/content.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(xz_rejects_bad_magic_without_output) {
    const auto root = test_temp_dir("xz-bad-magic");
    const auto archive = root / "bad.xz";
    write_binary_file(archive, {0xFDU, 0x37U, 0x7AU, 0x00U});

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_xz_file(archive, output, false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}
