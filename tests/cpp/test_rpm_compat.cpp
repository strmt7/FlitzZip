#include "test_compat_fixture.hpp"
#include "test_util.hpp"
#include "test_legacy_name.hpp"

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
#include <span>
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
void append_rpm_index_entry(std::vector<unsigned char>& header, std::uint32_t tag, std::uint32_t type,
                            std::uint32_t offset, std::uint32_t count) {
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
    flitzzip::GzipOutputStream output(target);
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
void write_rpm_fixture(const std::filesystem::path& archive, const std::filesystem::path& payload,
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
    const std::string name = "flitzzip-test";
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
    (void)flitzzip::compress_cpio({input}, cpio);
    return cpio;
}

}  // namespace

// Purpose: Build a minimal CPIO stream with explicit raw member-name bytes for RPM encoding tests.
// Inputs: Destination path and a bounded raw filename; the payload is the literal string "rpm".
// Outputs: Writes one aligned regular-file record and the required trailer without using production name encoding.
void write_rpm_name_payload(const std::filesystem::path& path, std::string_view name) {
    std::ofstream output(path, std::ios::binary);
    for (const bool trailer : {false, true}) {
        const auto member = trailer ? std::string_view("TRAILER!!!") : name;
        const std::array<std::uint32_t, 13> fields{1U,
                                                   trailer ? 0U : 0100644U,
                                                   0U,
                                                   0U,
                                                   1U,
                                                   0U,
                                                   trailer ? 0U : 3U,
                                                   0U,
                                                   0U,
                                                   0U,
                                                   0U,
                                                   static_cast<std::uint32_t>(member.size() + 1U),
                                                   0U};
        output << "070701";
        for (auto field : fields) {
            constexpr std::string_view digits = "0123456789ABCDEF";
            std::array<char, 8> hex{};
            for (std::size_t i = 0; i < hex.size(); ++i) {
                hex[hex.size() - 1U - i] = digits[field & 15U];
                field >>= 4U;
            }
            output.write(hex.data(), static_cast<std::streamsize>(hex.size()));
        }
        output.write(member.data(), static_cast<std::streamsize>(member.size()));
        output.put('\0');
        for (auto size = 111U + member.size(); size % 4U != 0; ++size) {
            output.put('\0');
        }
        if (!trailer) {
            output.write("rpm\0", 4);
        }
    }
}

// Purpose: Verify explicit name encoding survives the RPM wrapper and temporary-payload boundary.
// Inputs: Plain and Gzip RPM fixtures containing independent UTF-8 or Windows ANSI CPIO names.
// Outputs: Requires exact filenames and payloads for default UTF-8 and explicitly selected legacy mode.
TEST_CASE(rpm_member_names_encoding_forwarding) {
    const auto root = test_temp_dir("rpm-name-encoding");
    const auto [legacy, legacy_name] = test_legacy_archive_name();
    for (const bool system : {false, true}) {
        const std::string raw = system ? legacy : "\xe6\x97\xa5-\xf0\x9f\x9a\x80.txt";
        const auto expected = system ? legacy_name : std::wstring(L"\u65e5-\U0001f680.txt");
        const auto cpio = root / (system ? "legacy.cpio" : "utf8.cpio");
        write_rpm_name_payload(cpio, raw);
        const auto gzip = root / (system ? "legacy.gz" : "utf8.gz");
        gzip_file(cpio, gzip);
        for (const bool compressed : {false, true}) {
            const auto archive = root / (std::to_string(system) + std::to_string(compressed) + ".rpm");
            const auto output = root / archive.stem();
            write_rpm_fixture(archive, compressed ? gzip : cpio, compressed ? "gzip" : "none");
            const auto stats =
                system ? flitzzip::extract_rpm(archive, output, false, {}, flitzzip::ArchivePathEncoding::HostCodePage)
                       : flitzzip::extract_rpm(archive, output, false);
            REQUIRE_EQ(stats.entries, 1U);
            REQUIRE_EQ(read_text_file(output / expected), "rpm");
            REQUIRE_EQ(count_regular_files(output), 1U);
            if (system && compressed) {
                flitzzip_test::export_compat_fixture(archive, output);
            }
        }
    }
}

