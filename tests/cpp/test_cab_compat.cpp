#include "test_util.hpp"

#include "cab/cab_adapter.hpp"
#include "cab/cab_format.hpp"
#include "core/archive_format.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint16_t kCabFlagNextCabinet = 0x0002U;
constexpr std::size_t kCabHeaderBytes = 36U;
constexpr std::size_t kCabFolderBytes = 8U;
constexpr std::size_t kCabFileRecordBytes = 16U;
constexpr std::size_t kCabDataHeaderBytes = 8U;

struct CabFixtureEntry {
    std::string name;
    std::string payload;
};

// Purpose: Read a full text file for CAB extraction equality checks.
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

// Purpose: Append a little-endian 16-bit CAB fixture field.
// Inputs: `bytes` receives encoded output and `value` is the decoded integer.
// Outputs: Appends two bytes.
void append_le16(std::vector<unsigned char>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
}

// Purpose: Append a little-endian 32-bit CAB fixture field.
// Inputs: `bytes` receives encoded output and `value` is the decoded integer.
// Outputs: Appends four bytes.
void append_le32(std::vector<unsigned char>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 24U) & 0xFFU));
}

// Purpose: Append one NUL-terminated ASCII CAB filename.
// Inputs: `bytes` receives encoded output and `name` is the raw CAB member path.
// Outputs: Appends the filename and terminating NUL byte.
void append_cab_name(std::vector<unsigned char>& bytes, std::string_view name) {
    bytes.insert(bytes.end(), name.begin(), name.end());
    bytes.push_back(0U);
}

// Purpose: Return the CFDATA header offset for a fixture with one folder.
// Inputs: `entries` are the exact entries written by `write_uncompressed_cab`.
// Outputs: Returns the absolute byte offset of the first data block header.
std::size_t cab_data_header_offset(const std::vector<CabFixtureEntry>& entries) {
    std::size_t offset = kCabHeaderBytes + kCabFolderBytes;
    for (const auto& entry : entries) {
        offset += kCabFileRecordBytes + entry.name.size() + 1U;
    }
    return offset;
}

