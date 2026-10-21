#include "test_compat_fixture.hpp"
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

// Purpose: Replace one little-endian 32-bit fixture field without using production parsing code.
// Inputs: `bytes` contains the field at `offset`; `value` is the exact wire value.
// Outputs: Replaces four existing bytes.
void replace_le32(std::vector<unsigned char>& bytes, std::size_t offset, std::uint32_t value) {
    bytes.at(offset) = static_cast<unsigned char>(value & 0xFFU);
    bytes.at(offset + 1U) = static_cast<unsigned char>((value >> 8U) & 0xFFU);
    bytes.at(offset + 2U) = static_cast<unsigned char>((value >> 16U) & 0xFFU);
    bytes.at(offset + 3U) = static_cast<unsigned char>((value >> 24U) & 0xFFU);
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
    return flitzzip::crc32(std::as_bytes(bytes));
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

// Purpose: Append one independently checksummed ARJ extended header after a basic header.
// Inputs: `archive` ends with the basic header's zero extension marker; `extension` is exact test metadata.
// Outputs: Replaces that marker with one extension and a new terminator.
void append_arj_extension(std::vector<unsigned char>& archive, const std::vector<unsigned char>& extension,
                          std::optional<std::uint32_t> crc_override = std::nullopt) {
    archive.resize(archive.size() - 2U);
    append_le16(archive, static_cast<std::uint16_t>(extension.size()));
    archive.insert(archive.end(), extension.begin(), extension.end());
    append_le32(archive, crc_override.value_or(fixture_crc32(extension)));
    append_le16(archive, 0U);
}

// Purpose: Build one ARJ basic header for tests.
// Inputs: `name`, `method`, `file_type`, `compressed_size`, `original_size`, and `crc` are written into ARJ metadata
// fields. Outputs: Returns a complete basic header payload for `append_arj_header`.
std::vector<unsigned char> make_arj_basic_header(const std::string& name, std::uint8_t method, std::uint8_t file_type,
                                                 std::uint32_t compressed_size, std::uint32_t original_size,
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

// Purpose: Build an ARJ archive around one caller-controlled local header.
// Inputs: `header` is exact local metadata and `payload` is appended without reconciling declared sizes.
// Outputs: Returns a standard main header, the supplied record, and an end marker.
std::vector<unsigned char> make_arj_archive_with_local_header(const std::vector<unsigned char>& header,
                                                              std::span<const unsigned char> payload = {}) {
    std::vector<unsigned char> archive;
    append_arj_header(archive, make_arj_basic_header("fixture.arj", 2, 2, 0, 0, 0));
    append_arj_header(archive, header);
    archive.insert(archive.end(), payload.begin(), payload.end());
    archive.push_back(0x60U);
    archive.push_back(0xEAU);
    append_le16(archive, 0U);
    return archive;
}

// Purpose: Require malformed ARJ bytes to fail before changing an existing destination.
// Inputs: Exact fixture bytes, isolated directory and expected stable diagnostic.
// Outputs: Preserves the sentinel as the only output file and requires the exact error.
void require_arj_archive_rejected(const std::vector<unsigned char>& bytes, const std::filesystem::path& root,
                                  std::string_view expected) {
    std::filesystem::create_directories(root);
    const auto archive = root / "input.arj";
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    write_binary_file(archive, bytes);
    write_text_file(output / "sentinel.txt", "retain");
    std::string diagnostic;
    try {
        (void)flitzzip::extract_arj(archive, output, true);
    } catch (const flitzzip::Error& error) {
        diagnostic = error.what();
    }
    REQUIRE_EQ(diagnostic, expected);
    REQUIRE_EQ(read_text_file(output / "sentinel.txt"), std::string("retain"));
    REQUIRE_EQ(count_regular_files(output), 1U);
}

}  // namespace

// Purpose: Verify native ARJ extraction for stored file and directory entries.
// Inputs: A handcrafted ARJ fixture with method-0 regular files and one directory.
// Outputs: Throws if detection, extraction, directory creation, or payload restoration fails.
TEST_CASE(arj_extracts_stored_files_and_directories) {
    const auto root = test_temp_dir("arj-stored");
    const auto archive = root / "sample.arj";
    write_binary_file(archive, make_arj_archive({
                                   ArjFixtureEntry{.name = "nested", .file_type = 3},
                                   ArjFixtureEntry{.name = "nested/hello.txt", .payload = {'h', 'e', 'l', 'l', 'o'}},
                                   ArjFixtureEntry{.name = "root.txt", .payload = {'r', 'o', 'o', 't'}},
                               }));

    REQUIRE_EQ(flitzzip::detect_archive_format(archive), flitzzip::ArchiveFormat::Arj);
    const auto info = flitzzip::archive_format_info(flitzzip::ArchiveFormat::Arj);
    REQUIRE_TRUE(info.can_extract);
    REQUIRE_TRUE(!info.can_create);

    const auto output = root / "out";
    const auto stats = flitzzip::extract_arj(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(3));
    REQUIRE_TRUE(std::filesystem::is_directory(output / "nested"));
    REQUIRE_EQ(read_text_file(output / "nested" / "hello.txt"), std::string("hello"));
    REQUIRE_EQ(read_text_file(output / "root.txt"), std::string("root"));
    flitzzip_test::export_compat_fixture(archive, output);
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
        (void)flitzzip::extract_arj(archive, output, false);
    } catch (const flitzzip::SecurityError&) {
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
        (void)flitzzip::extract_arj(archive, output, true);
    } catch (const flitzzip::SecurityError&) {
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
    write_binary_file(archive,
                      make_arj_archive({ArjFixtureEntry{.name = "compressed.txt", .payload = {'x'}, .method = 1}}));

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_arj(archive, output, true);
    } catch (const flitzzip::ArchiveError&) {
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
    write_binary_file(archive, make_arj_archive({ArjFixtureEntry{
                                   .name = "bad.txt", .payload = {'b', 'a', 'd'}, .crc_override = 0x12345678U}}));

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_arj(archive, output, true);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Keep host filesystem failures inside the documented FlitzZip adapter error contract.
// Inputs: A missing archive path and an otherwise valid empty destination root.
// Outputs: Requires ArchiveError with actionable path context rather than a leaking filesystem exception type.
TEST_CASE(arj_missing_archive_reports_product_error) {
    const auto root = test_temp_dir("arj-missing-archive");
    const auto archive = root / "missing.arj";
    bool rejected = false;
    try {
        (void)flitzzip::extract_arj(archive, root / "out", false);
    } catch (const flitzzip::ArchiveError& error) {
        REQUIRE_TRUE(std::string_view(error.what()).find("cannot inspect ARJ archive") != std::string_view::npos);
        REQUIRE_TRUE(std::string_view(error.what()).find("missing.arj") != std::string_view::npos);
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "out"));
    std::filesystem::remove_all(root);
}

// Purpose: Exercise strict ARJ basic-header framing before any entry or output admission.
// Inputs: Independent prefixes, sizes, CRCs and filename-string boundary mutations.
// Outputs: Requires precise rejection and preservation of the destination for every malformed header.
TEST_CASE(arj_basic_header_failure_matrix_preserves_destination) {
    const auto root = test_temp_dir("arj-basic-header-failures");
    std::vector<std::pair<std::vector<unsigned char>, std::string>> cases;
    cases.push_back({{0x60U, 0xEAU, 0x01U}, "ARJ header extends past the end of the archive"});
    cases.push_back({{0x00U, 0x00U, 0x00U, 0x00U}, "malformed ARJ header id"});
    cases.push_back({{0x60U, 0xEAU, 0x29U, 0x0AU}, "ARJ basic header exceeds format limit"});
    cases.push_back({{0x60U, 0xEAU, 30U, 0U}, "ARJ header extends past the end of the archive"});

    auto bad_crc = make_arj_archive({});
    bad_crc[4] ^= 1U;
    cases.push_back({std::move(bad_crc), "ARJ basic header CRC mismatch"});

    std::vector<unsigned char> malformed;
    append_arj_header(malformed, {0U});
    cases.push_back({std::move(malformed), "ARJ basic header is malformed"});

    std::vector<unsigned char> missing_string(30U, 0U);
    missing_string[0] = 30U;
    malformed.clear();
    append_arj_header(malformed, missing_string);
    cases.push_back({std::move(malformed), "ARJ header string is missing"});

    std::vector<unsigned char> unterminated(31U, 0U);
    unterminated[0] = 30U;
    unterminated[30] = 'x';
    malformed.clear();
    append_arj_header(malformed, unterminated);
    cases.push_back({std::move(malformed), "ARJ header string is not NUL terminated"});

    for (std::size_t index = 0; index < cases.size(); ++index) {
        require_arj_archive_rejected(cases[index].first, root / std::to_string(index), cases[index].second);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Enforce unsupported ARJ flags, entry types and declared-size relationships at scan time.
// Inputs: Independently altered local/main headers plus one deliberately out-of-range payload extent.
// Outputs: Every invalid policy fails before publication with a stable cause.
TEST_CASE(arj_metadata_policy_failure_matrix_preserves_destination) {
    const auto root = test_temp_dir("arj-policy-failures");
    std::vector<std::pair<std::vector<unsigned char>, std::string>> cases;
    for (const auto [flag, message] : std::array<std::pair<std::uint8_t, std::string_view>, 3>{{
             {0x01U, "encrypted ARJ entries are not supported"},
             {0x04U, "multi-volume ARJ entries are not supported"},
             {0x08U, "split ARJ file-position metadata is not supported"},
         }}) {
        auto header = make_arj_basic_header("entry", 0U, 0U, 0U, 0U, 0U);
        header[4] = flag;
        cases.push_back({make_arj_archive_with_local_header(header), std::string(message)});
    }
    auto header = make_arj_basic_header("entry", 0U, 4U, 0U, 0U, 0U);
    cases.push_back({make_arj_archive_with_local_header(header), "unsupported ARJ entry type"});

    header = make_arj_basic_header("entry", 0U, 0U, 1U, 2U, 0U);
    cases.push_back({make_arj_archive_with_local_header(header, std::array<unsigned char, 1>{'x'}),
                     "stored ARJ entry has mismatched compressed and original sizes"});

    header = make_arj_basic_header("directory", 0U, 3U, 1U, 1U, 0U);
    cases.push_back({make_arj_archive_with_local_header(header, std::array<unsigned char, 1>{'x'}),
                     "ARJ directory entry unexpectedly contains payload bytes"});

    header = make_arj_basic_header("entry", 0U, 0U, 32U, 32U, 0U);
    auto past_end = make_arj_archive_with_local_header(header, std::array<unsigned char, 1>{'x'});
    past_end.resize(past_end.size() - 4U);
    cases.push_back({std::move(past_end), "ARJ payload extends past the end of the archive"});

    std::vector<unsigned char> main_end{0x60U, 0xEAU, 0U, 0U};
    cases.push_back({std::move(main_end), "ARJ archive is missing the main header"});
    std::vector<unsigned char> wrong_main;
    append_arj_header(wrong_main, make_arj_basic_header("entry", 0U, 0U, 0U, 0U, 0U));
    cases.push_back({std::move(wrong_main), "ARJ archive is missing the main header"});
    auto volume_main_header = make_arj_basic_header("fixture.arj", 2U, 2U, 0U, 0U, 0U);
    volume_main_header[4] = 0x04U;
    std::vector<unsigned char> volume_main;
    append_arj_header(volume_main, volume_main_header);
    cases.push_back({std::move(volume_main), "multi-volume ARJ archives are not supported"});

    for (std::size_t index = 0; index < cases.size(); ++index) {
        require_arj_archive_rejected(cases[index].first, root / std::to_string(index), cases[index].second);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Validate ARJ extended-header CRC handling and scan-to-publication payload revalidation.
// Inputs: One valid extension, one bad extension CRC and a callback mutation after the complete scan.
// Outputs: Extracts valid metadata, rejects corrupt metadata, and removes a payload changed before publication.
TEST_CASE(arj_extended_headers_and_second_pass_integrity_are_enforced) {
    const auto root = test_temp_dir("arj-extended-and-revalidation");
    const std::vector<unsigned char> extension{'m', 'e', 't', 'a'};
    const std::vector<unsigned char> payload{'u', 'n', 'i', 'q', 'u', 'e', '-', 'p', 'a', 'y', 'l', 'o', 'a', 'd'};

    std::vector<unsigned char> valid;
    const auto main_header = make_arj_basic_header("fixture.arj", 2U, 2U, 0U, 0U, 0U);
    append_arj_header(valid, main_header);
    append_arj_extension(valid, extension);
    const auto payload_header =
        make_arj_basic_header("payload.bin", 0U, 0U, static_cast<std::uint32_t>(payload.size()),
                              static_cast<std::uint32_t>(payload.size()), fixture_crc32(payload));
    append_arj_header(valid, payload_header);
    valid.insert(valid.end(), payload.begin(), payload.end());
    valid.insert(valid.end(), {0x60U, 0xEAU, 0U, 0U});
    const auto archive = root / "valid.arj";
    write_binary_file(archive, valid);
    const auto valid_output = root / "valid-output";
    (void)flitzzip::extract_arj(archive, valid_output, false);
    REQUIRE_EQ(read_text_file(valid_output / "payload.bin"), std::string(payload.begin(), payload.end()));

    auto bad_extension = valid;
    bad_extension[4U + main_header.size() + 4U + 2U] ^= 1U;
    require_arj_archive_rejected(bad_extension, root / "bad-extension", "ARJ extended header CRC mismatch");

    write_binary_file(archive, valid);
    const auto payload_offset = static_cast<std::size_t>(
        std::search(valid.begin(), valid.end(), payload.begin(), payload.end()) - valid.begin());
    bool mutated = false;
    bool rejected = false;
    try {
        (void)flitzzip::extract_arj(archive, root / "mutated-output", false, [&](const flitzzip::ProgressSnapshot&) {
            if (!mutated) {
                std::fstream changed(archive, std::ios::binary | std::ios::in | std::ios::out);
                changed.seekp(static_cast<std::streamoff>(payload_offset));
                changed.put('X');
                changed.flush();
                mutated = true;
            }
        });
    } catch (const flitzzip::ArchiveError& error) {
        REQUIRE_EQ(std::string_view(error.what()), "ARJ file payload CRC mismatch during publish");
        rejected = true;
    }
    REQUIRE_TRUE(mutated);
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "mutated-output" / "payload.bin"));
    std::filesystem::remove_all(root);
}