// Purpose: Verify native `.rpm` extraction reads an uncompressed CPIO payload.
// Inputs: A handcrafted RPM wrapper around a real CPIO archive.
// Outputs: Throws if extraction fails, format detection is wrong, or restored contents differ.
TEST_CASE(rpm_extraction_reads_uncompressed_cpio_payload) {
    const auto root = test_temp_dir("rpm-uncompressed");
    const auto cpio = make_cpio_payload(root, "input");
    const auto archive = root / "package.rpm";
    write_rpm_fixture(archive, cpio, "none");

    REQUIRE_EQ(flitzzip::detect_archive_format(archive), flitzzip::ArchiveFormat::Rpm);
    const auto info = flitzzip::scan_rpm_payload(archive);
    REQUIRE_EQ(info.compression, flitzzip::RpmPayloadCompression::None);

    const auto output = root / "out";
    const auto stats = flitzzip::extract_rpm(archive, output, false);
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

    const auto info = flitzzip::scan_rpm_payload(archive);
    REQUIRE_EQ(info.compression, flitzzip::RpmPayloadCompression::Gzip);

    const auto output = root / "out";
    const auto stats = flitzzip::extract_rpm(archive, output, false);
    REQUIRE_TRUE(stats.entries >= 2);
    REQUIRE_EQ(read_text_file(output / "input" / "alpha.txt"), read_text_file(root / "input" / "alpha.txt"));
    flitzzip_test::export_compat_fixture(archive, output);
}

