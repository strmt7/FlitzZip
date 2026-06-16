#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "cpio/cpio_adapter.hpp"
#include "gzip/gzip_stream.hpp"
#include "rpm/rpm_adapter.hpp"
#include "rpm/rpm_format.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t kRpmTagPayloadFormat = 1124U;
constexpr std::uint32_t kRpmTagPayloadCompressor = 1125U;
constexpr std::uint32_t kRpmTypeString = 6U;

// Purpose: Read a full text file for RPM extraction equality checks.
// Inputs: `path` is the extracted file to read.
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

// Purpose: Append a 32-bit big-endian RPM header field.
// Inputs: `bytes` receives encoded data and `value` is the decoded integer.
// Outputs: Appends four bytes.
void append_be32(std::vector<unsigned char>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<unsigned char>((value >> 24U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
}

// Purpose: Append one RPM header index entry.
// Inputs: `header` receives bytes; tag/type/offset/count are RPM index fields.
// Outputs: Appends a 16-byte index record.
void append_rpm_index_entry(
    std::vector<unsigned char>& header,
    std::uint32_t tag,
    std::uint32_t type,
    std::uint32_t offset,
    std::uint32_t count) {
    append_be32(header, tag);
    append_be32(header, type);
    append_be32(header, offset);
    append_be32(header, count);
}

// Purpose: Build one RPM header with string tags.
// Inputs: `strings` maps tag ids to string values.
// Outputs: Returns a complete RPM header section.
std::vector<unsigned char> make_rpm_header(const std::vector<std::pair<std::uint32_t, std::string>>& strings) {
    std::vector<unsigned char> store;
    struct PendingEntry {
        std::uint32_t tag = 0;
        std::uint32_t offset = 0;
    };
    std::vector<PendingEntry> entries;
    for (const auto& [tag, value] : strings) {
        entries.push_back(PendingEntry{.tag = tag, .offset = static_cast<std::uint32_t>(store.size())});
        store.insert(store.end(), value.begin(), value.end());
        store.push_back(0U);
    }

    std::vector<unsigned char> header{0x8E, 0xAD, 0xE8, 0x01, 0, 0, 0, 0};
    append_be32(header, static_cast<std::uint32_t>(entries.size()));
    append_be32(header, static_cast<std::uint32_t>(store.size()));
    for (const auto& entry : entries) {
        append_rpm_index_entry(header, entry.tag, kRpmTypeString, entry.offset, 1U);
    }
    header.insert(header.end(), store.begin(), store.end());
    return header;
}

// Purpose: Append zero padding until the output size is eight-byte aligned.
// Inputs: `output` receives padding bytes.
// Outputs: Appends zero to seven bytes.
void append_rpm_eight_padding(std::vector<unsigned char>& output) {
    while (output.size() % 8U != 0U) {
        output.push_back(0U);
    }
}

// Purpose: Read a complete binary file into memory.
// Inputs: `path` is the file to load.
// Outputs: Returns all bytes.
std::vector<unsigned char> read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Purpose: Copy a binary file into a Gzip stream.
// Inputs: `source` is the uncompressed payload and `target` is the `.gz` destination.
// Outputs: Creates a valid Gzip stream.
void gzip_file(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::ifstream input(source, std::ios::binary);
    superzip::GzipOutputStream output(target);
    std::array<char, 64U * 1024U> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            output.write(buffer.data(), count);
        }
    }
    output.close();
}

// Purpose: Build a minimal RPM wrapper around a CPIO payload.
// Inputs: `archive` is the RPM path, `payload` is the payload file, and `compressor` is written into RPM metadata.
// Outputs: Creates a complete RPM file with a valid lead, signature header, package header, and payload.
void write_rpm_fixture(
    const std::filesystem::path& archive,
    const std::filesystem::path& payload,
    std::string_view compressor) {
    std::vector<unsigned char> bytes(96U, 0);
    bytes[0] = 0xED;
    bytes[1] = 0xAB;
    bytes[2] = 0xEE;
    bytes[3] = 0xDB;
    bytes[4] = 3U;
    bytes[5] = 0U;
    bytes[6] = 0U;
    bytes[7] = 0U;
    const std::string name = "superzip-test";
    std::copy(name.begin(), name.end(), bytes.begin() + 10);

    const auto signature = make_rpm_header({});
    bytes.insert(bytes.end(), signature.begin(), signature.end());
    append_rpm_eight_padding(bytes);

    const auto header = make_rpm_header({
        {kRpmTagPayloadFormat, "cpio"},
        {kRpmTagPayloadCompressor, std::string(compressor)},
    });
    bytes.insert(bytes.end(), header.begin(), header.end());
    const auto payload_bytes = read_binary_file(payload);
    bytes.insert(bytes.end(), payload_bytes.begin(), payload_bytes.end());

    std::ofstream output(archive, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Purpose: Create a CPIO payload containing one source directory.
// Inputs: `root` is the test root and `name` labels the fixture.
// Outputs: Returns the created CPIO archive path.
std::filesystem::path make_cpio_payload(const std::filesystem::path& root, std::string_view name) {
    const auto input = root / std::string(name);
    std::filesystem::create_directories(input);
    std::ofstream(input / "alpha.txt") << "rpm payload\n";
    const auto cpio = root / (std::string(name) + ".cpio");
    (void)superzip::compress_cpio({input}, cpio);
    return cpio;
}

}  // namespace

// Purpose: Verify native `.rpm` extraction reads an uncompressed CPIO payload.
// Inputs: A handcrafted RPM wrapper around a real CPIO archive.
// Outputs: Throws if extraction fails, format detection is wrong, or restored contents differ.
TEST_CASE(rpm_extraction_reads_uncompressed_cpio_payload) {
    const auto root = test_temp_dir("rpm-uncompressed");
    const auto cpio = make_cpio_payload(root, "input");
    const auto archive = root / "package.rpm";
    write_rpm_fixture(archive, cpio, "none");

    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Rpm);
    const auto info = superzip::scan_rpm_payload(archive);
    REQUIRE_EQ(info.compression, superzip::RpmPayloadCompression::None);

    const auto output = root / "out";
    const auto stats = superzip::extract_rpm(archive, output, false);
    REQUIRE_TRUE(stats.entries >= 2);
    REQUIRE_EQ(read_text_file(output / "input" / "alpha.txt"), read_text_file(root / "input" / "alpha.txt"));
}

