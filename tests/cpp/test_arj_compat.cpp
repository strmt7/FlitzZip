#include "test_util.hpp"

#include "arj/arj_adapter.hpp"
#include "core/archive_format.hpp"
#include "core/checksum.hpp"
#include "core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

struct ArjFixtureEntry {
    std::string name;
    std::vector<unsigned char> payload;
    std::uint8_t method = 0;
    std::uint8_t file_type = 0;
    std::optional<std::uint32_t> crc_override;
};

// Purpose: Append one little-endian 16-bit fixture field.
// Inputs: `bytes` is the mutable ARJ fixture and `value` is the unsigned field value.
// Outputs: Appends two bytes.
void append_le16(std::vector<unsigned char>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
}

// Purpose: Append one little-endian 32-bit fixture field.
// Inputs: `bytes` is the mutable ARJ fixture and `value` is the unsigned field value.
// Outputs: Appends four bytes.
void append_le32(std::vector<unsigned char>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 24U) & 0xFFU));
}

// Purpose: Append one NUL-terminated ASCII ARJ fixture string.
// Inputs: `bytes` is the mutable ARJ header and `value` is the raw fixture text.
// Outputs: Appends `value` plus one NUL byte.
void append_c_string(std::vector<unsigned char>& bytes, const std::string& value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
    bytes.push_back(0);
}

// Purpose: Compute CRC-32 over fixture bytes.
// Inputs: `bytes` is the fixture payload.
// Outputs: Returns the finalized CRC-32 value.
std::uint32_t fixture_crc32(std::span<const unsigned char> bytes) {
    return superzip::crc32(std::as_bytes(bytes));
}

// Purpose: Append an ARJ basic header with a zero extended-header terminator.
// Inputs: `archive` is the mutable archive and `header` contains bytes from first-header-size through comment NUL.
// Outputs: Appends header id, size, header bytes, header CRC, and zero extended-header size.
void append_arj_header(std::vector<unsigned char>& archive, const std::vector<unsigned char>& header) {
    archive.push_back(0x60);
    archive.push_back(0xEA);
    append_le16(archive, static_cast<std::uint16_t>(header.size()));
    archive.insert(archive.end(), header.begin(), header.end());
    append_le32(archive, fixture_crc32(header));
    append_le16(archive, 0);
}

// Purpose: Build one ARJ basic header for tests.
// Inputs: `name`, `method`, `file_type`, `compressed_size`, `original_size`, and `crc` are written into ARJ metadata fields.
// Outputs: Returns a complete basic header payload for `append_arj_header`.
std::vector<unsigned char> make_arj_basic_header(
    const std::string& name,
    std::uint8_t method,
    std::uint8_t file_type,
    std::uint32_t compressed_size,
    std::uint32_t original_size,
    std::uint32_t crc) {
    std::vector<unsigned char> header;
    header.reserve(32U + name.size());
    header.push_back(30);
    header.push_back(3);
    header.push_back(1);
    header.push_back(0);
    header.push_back(0);
    header.push_back(method);
    header.push_back(file_type);
    header.push_back(0);
    append_le32(header, 0);
    append_le32(header, compressed_size);
    append_le32(header, original_size);
    append_le32(header, crc);
    append_le16(header, 0);
    append_le16(header, 0x20);
    append_le16(header, 0);
    append_c_string(header, name);
    append_c_string(header, "");
    return header;
}

// Purpose: Write one byte vector to disk for ARJ fixtures.
// Inputs: `path` is the target file and `bytes` is the exact archive content.
// Outputs: Creates or replaces `path`.
void write_binary_file(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Purpose: Write one text file for overwrite and comparison checks.
// Inputs: `path` is the target file and `text` is the payload.
// Outputs: Creates or replaces `path`.
void write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// Purpose: Read one text file for equality assertions.
// Inputs: `path` is the file to read.
// Outputs: Returns the complete file payload as a string.
std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Purpose: Count regular files below a directory for cleanup assertions.
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

// Purpose: Build a minimal ARJ archive with one main header and supplied local entries.
// Inputs: `entries` are exact local-entry fixtures.
// Outputs: Returns ARJ bytes ending in the standard zero-size end marker.
std::vector<unsigned char> make_arj_archive(const std::vector<ArjFixtureEntry>& entries) {
    std::vector<unsigned char> archive;
    append_arj_header(archive, make_arj_basic_header("fixture.arj", 2, 2, 0, 0, 0));
    for (const auto& entry : entries) {
        const auto crc = entry.crc_override.value_or(fixture_crc32(entry.payload));
        const auto size = static_cast<std::uint32_t>(entry.payload.size());
        append_arj_header(archive, make_arj_basic_header(entry.name, entry.method, entry.file_type, size, size, crc));
        archive.insert(archive.end(), entry.payload.begin(), entry.payload.end());
    }
    archive.push_back(0x60);
    archive.push_back(0xEA);
    append_le16(archive, 0);
    return archive;
}

}  // namespace