// Purpose: Verify an RPM header compressor is selected without evaluating an absent magic fallback.
// Inputs: A package declaring Gzip whose intentionally malformed payload has no recognized compression magic.
// Outputs: Returns Gzip metadata without dereferencing a disengaged optional; later decode remains fail-closed.
TEST_CASE(rpm_scanner_uses_recognized_header_when_payload_magic_is_unknown) {
    const auto root = test_temp_dir("rpm-header-only-compressor");
    const auto payload = root / "opaque-payload.bin";
    std::ofstream(payload, std::ios::binary) << "not-compressed";
    const auto archive = root / "package.rpm";
    write_rpm_fixture(archive, payload, "gzip");

    const auto info = flitzzip::scan_rpm_payload(archive);
    REQUIRE_EQ(info.compression, flitzzip::RpmPayloadCompression::Gzip);

    bool rejected = false;
    try {
        static_cast<void>(flitzzip::extract_rpm(archive, root / "out", false));
    } catch (const flitzzip::Error&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
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
        static_cast<void>(flitzzip::extract_rpm(archive, output, false));
    } catch (const flitzzip::SecurityError&) {
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
        static_cast<void>(flitzzip::extract_rpm(archive, root / "out", false));
    } catch (const flitzzip::ArchiveError&) {
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
        static_cast<void>(flitzzip::scan_rpm_payload(archive));
    } catch (const flitzzip::ArchiveError&) {
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
        static_cast<void>(flitzzip::scan_rpm_payload(archive));
    } catch (const flitzzip::ArchiveError&) {
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
        static_cast<void>(flitzzip::scan_rpm_payload(archive));
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

namespace {

// Independent fixtures: CPython 3.13 gzip/bz2/lzma encoders over handcrafted newc records.
// The Zstandard fixture is a directly framed single-segment raw block (RFC 8878).
// Every payload contains only alpha.txt with bytes "rpm", followed by the newc trailer.
constexpr std::array<unsigned char, 84> kIndependentRpmGzip{
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0x33, 0x30, 0x37, 0x30, 0x37, 0x30, 0x34,
    0x80, 0x00, 0x30, 0x6d, 0x61, 0xe8, 0x68, 0x62, 0x80, 0x1d, 0x18, 0xa2, 0xf1, 0x8d, 0x0d, 0x88, 0x03,
    0x8e, 0x30, 0x46, 0x62, 0x4e, 0x41, 0x46, 0xa2, 0x5e, 0x49, 0x45, 0x09, 0x43, 0x51, 0x41, 0x2e, 0x83,
    0x01, 0x86, 0xdd, 0x78, 0x80, 0xa1, 0x01, 0x79, 0xc0, 0x09, 0xc6, 0x08, 0x09, 0x72, 0xf4, 0xf4, 0x71,
    0x0d, 0x52, 0x54, 0x54, 0x64, 0x00, 0x02, 0x00, 0x5b, 0x12, 0x10, 0x54, 0xf8, 0x00, 0x00, 0x00,
};

constexpr std::array<unsigned char, 106> kIndependentRpmBzip2{
    0x42, 0x5a, 0x68, 0x39, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0x4b, 0x96, 0x5d, 0x69, 0x00, 0x00, 0x20, 0x7f,
    0x80, 0x4c, 0x08, 0x08, 0x08, 0x28, 0x01, 0x6c, 0xc0, 0x32, 0x24, 0x14, 0x00, 0x20, 0x46, 0x54, 0x40, 0x20,
    0x00, 0x54, 0x53, 0x53, 0x23, 0x26, 0x4d, 0x3d, 0x04, 0xcd, 0x0d, 0x10, 0x62, 0x51, 0xa0, 0x1a, 0x0f, 0x53,
    0xd4, 0x00, 0x54, 0x80, 0xea, 0xef, 0x51, 0x34, 0x8d, 0xdb, 0x94, 0x48, 0x38, 0x98, 0x86, 0x74, 0xea, 0x43,
    0x50, 0x90, 0xc0, 0x4e, 0x81, 0x9b, 0x69, 0x0f, 0x2b, 0xd0, 0x43, 0x28, 0xe1, 0xf0, 0x98, 0x45, 0x72, 0xc0,
    0x1d, 0x80, 0xf4, 0x3b, 0xde, 0xbf, 0x17, 0x72, 0x45, 0x38, 0x50, 0x90, 0x4b, 0x96, 0x5d, 0x69,
};

constexpr std::array<unsigned char, 128> kIndependentRpmXz{
    0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00, 0x00, 0x04, 0xe6, 0xd6, 0xb4, 0x46, 0x02, 0x00, 0x21, 0x01, 0x16, 0x00, 0x00,
    0x00, 0x74, 0x2f, 0xe5, 0xa3, 0xe0, 0x00, 0xf7, 0x00, 0x3e, 0x5d, 0x00, 0x18, 0x0d, 0xdd, 0x04, 0x63, 0x9c, 0x72,
    0x1b, 0x76, 0xa1, 0xba, 0xc0, 0x1d, 0x75, 0x2e, 0xab, 0xee, 0xc0, 0x18, 0x46, 0x76, 0xdc, 0x48, 0x79, 0xe5, 0x47,
    0xf2, 0xc0, 0xa6, 0xf3, 0x1d, 0x32, 0x66, 0x77, 0xa0, 0x3e, 0x7c, 0x0e, 0x06, 0x1f, 0x06, 0xe9, 0x13, 0xb2, 0xcd,
    0x44, 0x98, 0xaf, 0x73, 0xb8, 0x93, 0xee, 0x39, 0xe6, 0xac, 0xdf, 0x67, 0xbf, 0x9a, 0xde, 0xe0, 0x00, 0x00, 0x00,
    0x00, 0x09, 0xf3, 0x02, 0x4f, 0x14, 0xa6, 0xde, 0x7a, 0x00, 0x01, 0x5a, 0xf8, 0x01, 0x00, 0x00, 0x00, 0xb5, 0x83,
    0xb8, 0xae, 0xb1, 0xc4, 0x67, 0xfb, 0x02, 0x00, 0x00, 0x00, 0x00, 0x04, 0x59, 0x5a,
};

constexpr std::array<unsigned char, 257> kIndependentRpmZstd{
    0x28, 0xb5, 0x2f, 0xfd, 0x20, 0xf8, 0xc1, 0x07, 0x00, 0x30, 0x37, 0x30, 0x37, 0x30, 0x31, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x31, 0x30, 0x30, 0x30, 0x30, 0x38, 0x31, 0x41, 0x34, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x33, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x41, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x61, 0x6c, 0x70, 0x68, 0x61, 0x2e, 0x74, 0x78, 0x74, 0x00, 0x72, 0x70, 0x6d, 0x00,
    0x30, 0x37, 0x30, 0x37, 0x30, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x31, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
    0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x42, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x54, 0x52, 0x41, 0x49,
    0x4c, 0x45, 0x52, 0x21, 0x21, 0x21, 0x00, 0x00, 0x00, 0x00,
};

struct IndependentRpmPayload {
    std::string_view name;
    std::string_view alias;
    std::span<const unsigned char> bytes;
    flitzzip::RpmPayloadCompression compression;
};
const std::array kIndependentRpmPayloads{
    IndependentRpmPayload{"gzip", "gz", kIndependentRpmGzip, flitzzip::RpmPayloadCompression::Gzip},
    IndependentRpmPayload{"bzip2", "bz2", kIndependentRpmBzip2, flitzzip::RpmPayloadCompression::Bzip2},
    IndependentRpmPayload{"xz", "xz", kIndependentRpmXz, flitzzip::RpmPayloadCompression::Xz},
    IndependentRpmPayload{"zstd", "zst", kIndependentRpmZstd, flitzzip::RpmPayloadCompression::Zstd},
};

// Purpose: Persist a bounded independent RPM fixture or a single controlled mutation.
// Inputs: Owned output path and at most one MiB of literal fixture bytes.
// Outputs: Complete fixture file; any write failure fails the test.
void write_rpm_bytes(const std::filesystem::path& path, std::span<const unsigned char> bytes) {
    REQUIRE_TRUE(bytes.size() <= 1024U * 1024U);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    REQUIRE_TRUE(output.good());
}

// Purpose: Require an RPM error to preserve caller data and leave no temporary payloads behind.
// Inputs: Invalid archive, owned output directory and expected error category.
// Outputs: Requires rejection, the original sentinel bytes and exactly one destination entry.
void require_rpm_rejected_without_publication(const std::filesystem::path& archive,
                                              const std::filesystem::path& destination, std::string_view category) {
    std::filesystem::create_directory(destination);
    std::ofstream(destination / "sentinel.txt", std::ios::binary) << "keep caller data";
    bool rejected = false;
    try {
        (void)flitzzip::extract_rpm(archive, destination, true);
    } catch (const flitzzip::Error& error) {
        REQUIRE_TRUE(std::string_view(error.what()).find(category) != std::string_view::npos);
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(destination / "sentinel.txt"), "keep caller data");
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator{}),
               1);
}
}  // namespace

