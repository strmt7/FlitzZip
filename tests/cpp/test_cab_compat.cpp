#include "test_compat_fixture.hpp"
#include "test_util.hpp"

#include "cab/cab_adapter.hpp"
#include "cab/cab_format.hpp"
#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "core/path_text.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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
// Inputs: `archive` is the output path, `entries` are member paths/payloads, and `flags` can inject malformed
// spanned-CAB metadata. Outputs: Creates a CAB file that Windows FDI can extract when metadata is valid.
void write_uncompressed_cab(const std::filesystem::path& archive, const std::vector<CabFixtureEntry>& entries,
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

// Purpose: Preserve a missing cabinet's Unicode filename in the archive error contract.
// Inputs: A nonexistent CAB beneath a test-owned path containing supplementary Unicode characters.
// Outputs: Requires ArchiveError with exact UTF-8 path text and no created source file.
TEST_CASE(cab_missing_unicode_source_has_actionable_error) {
    const auto root = test_temp_dir("cab-unicode-missing");
    const auto archive = root / std::filesystem::path(u"missing-\U0001F9EA.cab");
    bool rejected = false;
    try {
        (void)flitzzip::scan_cab_metadata(archive);
    } catch (const flitzzip::ArchiveError& error) {
        REQUIRE_EQ(std::string(error.what()),
                   "cannot read CAB archive size: " + flitzzip::path_diagnostic_utf8(archive));
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(archive));
}

// Purpose: Verify native `.cab` extraction reads uncompressed CAB payloads through Windows FDI.
// Inputs: A handcrafted single-folder CAB with one nested file.
// Outputs: Throws if detection, scanning, or extraction regresses.
TEST_CASE(cab_extraction_reads_uncompressed_payload) {
    const auto root = test_temp_dir("cab-basic");
    const auto archive = root / "sample.cab";
    write_uncompressed_cab(archive, {{.name = "input\\alpha.txt", .payload = "cab payload\n"}});

    REQUIRE_EQ(flitzzip::detect_archive_format(archive), flitzzip::ArchiveFormat::Cab);
    const auto info = flitzzip::archive_format_info(flitzzip::ArchiveFormat::Cab);
    REQUIRE_TRUE(!info.can_create);
    REQUIRE_TRUE(info.can_extract);
    REQUIRE_TRUE(info.bundled_native);
    const auto metadata = flitzzip::scan_cab_metadata(archive);
    REQUIRE_EQ(metadata.entries.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(metadata.entries.front().path, "input/alpha.txt");

    const auto output = root / "out";
    const auto stats = flitzzip::extract_cab(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(stats.output_bytes, static_cast<std::uint64_t>(std::string_view("cab payload\n").size()));
    REQUIRE_EQ(read_text_file(output / "input" / "alpha.txt"), "cab payload\n");
    flitzzip_test::export_compat_fixture(archive, output);
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
        static_cast<void>(flitzzip::extract_cab(archive, root / "out", false));
    } catch (const flitzzip::SecurityError&) {
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
        static_cast<void>(flitzzip::extract_cab(archive, output, false));
    } catch (const flitzzip::SecurityError&) {
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
        static_cast<void>(flitzzip::scan_cab_metadata(archive));
    } catch (const flitzzip::ArchiveError&) {
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
        static_cast<void>(flitzzip::scan_cab_metadata(archive));
    } catch (const flitzzip::ArchiveError&) {
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
    write_uncompressed_cab(archive, {
                                        {.name = "dir\\alpha.txt", .payload = "one"},
                                        {.name = "dir/alpha.txt", .payload = "two"},
                                    });

    bool rejected = false;
    try {
        static_cast<void>(flitzzip::scan_cab_metadata(archive));
    } catch (const flitzzip::SecurityError&) {
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
    write_uncompressed_cab(archive, {
                                        {.name = "dir", .payload = "file"},
                                        {.name = "dir\\child.txt", .payload = "child"},
                                    });

    bool rejected = false;
    try {
        static_cast<void>(flitzzip::scan_cab_metadata(archive));
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

namespace {

// Purpose: Change an independently framed CAB field without a production serializer.
// Inputs: Small fixture, field offset/width and unsigned value. Outputs: Exact little-endian bytes.
void set_cab_field(std::vector<unsigned char>& bytes, std::size_t offset, std::size_t width, std::uint32_t value) {
    REQUIRE_TRUE(width > 0U && width <= 4U && offset + width <= bytes.size());
    for (std::size_t index = 0; index < width; ++index) {
        bytes[offset + index] = static_cast<unsigned char>(value >> (8U * index));
    }
}

// Purpose: Verify metadata rejection precedes FDI progress and all destination mutation.
// Inputs: Malformed CAB, owned existing destination and expected diagnostic fragment.
// Outputs: Requires the specific error, unchanged bytes and no temporary directory leakage.
void require_cab_admission_rejected(const std::filesystem::path& archive, const std::filesystem::path& destination,
                                    std::string_view diagnostic) {
    std::filesystem::create_directories(destination);
    const auto target = destination / "file.txt";
    {
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << "retain";
    }
    unsigned callbacks = 0;
    bool rejected = false;
    try {
        (void)flitzzip::extract_cab(archive, destination, true,
                                    [&](const flitzzip::ProgressSnapshot&) { ++callbacks; });
    } catch (const flitzzip::ArchiveError& error) {
        REQUIRE_TRUE(std::string_view(error.what()).find(diagnostic) != std::string_view::npos);
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(callbacks, 0U);
    REQUIRE_EQ(read_text_file(target), "retain");
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator()),
               1);
}

}  // namespace

// Purpose: Exercise each binary CAB admission boundary independently of Windows decompression.
// Inputs: Known-good stored fixture and hostile sizes, offsets, versions, counts and folder references.
// Outputs: Each mutation rejects with the intended diagnostic before progress; the original still extracts.
TEST_CASE(cab_binary_metadata_failure_matrix_preserves_destination) {
    const auto root = test_temp_dir("cab-binary-metadata-matrix");
    const auto archive = root / "matrix.cab";
    const auto output = root / "out";
    write_uncompressed_cab(archive, {{"file.txt", "cab payload"}});
    const auto valid = read_binary_file(archive);
    const auto size = static_cast<std::uint32_t>(valid.size());
    struct Mutation {
        std::size_t offset;
        std::size_t width;
        std::uint32_t value;
        const char* diagnostic;
    };
    const Mutation mutations[] = {
        {0, 1, 0, "magic"},
        {24, 1, 2, "version"},
        {25, 1, 2, "version"},
        {8, 4, 35, "declared size"},
        {8, 4, size + 1U, "declared size"},
        {26, 2, 4097, "folder count"},
        {26, 2, 0, "no folder table"},
        {26, 2, 32, "folder table extends"},
        {36, 4, size, "folder data offset"},
        {40, 2, 0, "no data blocks"},
        {42, 2, 4, "compression type"},
        {16, 4, 35, "file table offset"},
        {16, 4, size, "file table offset"},
        {52, 2, 1, "invalid folder index"},
        {52, 2, 0xFFFDU, "spans cabinet"},
        {52, 2, 0xFFFEU, "spans cabinet"},
        {52, 2, 0xFFFFU, "spans cabinet"},
        {48, 4, 0xFFFFFFFFU, "folder offset overflows"},
        {60, 1, 0, "filename is empty"},
    };
    for (const auto& mutation : mutations) {
        auto changed = valid;
        set_cab_field(changed, mutation.offset, mutation.width, mutation.value);
        write_binary_file(archive, changed);
        require_cab_admission_rejected(archive, output, mutation.diagnostic);
    }
    write_binary_file(archive, valid);
    const auto stats = flitzzip::extract_cab(archive, output, true);
    REQUIRE_EQ(stats.entries, 1U);
    REQUIRE_EQ(stats.output_bytes, 11U);
    REQUIRE_EQ(read_text_file(output / "file.txt"), "cab payload");
}

// Purpose: Reject every strict prefix of a complete CAB before invoking extraction callbacks.
// Inputs: A small independently framed cabinet, including its header, table and data block.
// Outputs: No prefix is accepted or changes an existing file; complete bytes retain exact readback.
TEST_CASE(cab_all_truncation_boundaries_preserve_destination) {
    const auto root = test_temp_dir("cab-all-truncations");
    const auto archive = root / "truncated.cab";
    const auto output = root / "out";
    write_uncompressed_cab(archive, {{"file.txt", "cab payload"}});
    const auto valid = read_binary_file(archive);
    for (std::size_t length = 0; length < valid.size(); ++length) {
        const std::vector<unsigned char> prefix(valid.begin(), valid.begin() + length);
        write_binary_file(archive, prefix);
        require_cab_admission_rejected(archive, output, length < 36U ? "too small" : "declared size");
    }
    write_binary_file(archive, valid);
    (void)flitzzip::extract_cab(archive, output, true);
    REQUIRE_EQ(read_text_file(output / "file.txt"), "cab payload");
}

// Purpose: Exercise optional CAB header, folder and CFDATA reserve fields through the actual FDI consumer.
// Inputs: A valid stored cabinet with distinct opaque bytes inserted into all three reserve regions.
// Outputs: Exact payload readback and rejection when the header reserve crosses the declared cabinet bound.
TEST_CASE(cab_reserved_regions_are_bounded_and_extractable) {
    const auto root = test_temp_dir("cab-reserved-regions");
    const auto archive = root / "reserved.cab";
    const std::vector<CabFixtureEntry> entries{{"file.txt", "cab payload"}};
    write_uncompressed_cab(archive, entries);
    auto bytes = read_binary_file(archive);
    const auto original_data = cab_data_header_offset(entries);
    bytes.insert(bytes.begin() + original_data + 8U, {0xA1U, 0xA2U});
    bytes.insert(bytes.begin() + 44U, {0xB1U, 0xB2U, 0xB3U});
    bytes.insert(bytes.begin() + 36U, {2U, 0U, 3U, 2U, 0xC1U, 0xC2U});
    set_cab_field(bytes, 8, 4, static_cast<std::uint32_t>(bytes.size()));
    set_cab_field(bytes, 16, 4, 53U);
    set_cab_field(bytes, 30, 2, 4U);
    set_cab_field(bytes, 42, 4, static_cast<std::uint32_t>(original_data + 9U));
    write_binary_file(archive, bytes);
    const auto metadata = flitzzip::scan_cab_metadata(archive);
    REQUIRE_EQ(metadata.entries.size(), 1U);
    const auto stats = flitzzip::extract_cab(archive, root / "out", false);
    REQUIRE_EQ(stats.output_bytes, 11U);
    REQUIRE_EQ(read_text_file(root / "out" / "file.txt"), "cab payload");

    set_cab_field(bytes, 36, 2, 0xFFFFU);
    write_binary_file(archive, bytes);
    require_cab_admission_rejected(archive, root / "out", "header reserve extends");
}

// Purpose: Keep a validated CAB source immutable while the Windows decoder reopens it for extraction.
// Inputs: An owned valid cabinet and a callback that requests real Windows write access during extraction.
// Outputs: Write admission fails with a sharing violation until extraction finishes, then succeeds after cleanup.
TEST_CASE(cab_source_remains_pinned_through_extraction) {
    const auto root = test_temp_dir("cab-source-lifetime");
    const auto archive = root / "source.cab";
    write_uncompressed_cab(archive, {{"file.txt", "cab payload"}});
    const auto original = read_binary_file(archive);
    bool observed = false;
    bool writable = false;
    DWORD admission_error = ERROR_SUCCESS;
    const auto callback = [&](const flitzzip::ProgressSnapshot&) {
        if (observed) {
            return;
        }
        observed = true;
        const HANDLE writer =
            CreateFileW(archive.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        admission_error = writer == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        writable = writer != INVALID_HANDLE_VALUE;
        if (writable) {
            CloseHandle(writer);
        }
    };
    const auto stats = flitzzip::extract_cab(archive, root / "out", false, callback);
    REQUIRE_TRUE(observed);
    REQUIRE_TRUE(!writable);
    REQUIRE_EQ(admission_error, static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    REQUIRE_EQ(stats.output_bytes, 11U);
    REQUIRE_EQ(read_binary_file(archive), original);
    REQUIRE_EQ(read_text_file(root / "out" / "file.txt"), "cab payload");
    const HANDLE released =
        CreateFileW(archive.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE_TRUE(released != INVALID_HANDLE_VALUE);
    REQUIRE_TRUE(CloseHandle(released) != FALSE);
}

// Purpose: Reject an actively writable CAB before metadata validation or destination creation.
// Inputs: A valid cabinet with an independently held Windows write handle, then the same cabinet after release.
// Outputs: Security rejection without callbacks or output, followed by successful exact extraction.
TEST_CASE(cab_active_writer_is_rejected_before_output_and_recovers) {
    const auto root = test_temp_dir("cab-active-writer");
    const auto archive = root / "source.cab";
    const auto output = root / "out";
    write_uncompressed_cab(archive, {{"file.txt", "cab payload"}});
    const HANDLE writer =
        CreateFileW(archive.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE_TRUE(writer != INVALID_HANDLE_VALUE);
    std::unique_ptr<void, decltype(&CloseHandle)> writer_owner(writer, &CloseHandle);
    bool rejected = false;
    unsigned callbacks = 0;
    try {
        (void)flitzzip::extract_cab(archive, output, false, [&](const flitzzip::ProgressSnapshot&) { ++callbacks; });
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(callbacks, 0U);
    REQUIRE_TRUE(!std::filesystem::exists(output));
    writer_owner.reset();
    const auto stats = flitzzip::extract_cab(archive, output, false);
    REQUIRE_EQ(stats.output_bytes, 11U);
    REQUIRE_EQ(read_text_file(output / "file.txt"), "cab payload");
}
