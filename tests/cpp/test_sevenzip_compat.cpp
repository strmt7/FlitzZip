#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/checksum.hpp"
#include "core/result.hpp"
#include "sevenzip/sevenzip_adapter.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr std::array<unsigned char, 174> kNestedSevenZipFixture{
    0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C, 0x00, 0x03, 0x3D, 0x43, 0x5A, 0x95, 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x53, 0x98, 0x07, 0xA4, 0x01, 0x00, 0x0F, 0x73,
    0x65, 0x76, 0x65, 0x6E, 0x7A, 0x69, 0x70, 0x20, 0x70, 0x61, 0x79, 0x6C, 0x6F, 0x61, 0x64, 0x00, 0x00, 0x00,
    0x81, 0x33, 0x07, 0xAE, 0x0F, 0xCF, 0x39, 0xB0, 0x0C, 0x07, 0xC8, 0x43, 0x7F, 0x41, 0xB1, 0xFA, 0xFD, 0xE2,
    0xFB, 0x79, 0xDB, 0x20, 0x2B, 0xAD, 0x5E, 0x2C, 0x2A, 0x08, 0x11, 0x40, 0xDD, 0xAF, 0x93, 0x26, 0x4C, 0x87,
    0xDD, 0x72, 0x24, 0xFF, 0x4E, 0x59, 0xEE, 0xE8, 0x34, 0x54, 0xB0, 0xAD, 0x39, 0x27, 0x50, 0xB2, 0xAD, 0x8D,
    0xB6, 0xC9, 0xF8, 0x8C, 0x2D, 0xE1, 0x19, 0x25, 0x71, 0xE0, 0xDE, 0x9B, 0x28, 0xC7, 0x3A, 0xA1, 0x35, 0x2D,
    0x26, 0x09, 0xCC, 0xCB, 0xC8, 0xE1, 0xCC, 0x68, 0x28, 0x86, 0xDD, 0xFC, 0xEF, 0x33, 0xC0, 0x00, 0x17, 0x06,
    0x14, 0x01, 0x09, 0x5A, 0x00, 0x07, 0x0B, 0x01, 0x00, 0x01, 0x23, 0x03, 0x01, 0x01, 0x05, 0x5D, 0x00, 0x00,
    0x40, 0x00, 0x0C, 0x5E, 0x0A, 0x01, 0xA7, 0x80, 0x03, 0x07, 0x00, 0x00,
};

// Purpose: Write an embedded 7z fixture to disk.
// Inputs: `archive` is the destination and `bytes` are the exact archive bytes.
// Outputs: Creates or replaces a small 7z fixture.
void write_7z_fixture(const std::filesystem::path& archive,
                      std::span<const unsigned char> bytes = kNestedSevenZipFixture) {
    std::ofstream output(archive, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Purpose: Append a little-endian 32-bit field to a handcrafted 7z header.
// Inputs: `bytes` receives the encoding and `value` is the decoded integer.
// Outputs: Appends four bytes.
void append_le32(std::vector<unsigned char>& bytes, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<unsigned char>((value >> shift) & 0xFFU));
    }
}

// Purpose: Append a little-endian 64-bit field to a handcrafted 7z header.
// Inputs: `bytes` receives the encoding and `value` is the decoded integer.
// Outputs: Appends eight bytes.
void append_le64(std::vector<unsigned char>& bytes, std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<unsigned char>((value >> shift) & 0xFFU));
    }
}

// Purpose: Calculate the standard 7z CRC-32 for a byte span.
// Inputs: `bytes` is the exact serialized header region.
// Outputs: Returns its CRC-32.
std::uint32_t sevenzip_crc32(std::span<const unsigned char> bytes) {
    return superzip::crc32(std::as_bytes(bytes));
}

// Purpose: Wrap an unencoded next-header body in a complete 7z signature header.
// Inputs: `next_header` contains a small attacker-controlled 7z metadata stream.
// Outputs: Returns a checksum-correct archive with no packed payload bytes.
std::vector<unsigned char> make_unencoded_7z(std::span<const unsigned char> next_header) {
    std::vector<unsigned char> start_fields;
    append_le64(start_fields, 0U);
    append_le64(start_fields, next_header.size());
    append_le32(start_fields, sevenzip_crc32(next_header));

    std::vector<unsigned char> archive{0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C, 0x00, 0x04};
    append_le32(archive, sevenzip_crc32(start_fields));
    archive.insert(archive.end(), start_fields.begin(), start_fields.end());
    archive.insert(archive.end(), next_header.begin(), next_header.end());
    return archive;
}

// Purpose: Read a full text file for extraction equality checks.
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