// Purpose: Exercise every supported compressed RPM payload with independently encoded content.
// Inputs: Small literal streams, explicit compressor names, aliases and absent compressor values.
// Outputs: Exact metadata, one byte-exact file and no retained temporary files for every decoder.
TEST_CASE(rpm_independent_compressed_payloads_and_aliases_round_trip) {
    const auto root = test_temp_dir("rpm-independent-methods");
    unsigned trial = 0;
    for (const auto& fixture : kIndependentRpmPayloads) {
        const auto payload = root / (std::string(fixture.name) + ".payload");
        write_rpm_bytes(payload, fixture.bytes);
        for (const auto compressor : {fixture.name, fixture.alias, std::string_view{}}) {
            const auto archive = root / ("package-" + std::to_string(trial) + ".rpm");
            const auto destination = root / ("out-" + std::to_string(trial++));
            write_rpm_fixture(archive, payload, compressor);
            const auto metadata = flitzzip::scan_rpm_payload(archive);
            REQUIRE_EQ(metadata.compression, fixture.compression);
            REQUIRE_EQ(metadata.size, fixture.bytes.size());
            REQUIRE_EQ(metadata.offset + metadata.size, std::filesystem::file_size(archive));
            const auto stats = flitzzip::extract_rpm(archive, destination, false);
            REQUIRE_EQ(stats.entries, 1U);
            REQUIRE_EQ(stats.output_bytes, 3U);
            REQUIRE_EQ(stats.input_bytes, std::filesystem::file_size(archive));
            REQUIRE_TRUE(!stats.gpu_used);
            REQUIRE_EQ(read_text_file(destination / "alpha.txt"), "rpm");
            REQUIRE_EQ(
                std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator{}),
                1);
        }
    }
}

// Purpose: Reject truncated compressed RPM tails before publication and permit a valid subsequent decode.
// Inputs: Each independent payload with its final byte removed, then the intact stream.
// Outputs: Failure leaves sentinel data and no staging debris; valid reuse recovers the exact member.
TEST_CASE(rpm_compressed_tail_failure_cleans_staging_and_allows_reuse) {
    const auto root = test_temp_dir("rpm-truncated-methods");
    for (const auto& fixture : kIndependentRpmPayloads) {
        const auto payload = root / (std::string(fixture.name) + ".payload");
        const auto archive = root / (std::string(fixture.name) + ".rpm");
        const auto destination = root / std::string(fixture.name);
        write_rpm_bytes(payload, fixture.bytes.first(fixture.bytes.size() - 1U));
        write_rpm_fixture(archive, payload, fixture.name);
        require_rpm_rejected_without_publication(archive, destination, "");
        write_rpm_bytes(payload, fixture.bytes);
        write_rpm_fixture(archive, payload, fixture.name);
        REQUIRE_EQ(flitzzip::extract_rpm(archive, destination, false).entries, 1U);
        REQUIRE_EQ(read_text_file(destination / "alpha.txt"), "rpm");
        REQUIRE_EQ(read_text_file(destination / "sentinel.txt"), "keep caller data");
        REQUIRE_EQ(
            std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator{}), 2);
    }
}

