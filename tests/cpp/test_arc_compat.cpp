#include "test_util.hpp"

#include "arc/arc_adapter.hpp"
#include "core/archive_format.hpp"
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

struct ArcFixtureEntry {
    std::string name;
    std::vector<unsigned char> payload;
    std::uint8_t method = 2;
    std::optional<std::uint16_t> crc_override;
};

// Purpose: Append one little-endian 16-bit fixture field.
// Inputs: `bytes` is the mutable ARC fixture and `value` is the unsigned field value.
// Outputs: Appends two bytes.
void append_le16(std::vector<unsigned char>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
}

// Purpose: Append one little-endian 32-bit fixture field.
// Inputs: `bytes` is the mutable ARC fixture and `value` is the unsigned field value.
// Outputs: Appends four bytes.
void append_le32(std::vector<unsigned char>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 24U) & 0xFFU));
}

// Purpose: Compute fixture CRC-16/ARC over payload bytes.
// Inputs: `bytes` is the fixture payload.
// Outputs: Returns the finalized CRC-16/ARC value.
std::uint16_t fixture_crc16(std::span<const unsigned char> bytes) {
    std::uint16_t crc = 0;
    for (const auto byte : bytes) {
        crc = static_cast<std::uint16_t>(crc ^ byte);
        for (int bit = 0; bit < 8; ++bit) {
            const bool carry = (crc & 1U) != 0U;
            crc = static_cast<std::uint16_t>(crc >> 1U);
            if (carry) {
                crc = static_cast<std::uint16_t>(crc ^ 0xA001U);
            }
        }
    }
    return crc;
}

// Purpose: Append one fixed-width ARC filename field.
// Inputs: `archive` is the mutable fixture and `name` is the ASCII path to encode.
// Outputs: Appends exactly 13 bytes or throws through the test assertion framework.
void append_arc_name(std::vector<unsigned char>& archive, const std::string& name) {
    REQUIRE_TRUE(name.size() <= 13U);
    archive.insert(archive.end(), name.begin(), name.end());
    archive.resize(archive.size() + (13U - name.size()), 0);
}

// Purpose: Append one ARC entry header and payload.
// Inputs: `archive` is the mutable fixture and `entry` supplies method, path, payload, and optional CRC override.
// Outputs: Appends one complete ARC member.
void append_arc_entry(std::vector<unsigned char>& archive, const ArcFixtureEntry& entry) {
    const auto size = static_cast<std::uint32_t>(entry.payload.size());
    const auto crc = entry.crc_override.value_or(fixture_crc16(entry.payload));
    archive.push_back(0x1A);
    archive.push_back(entry.method);
    append_arc_name(archive, entry.name);
    append_le32(archive, size);
    append_le16(archive, 0);
    append_le16(archive, 0);
    append_le16(archive, crc);
    if (entry.method != 1U) {
        append_le32(archive, size);
    }
    archive.insert(archive.end(), entry.payload.begin(), entry.payload.end());
}

// Purpose: Build a minimal ARC archive with supplied local entries.
// Inputs: `entries` are exact local-entry fixtures.
// Outputs: Returns ARC bytes ending in the standard `0x1A 0x00` end marker.
std::vector<unsigned char> make_arc_archive(const std::vector<ArcFixtureEntry>& entries) {
    std::vector<unsigned char> archive;
    for (const auto& entry : entries) {
        append_arc_entry(archive, entry);
    }
    archive.push_back(0x1A);
    archive.push_back(0x00);
    return archive;
}

// Purpose: Write one byte vector to disk for ARC fixtures.
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

}  // namespace

// Purpose: Verify native ARC extraction for unpacked method-1 and method-2 files.
// Inputs: A handcrafted ARC fixture with old and new unpacked entries.
// Outputs: Throws if detection, extraction, or payload restoration fails.
TEST_CASE(arc_extracts_unpacked_files) {
    const auto root = test_temp_dir("arc-unpacked");
    const auto archive = root / "sample.arc";
    write_binary_file(
        archive,
        make_arc_archive({
            ArcFixtureEntry{.name = "hello.txt", .payload = {'h', 'e', 'l', 'l', 'o'}, .method = 2},
            ArcFixtureEntry{.name = "old.txt", .payload = {'o', 'l', 'd'}, .method = 1},
        }));

    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Arc);
    const auto info = superzip::archive_format_info(superzip::ArchiveFormat::Arc);
    REQUIRE_TRUE(info.can_extract);
    REQUIRE_TRUE(!info.can_create);

    const auto output = root / "out";
    const auto stats = superzip::extract_arc(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(2));
    REQUIRE_EQ(read_text_file(output / "hello.txt"), std::string("hello"));
    REQUIRE_EQ(read_text_file(output / "old.txt"), std::string("old"));
    std::filesystem::remove_all(root);
}

// Purpose: Verify ARC extraction refuses existing output files unless overwrite is explicit.
// Inputs: An unpacked ARC entry whose target already exists.
// Outputs: Throws if overwrite refusal is bypassed or existing content changes.
TEST_CASE(arc_extract_rejects_overwrite_by_default) {
    const auto root = test_temp_dir("arc-overwrite");
    const auto archive = root / "sample.arc";
    write_binary_file(archive, make_arc_archive({ArcFixtureEntry{.name = "same.txt", .payload = {'n', 'e', 'w'}}}));
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    write_text_file(output / "same.txt", "old");

    bool rejected = false;
    try {
        (void)superzip::extract_arc(archive, output, false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "same.txt"), std::string("old"));
    std::filesystem::remove_all(root);
}

// Purpose: Verify ARC extraction rejects unsafe embedded paths before publication.
// Inputs: An unpacked ARC entry with parent-directory traversal.
// Outputs: Throws if extraction succeeds or writes outside the destination.
TEST_CASE(arc_extract_rejects_unsafe_paths) {
    const auto root = test_temp_dir("arc-unsafe-path");
    const auto archive = root / "unsafe.arc";
    write_binary_file(archive, make_arc_archive({ArcFixtureEntry{.name = "..\\evil.txt", .payload = {'x'}}}));

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_arc(archive, output, true);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "evil.txt"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify compressed ARC methods fail explicitly until a vetted decoder exists.
// Inputs: A valid ARC header using compression method 3.
// Outputs: Throws if unsupported compressed ARC data is accepted or publishes output.
TEST_CASE(arc_extract_rejects_compressed_methods_without_output) {
    const auto root = test_temp_dir("arc-compressed-method");
    const auto archive = root / "compressed.arc";
    write_binary_file(archive, make_arc_archive({ArcFixtureEntry{.name = "packed.txt", .payload = {'x'}, .method = 3}}));

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_arc(archive, output, true);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify ARC CRC mismatches fail before final output publication.
// Inputs: An unpacked ARC entry with a deliberately incorrect payload CRC.
// Outputs: Throws if corrupt payload is extracted or leaves final files behind.
TEST_CASE(arc_extract_rejects_payload_crc_mismatch_without_output) {
    const auto root = test_temp_dir("arc-crc-mismatch");
    const auto archive = root / "bad-crc.arc";
    write_binary_file(
        archive,
        make_arc_archive({ArcFixtureEntry{.name = "bad.txt", .payload = {'b', 'a', 'd'}, .crc_override = 0x1234U}}));

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_arc(archive, output, true);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}