// Purpose: Read a complete binary file into memory for fixture mutation.
// Inputs: `path` is the file to load.
// Outputs: Returns all bytes.
std::vector<unsigned char> read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Purpose: Write a complete binary file from memory.
// Inputs: `path` is the output file and `bytes` are the payload.
// Outputs: Replaces the file contents.
void write_binary_file(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Purpose: Build a minimal uncompressed single-folder CAB fixture.
// Inputs: `archive` is the output path, `entries` are member paths/payloads, and `flags` can inject malformed spanned-CAB metadata.
// Outputs: Creates a CAB file that Windows FDI can extract when metadata is valid.
void write_uncompressed_cab(
    const std::filesystem::path& archive,
    const std::vector<CabFixtureEntry>& entries,
    std::uint16_t flags = 0U) {
    REQUIRE_TRUE(entries.size() <= 0xFFFFU);
    std::uint64_t payload_size = 0;
    std::uint64_t file_table_size = 0;
    for (const auto& entry : entries) {
        REQUIRE_TRUE(!entry.name.empty());
        REQUIRE_TRUE(entry.name.find('\0') == std::string::npos);
        REQUIRE_TRUE(entry.payload.size() <= 0xFFFFFFFFULL);
        REQUIRE_TRUE(payload_size + entry.payload.size() <= 0xFFFFULL);
        payload_size += entry.payload.size();
        file_table_size += kCabFileRecordBytes + entry.name.size() + 1U;
    }
    REQUIRE_TRUE(payload_size <= 0xFFFFULL);
    REQUIRE_TRUE(file_table_size <= 0xFFFFFFFFULL);

    const auto file_table_offset = static_cast<std::uint32_t>(kCabHeaderBytes + kCabFolderBytes);
    const auto data_offset = static_cast<std::uint32_t>(file_table_offset + file_table_size);
    const auto cabinet_size = static_cast<std::uint32_t>(data_offset + kCabDataHeaderBytes + payload_size);

    std::vector<unsigned char> bytes;
    bytes.reserve(cabinet_size);
    bytes.insert(bytes.end(), {'M', 'S', 'C', 'F'});
    append_le32(bytes, 0U);
    append_le32(bytes, cabinet_size);
    append_le32(bytes, 0U);
    append_le32(bytes, file_table_offset);
    append_le32(bytes, 0U);
    bytes.push_back(3U);
    bytes.push_back(1U);
    append_le16(bytes, 1U);
    append_le16(bytes, static_cast<std::uint16_t>(entries.size()));
    append_le16(bytes, flags);
    append_le16(bytes, 0x1234U);
    append_le16(bytes, 0U);

    append_le32(bytes, data_offset);
    append_le16(bytes, 1U);
    append_le16(bytes, 0U);

    std::uint32_t folder_offset = 0;
    for (const auto& entry : entries) {
        append_le32(bytes, static_cast<std::uint32_t>(entry.payload.size()));
        append_le32(bytes, folder_offset);
        append_le16(bytes, 0U);
        append_le16(bytes, 0U);
        append_le16(bytes, 0U);
        append_le16(bytes, 0x20U);
        append_cab_name(bytes, entry.name);
        folder_offset += static_cast<std::uint32_t>(entry.payload.size());
    }

    append_le32(bytes, 0U);
    append_le16(bytes, static_cast<std::uint16_t>(payload_size));
    append_le16(bytes, static_cast<std::uint16_t>(payload_size));
    for (const auto& entry : entries) {
        bytes.insert(bytes.end(), entry.payload.begin(), entry.payload.end());
    }
    REQUIRE_EQ(bytes.size(), static_cast<std::size_t>(cabinet_size));

    std::ofstream output(archive, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

// Purpose: Verify native `.cab` extraction reads uncompressed CAB payloads through Windows FDI.
// Inputs: A handcrafted single-folder CAB with one nested file.
// Outputs: Throws if detection, scanning, or extraction regresses.
TEST_CASE(cab_extraction_reads_uncompressed_payload) {
    const auto root = test_temp_dir("cab-basic");
    const auto archive = root / "sample.cab";
    write_uncompressed_cab(archive, {{.name = "input\\alpha.txt", .payload = "cab payload\n"}});

    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Cab);
    const auto info = superzip::archive_format_info(superzip::ArchiveFormat::Cab);
    REQUIRE_TRUE(!info.can_create);
    REQUIRE_TRUE(info.can_extract);
    REQUIRE_TRUE(info.bundled_native);
    const auto metadata = superzip::scan_cab_metadata(archive);
    REQUIRE_EQ(metadata.entries.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(metadata.entries.front().path, "input/alpha.txt");

    const auto output = root / "out";
    const auto stats = superzip::extract_cab(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(stats.output_bytes, static_cast<std::uint64_t>(std::string_view("cab payload\n").size()));
    REQUIRE_EQ(read_text_file(output / "input" / "alpha.txt"), "cab payload\n");
}

// Purpose: Verify CAB extraction rejects traversal paths before output.
// Inputs: A CAB whose member name attempts to escape the extraction root.
// Outputs: Throws `SecurityError` and creates no output files.
TEST_CASE(cab_extraction_rejects_traversal_before_output) {
    const auto root = test_temp_dir("cab-traversal");
    const auto archive = root / "bad.cab";
    write_uncompressed_cab(archive, {{.name = "..\\escape.txt", .payload = "bad"}});

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_cab(archive, root / "out", false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify CAB extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A CAB member path and a preexisting destination file of the same name.
// Outputs: Throws `SecurityError` and preserves the existing file.
TEST_CASE(cab_extraction_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("cab-overwrite");
    const auto archive = root / "overwrite.cab";
    write_uncompressed_cab(archive, {{.name = "file.txt", .payload = "new"}});
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    std::ofstream(output / "file.txt") << "old";

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_cab(archive, output, false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "file.txt"), "old");
}

// Purpose: Verify CAB metadata scanning rejects spanned cabinet sets.
// Inputs: A CAB header with the next-cabinet flag set.
// Outputs: Throws `ArchiveError` before FDI is invoked.
TEST_CASE(cab_scanner_rejects_spanned_cabinet_sets) {
    const auto root = test_temp_dir("cab-spanned");
    const auto archive = root / "spanned.cab";
    write_uncompressed_cab(archive, {{.name = "file.txt", .payload = "payload"}}, kCabFlagNextCabinet);

    bool rejected = false;
    try {
        static_cast<void>(superzip::scan_cab_metadata(archive));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify CAB metadata scanning rejects data blocks whose declared payload extends past the cabinet.
// Inputs: A valid CAB fixture mutated to overstate its first CFDATA compressed size.
// Outputs: Throws `ArchiveError` before FDI is invoked.
TEST_CASE(cab_scanner_rejects_data_block_extent_past_cabinet_boundary) {
    const auto root = test_temp_dir("cab-data-block-boundary");
    const auto archive = root / "bad.cab";
    const std::vector<CabFixtureEntry> entries{{.name = "file.txt", .payload = "tiny"}};
    write_uncompressed_cab(archive, entries);

    auto bytes = read_binary_file(archive);
    const auto compressed_size_offset = cab_data_header_offset(entries) + 4U;
    REQUIRE_TRUE(compressed_size_offset + 1U < bytes.size());
    bytes[compressed_size_offset] = 0xFFU;
    bytes[compressed_size_offset + 1U] = 0x7FU;
    write_binary_file(archive, bytes);

    bool rejected = false;
    try {
        static_cast<void>(superzip::scan_cab_metadata(archive));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify CAB metadata scanning rejects duplicate paths after normalization.
// Inputs: Two CAB names that differ only by slash style.
// Outputs: Throws `SecurityError` before extraction creates output.
TEST_CASE(cab_scanner_rejects_duplicate_normalized_paths) {
    const auto root = test_temp_dir("cab-duplicate-path");
    const auto archive = root / "duplicate.cab";
    write_uncompressed_cab(
        archive,
        {
            {.name = "dir\\alpha.txt", .payload = "one"},
            {.name = "dir/alpha.txt", .payload = "two"},
        });

    bool rejected = false;
    try {
        static_cast<void>(superzip::scan_cab_metadata(archive));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify CAB metadata scanning rejects file/child conflicts before output.
// Inputs: One CAB member named as a file and another member below that path.
// Outputs: Throws `SecurityError` during metadata validation.
TEST_CASE(cab_scanner_rejects_file_child_path_conflicts) {
    const auto root = test_temp_dir("cab-file-child-conflict");
    const auto archive = root / "conflict.cab";
    write_uncompressed_cab(
        archive,
        {
            {.name = "dir", .payload = "file"},
            {.name = "dir\\child.txt", .payload = "child"},
        });

    bool rejected = false;
    try {
        static_cast<void>(superzip::scan_cab_metadata(archive));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}