// Purpose: Verify native `.rpm` extraction reads a Gzip-compressed CPIO payload.
// Inputs: A handcrafted RPM wrapper around a Gzip-compressed CPIO archive.
// Outputs: Throws if extraction fails or restored contents differ.
TEST_CASE(rpm_extraction_reads_gzip_cpio_payload) {
    const auto root = test_temp_dir("rpm-gzip");
    const auto cpio = make_cpio_payload(root, "input");
    const auto gzip = root / "payload.cpio.gz";
    gzip_file(cpio, gzip);
    const auto archive = root / "package.rpm";
    write_rpm_fixture(archive, gzip, "gzip");

    const auto info = superzip::scan_rpm_payload(archive);
    REQUIRE_EQ(info.compression, superzip::RpmPayloadCompression::Gzip);

    const auto output = root / "out";
    const auto stats = superzip::extract_rpm(archive, output, false);
    REQUIRE_TRUE(stats.entries >= 2);
    REQUIRE_EQ(read_text_file(output / "input" / "alpha.txt"), read_text_file(root / "input" / "alpha.txt"));
}

// Purpose: Verify RPM extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A valid RPM payload and a preexisting output file of the same name.
// Outputs: Throws if extraction overwrites while `overwrite` is false.
TEST_CASE(rpm_extraction_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("rpm-overwrite");
    const auto cpio = make_cpio_payload(root, "input");
    const auto archive = root / "package.rpm";
    write_rpm_fixture(archive, cpio, "none");
    const auto output = root / "out";
    std::filesystem::create_directories(output / "input");
    std::ofstream(output / "input" / "alpha.txt") << "old";

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_rpm(archive, output, false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "input" / "alpha.txt"), "old");
}

// Purpose: Verify RPM extraction rejects unsupported payload compressors before output.
// Inputs: A handcrafted RPM that declares `lzma` as its payload compressor.
// Outputs: Throws and creates no extracted files.
TEST_CASE(rpm_extraction_rejects_unsupported_payload_compressor_before_output) {
    const auto root = test_temp_dir("rpm-unsupported-compressor");
    const auto cpio = make_cpio_payload(root, "input");
    const auto archive = root / "package.rpm";
    write_rpm_fixture(archive, cpio, "lzma");

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_rpm(archive, root / "out", false));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify RPM parsing rejects malformed header entry counts safely.
// Inputs: A minimal RPM lead followed by a header that declares an excessive index count.
// Outputs: Throws `ArchiveError` without attempting payload extraction.
TEST_CASE(rpm_scanner_rejects_oversized_header_entry_count) {
    const auto root = test_temp_dir("rpm-oversized-header");
    const auto archive = root / "bad.rpm";
    std::vector<unsigned char> bytes(96U, 0);
    bytes[0] = 0xED;
    bytes[1] = 0xAB;
    bytes[2] = 0xEE;
    bytes[3] = 0xDB;
    bytes[4] = 3U;
    bytes[5] = 0U;
    bytes.insert(bytes.end(), {0x8E, 0xAD, 0xE8, 0x01, 0, 0, 0, 0});
    append_be32(bytes, 70'000U);
    append_be32(bytes, 0U);
    std::ofstream output(archive, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();

    bool rejected = false;
    try {
        static_cast<void>(superzip::scan_rpm_payload(archive));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify RPM parsing rejects unsupported lead versions before payload handling.
// Inputs: A minimal RPM-like file with valid magic but invalid major version.
// Outputs: Throws `ArchiveError` without attempting payload extraction.
TEST_CASE(rpm_scanner_rejects_invalid_lead_version) {
    const auto root = test_temp_dir("rpm-invalid-lead-version");
    const auto archive = root / "bad.rpm";
    std::vector<unsigned char> bytes(96U, 0);
    bytes[0] = 0xED;
    bytes[1] = 0xAB;
    bytes[2] = 0xEE;
    bytes[3] = 0xDB;
    bytes[4] = 4U;
    bytes[5] = 0U;
    std::ofstream output(archive, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();

    bool rejected = false;
    try {
        static_cast<void>(superzip::scan_rpm_payload(archive));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify RPM parsing rejects non-zero header reserved bytes.
// Inputs: A valid lead followed by a signature header with non-zero reserved bytes.
// Outputs: Throws `ArchiveError` while reading the signature header.
TEST_CASE(rpm_scanner_rejects_header_reserved_bytes) {
    const auto root = test_temp_dir("rpm-header-reserved-bytes");
    const auto archive = root / "bad.rpm";
    std::vector<unsigned char> bytes(96U, 0);
    bytes[0] = 0xED;
    bytes[1] = 0xAB;
    bytes[2] = 0xEE;
    bytes[3] = 0xDB;
    bytes[4] = 3U;
    bytes[5] = 0U;
    bytes.insert(bytes.end(), {0x8E, 0xAD, 0xE8, 0x01, 0, 1, 0, 0});
    append_be32(bytes, 0U);
    append_be32(bytes, 0U);
    std::ofstream output(archive, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();

    bool rejected = false;
    try {
        static_cast<void>(superzip::scan_rpm_payload(archive));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}