// Purpose: Verify native `.7z` extraction reads a nested payload through the LZMA SDK decoder.
// Inputs: An embedded 7z archive with `nested/payload.txt`.
// Outputs: Throws if detection, registry state, extraction, or stats regress.
TEST_CASE(sevenzip_extraction_reads_nested_payload) {
    const auto root = test_temp_dir("sevenzip-basic");
    const auto archive = root / "sample.7z";
    write_7z_fixture(archive);

    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::SevenZip);
    const auto info = superzip::archive_format_info(superzip::ArchiveFormat::SevenZip);
    REQUIRE_TRUE(!info.can_create);
    REQUIRE_TRUE(info.can_extract);
    REQUIRE_TRUE(info.bundled_native);
    REQUIRE_TRUE(!info.gpu_native);

    const auto output = root / "out";
    const auto stats = superzip::extract_7z(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(stats.output_bytes, static_cast<std::uint64_t>(std::string_view("sevenzip payload").size()));
    REQUIRE_TRUE(!stats.gpu_used);
    REQUIRE_EQ(read_text_file(output / "nested" / "payload.txt"), "sevenzip payload");
}

// Purpose: Verify 7z extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A 7z member path and a preexisting destination file of the same name.
// Outputs: Throws `SecurityError` and preserves the existing file.
TEST_CASE(sevenzip_extraction_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("sevenzip-overwrite");
    const auto archive = root / "sample.7z";
    write_7z_fixture(archive);
    const auto output = root / "out";
    std::filesystem::create_directories(output / "nested");
    std::ofstream(output / "nested" / "payload.txt") << "old";

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_7z(archive, output, false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "nested" / "payload.txt"), "old");
}

// Purpose: Verify 7z extraction rejects truncated archives before output publication.
// Inputs: A 7z fixture truncated to the signature/header area.
// Outputs: Throws `ArchiveError` and creates no extracted files.
TEST_CASE(sevenzip_extraction_rejects_truncated_archive_before_output) {
    const auto root = test_temp_dir("sevenzip-truncated");
    const auto archive = root / "truncated.7z";
    write_7z_fixture(archive, std::span<const unsigned char>(kNestedSevenZipFixture.data(), 32));

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_7z(archive, root / "out", false));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify 7z extraction rejects corrupted payload bytes through CRC validation.
// Inputs: A valid 7z fixture with one payload byte flipped.
// Outputs: Throws `ArchiveError` and creates no extracted files.
TEST_CASE(sevenzip_extraction_rejects_corrupt_payload_crc_before_output) {
    const auto root = test_temp_dir("sevenzip-corrupt-payload");
    const auto archive = root / "corrupt.7z";
    auto bytes = std::vector<unsigned char>(kNestedSevenZipFixture.begin(), kNestedSevenZipFixture.end());
    bytes[35] ^= 0x01U;
    write_7z_fixture(archive, bytes);

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_7z(archive, root / "out", false));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify hardened 7z FilesInfo singleton and bounded-slice parsing rejects malformed metadata.
// Inputs: Checksum-correct headers with zero-sized/duplicate Name, duplicate WinAttrib, and duplicate MTime records.
// Outputs: Every archive throws a normal error and publishes no output.
TEST_CASE(sevenzip_extraction_rejects_malformed_filesinfo_singletons) {
    const std::vector<std::vector<unsigned char>> next_headers{
        {0x01, 0x05, 0x01, 0x0E, 0x01, 0x80, 0x11, 0x00, 0x00, 0x00},
        {0x01, 0x05, 0x01, 0x0E, 0x01, 0x80, 0x11, 0x05, 0x00, 0x61, 0x00,
         0x00, 0x00, 0x11, 0x05, 0x00, 0x62, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0x01, 0x05, 0x01, 0x0E, 0x01, 0x80, 0x15, 0x06, 0x01, 0x00, 0x20, 0x00,
         0x00, 0x00, 0x15, 0x06, 0x01, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0x01, 0x05, 0x01, 0x0E, 0x01, 0x80, 0x14, 0x0A, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x14, 0x0A, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    };

    for (std::size_t index = 0; index < next_headers.size(); ++index) {
        const auto root = test_temp_dir("sevenzip-filesinfo-singleton-" + std::to_string(index));
        const auto archive = root / "malformed.7z";
        const auto bytes = make_unencoded_7z(next_headers[index]);
        write_7z_fixture(archive, bytes);

        bool rejected = false;
        try {
            static_cast<void>(superzip::extract_7z(archive, root / "out", false));
        } catch (const superzip::Error&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
    }
}

// Purpose: Verify a 7z archive with no Name property receives a defined empty SDK name instead of a null dereference.
// Inputs: One empty-stream entry with no FilesInfo Name record.
// Outputs: SuperZip rejects the empty path through normal path validation and creates no output.
TEST_CASE(sevenzip_extraction_rejects_missing_name_property_without_crash) {
    constexpr std::array<unsigned char, 8> next_header{0x01, 0x05, 0x01, 0x0E, 0x01, 0x80, 0x00, 0x00};
    const auto root = test_temp_dir("sevenzip-missing-name");
    const auto archive = root / "missing-name.7z";
    const auto bytes = make_unencoded_7z(next_header);
    write_7z_fixture(archive, bytes);

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_7z(archive, root / "out", false));
    } catch (const superzip::Error&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}