// Purpose: Check RPM temporary-payload cleanup when the caller cancels before first publication.
// Inputs: Each valid compressed method and an independently throwing progress callback.
// Outputs: Original error propagation and unchanged destination, followed by successful reuse.
TEST_CASE(rpm_cancellation_cleans_each_compressed_payload) {
    const auto root = test_temp_dir("rpm-cancel-methods");
    for (const auto& fixture : kIndependentRpmPayloads) {
        const auto payload = root / (std::string(fixture.name) + ".payload");
        const auto archive = root / (std::string(fixture.name) + ".rpm");
        const auto destination = root / std::string(fixture.name);
        write_rpm_bytes(payload, fixture.bytes);
        write_rpm_fixture(archive, payload, fixture.name);
        std::filesystem::create_directory(destination);
        std::ofstream(destination / "alpha.txt", std::ios::binary) << "original";
        unsigned callbacks = 0;
        bool cancelled = false;
        try {
            (void)flitzzip::extract_rpm(archive, destination, true, [&](const flitzzip::ProgressSnapshot&) {
                ++callbacks;
                throw std::runtime_error("independent RPM cancellation");
            });
        } catch (const std::runtime_error& error) {
            REQUIRE_EQ(std::string(error.what()), "independent RPM cancellation");
            cancelled = true;
        }
        REQUIRE_TRUE(cancelled);
        REQUIRE_EQ(callbacks, 1U);
        REQUIRE_EQ(read_text_file(destination / "alpha.txt"), "original");
        REQUIRE_EQ(
            std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator{}), 1);
        REQUIRE_EQ(flitzzip::extract_rpm(archive, destination, true).entries, 1U);
        REQUIRE_EQ(read_text_file(destination / "alpha.txt"), "rpm");
    }
}

// Purpose: Exercise RPM structural bounds and ambiguous payload metadata without large allocations.
// Inputs: Single-field mutations of a valid independently compressed package.
// Outputs: Specific errors before destination publication for every malformed field.
TEST_CASE(rpm_header_boundary_mutations_preserve_destination) {
    const auto root = test_temp_dir("rpm-header-boundaries");
    const auto payload = root / "payload.gz";
    const auto archive = root / "package.rpm";
    write_rpm_bytes(payload, kIndependentRpmGzip);
    write_rpm_fixture(archive, payload, "gzip");
    const auto original = read_binary_file(archive);
    // Empty signature ends at 112; the package has two 16-byte indexes and a 10-byte string store.
    REQUIRE_TRUE(original.size() == 170U + kIndependentRpmGzip.size());
    struct Mutation {
        std::size_t offset;
        unsigned char value;
        std::string_view category;
    };
    constexpr std::array mutations{
        Mutation{0U, 0U, "lead magic"},
        Mutation{96U, 0U, "signature header magic"},
        Mutation{112U, 0U, "package header magic"},
        Mutation{116U, 1U, "package header reserved bytes"},
        Mutation{121U, 1U, "package header contains too many entries"},
        Mutation{124U, 5U, "header store exceeds"},
        Mutation{127U, 255U, "header extends past end"},
        Mutation{139U, 11U, "entry offset is outside"},
        Mutation{139U, 10U, "unterminated or too long"},
        Mutation{143U, 0U, "invalid cardinality"},
        Mutation{143U, 2U, "invalid cardinality"},
        Mutation{147U, 100U, "duplicate payload string tag"},
        Mutation{169U, 1U, "unterminated or too long"},
        Mutation{160U, 't', "unsupported RPM payload format"},
    };
    unsigned trial = 0;
    for (const auto& mutation : mutations) {
        auto bytes = original;
        bytes.at(mutation.offset) = mutation.value;
        write_rpm_bytes(archive, bytes);
        require_rpm_rejected_without_publication(archive, root / std::to_string(trial++), mutation.category);
    }
    for (const std::size_t size : {0U, 95U, 96U, 111U, 112U, 127U, 159U, 169U, 170U}) {
        write_rpm_bytes(archive, std::span(original).first(size));
        require_rpm_rejected_without_publication(archive, root / std::to_string(trial++), "");
    }
    write_rpm_fixture(archive, payload, "none");
    require_rpm_rejected_without_publication(archive, root / "mismatch", "does not match payload magic");
    write_rpm_bytes(payload, std::array<unsigned char, 1>{1U});
    write_rpm_fixture(archive, payload, "");
    require_rpm_rejected_without_publication(archive, root / "unknown", "unrecognized RPM payload compression");
}