// Purpose: Verify native ARJ extraction for stored file and directory entries.
// Inputs: A handcrafted ARJ fixture with method-0 regular files and one directory.
// Outputs: Throws if detection, extraction, directory creation, or payload restoration fails.
TEST_CASE(arj_extracts_stored_files_and_directories) {
    const auto root = test_temp_dir("arj-stored");
    const auto archive = root / "sample.arj";
    write_binary_file(
        archive,
        make_arj_archive({
            ArjFixtureEntry{.name = "nested", .file_type = 3},
            ArjFixtureEntry{.name = "nested/hello.txt", .payload = {'h', 'e', 'l', 'l', 'o'}},
            ArjFixtureEntry{.name = "root.txt", .payload = {'r', 'o', 'o', 't'}},
        }));

    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Arj);
    const auto info = superzip::archive_format_info(superzip::ArchiveFormat::Arj);
    REQUIRE_TRUE(info.can_extract);
    REQUIRE_TRUE(!info.can_create);

    const auto output = root / "out";
    const auto stats = superzip::extract_arj(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(3));
    REQUIRE_TRUE(std::filesystem::is_directory(output / "nested"));
    REQUIRE_EQ(read_text_file(output / "nested" / "hello.txt"), std::string("hello"));
    REQUIRE_EQ(read_text_file(output / "root.txt"), std::string("root"));
    std::filesystem::remove_all(root);
}

// Purpose: Verify ARJ extraction refuses existing output files unless overwrite is explicit.
// Inputs: A stored ARJ entry whose target already exists.
// Outputs: Throws if overwrite refusal is bypassed or existing content changes.
TEST_CASE(arj_extract_rejects_overwrite_by_default) {
    const auto root = test_temp_dir("arj-overwrite");
    const auto archive = root / "sample.arj";
    write_binary_file(archive, make_arj_archive({ArjFixtureEntry{.name = "same.txt", .payload = {'n', 'e', 'w'}}}));
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    write_text_file(output / "same.txt", "old");

    bool rejected = false;
    try {
        (void)superzip::extract_arj(archive, output, false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "same.txt"), std::string("old"));
    std::filesystem::remove_all(root);
}

// Purpose: Verify ARJ extraction rejects unsafe embedded paths before publication.
// Inputs: A stored ARJ entry with parent-directory traversal.
// Outputs: Throws if extraction succeeds or writes outside the destination.
TEST_CASE(arj_extract_rejects_unsafe_paths) {
    const auto root = test_temp_dir("arj-unsafe-path");
    const auto archive = root / "unsafe.arj";
    write_binary_file(archive, make_arj_archive({ArjFixtureEntry{.name = "..\\escape.txt", .payload = {'x'}}}));

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_arj(archive, output, true);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify compressed ARJ methods fail explicitly until a vetted decoder exists.
// Inputs: A valid ARJ header using compression method 1.
// Outputs: Throws if unsupported compressed ARJ data is accepted or publishes output.
TEST_CASE(arj_extract_rejects_compressed_methods_without_output) {
    const auto root = test_temp_dir("arj-compressed-method");
    const auto archive = root / "compressed.arj";
    write_binary_file(archive, make_arj_archive({ArjFixtureEntry{.name = "compressed.txt", .payload = {'x'}, .method = 1}}));

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_arj(archive, output, true);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify ARJ CRC mismatches fail before final output publication.
// Inputs: A stored ARJ entry with a deliberately incorrect payload CRC.
// Outputs: Throws if corrupt payload is extracted or leaves final files behind.
TEST_CASE(arj_extract_rejects_payload_crc_mismatch_without_output) {
    const auto root = test_temp_dir("arj-crc-mismatch");
    const auto archive = root / "bad-crc.arj";
    write_binary_file(
        archive,
        make_arj_archive({ArjFixtureEntry{.name = "bad.txt", .payload = {'b', 'a', 'd'}, .crc_override = 0x12345678U}}));

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_arj(archive, output, true);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}
