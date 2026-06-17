#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "cpio/cpio_adapter.hpp"
#include "gzip/gzip_adapter.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t kTestCpioRegularMode = 0100644U;
constexpr std::uint32_t kTestCpioSymlinkMode = 0120777U;

// Purpose: Read a full text file for roundtrip equality checks.
// Inputs: `path` is the file to read.
// Outputs: Returns the complete file payload.
std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Purpose: Read a full binary file for corruption tests.
// Inputs: `path` is the file to read.
// Outputs: Returns the complete byte payload.
std::vector<unsigned char> read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<unsigned char> bytes;
    char value = 0;
    while (input.get(value)) {
        bytes.push_back(static_cast<unsigned char>(value));
    }
    return bytes;
}

// Purpose: Write exact binary bytes for corruption tests.
// Inputs: `path` is the destination file and `bytes` is the payload.
// Outputs: Creates or replaces the file.
void write_binary_file(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
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

// Purpose: Append one uppercase eight-digit CPIO hex field for handcrafted fixtures.
// Inputs: `header` receives bytes and `value` is the field value.
// Outputs: Appends exactly eight ASCII hex bytes.
void append_test_cpio_hex(std::string& header, std::uint32_t value) {
    std::array<char, 9> encoded{};
    const int written = std::snprintf(encoded.data(), encoded.size(), "%08X", value);
    REQUIRE_EQ(written, 8);
    header.append(encoded.data(), 8U);
}

// Purpose: Compute new ASCII CPIO padding for a section length.
// Inputs: `size` is an unaligned byte count.
// Outputs: Returns a value in `[0, 3]`.
std::size_t cpio_padding(std::size_t size) {
    const auto remainder = size % 4U;
    return remainder == 0 ? 0 : 4U - remainder;
}

// Purpose: Compute the simple `070702` CPIO payload checksum.
// Inputs: `payload` is the file payload.
// Outputs: Returns the unsigned byte sum used by the CRC CPIO variant.
std::uint32_t cpio_payload_sum(std::string_view payload) {
    std::uint32_t sum = 0;
    for (const unsigned char ch : payload) {
        sum += ch;
    }
    return sum;
}

// Purpose: Write one CPIO header/name/payload entry for handcrafted parser tests.
// Inputs: `output` is the archive stream and the remaining values define one CPIO entry.
// Outputs: Appends a complete aligned CPIO entry.
void write_cpio_entry(
    std::ofstream& output,
    std::string_view magic,
    std::string_view path,
    std::uint32_t mode,
    std::uint32_t nlink,
    std::string_view payload,
    std::uint32_t check) {
    std::string header;
    header.reserve(110U);
    header.append(magic);
    append_test_cpio_hex(header, 1);
    append_test_cpio_hex(header, mode);
    append_test_cpio_hex(header, 0);
    append_test_cpio_hex(header, 0);
    append_test_cpio_hex(header, nlink);
    append_test_cpio_hex(header, 0);
    append_test_cpio_hex(header, static_cast<std::uint32_t>(payload.size()));
    append_test_cpio_hex(header, 0);
    append_test_cpio_hex(header, 0);
    append_test_cpio_hex(header, 0);
    append_test_cpio_hex(header, 0);
    append_test_cpio_hex(header, static_cast<std::uint32_t>(path.size() + 1U));
    append_test_cpio_hex(header, check);
    REQUIRE_EQ(header.size(), static_cast<std::size_t>(110));

    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    output.write(path.data(), static_cast<std::streamsize>(path.size()));
    const char nul = '\0';
    output.write(&nul, 1);
    std::array<char, 4> zeros{};
    output.write(zeros.data(), static_cast<std::streamsize>(cpio_padding(110U + path.size() + 1U)));
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    output.write(zeros.data(), static_cast<std::streamsize>(cpio_padding(payload.size())));
}

// Purpose: Write the required CPIO trailer entry.
// Inputs: `output` is the archive stream.
// Outputs: Appends a complete `TRAILER!!!` record.
void write_cpio_trailer(std::ofstream& output) {
    write_cpio_entry(output, "070701", "TRAILER!!!", 0, 1, "", 0);
}

// Purpose: Write a one-entry CPIO archive fixture.
// Inputs: `archive` is the output path and the remaining values define the sole non-trailer entry.
// Outputs: Creates a complete CPIO file.
void write_one_entry_cpio(
    const std::filesystem::path& archive,
    std::string_view magic,
    std::string_view path,
    std::uint32_t mode,
    std::uint32_t nlink,
    std::string_view payload,
    std::uint32_t check) {
    std::ofstream output(archive, std::ios::binary);
    write_cpio_entry(output, magic, path, mode, nlink, payload, check);
    write_cpio_trailer(output);
}

}  // namespace

// Purpose: Verify native `.cpio` compatibility roundtrip for files and directories.
// Inputs: A temporary source tree compressed through the CPIO adapter.
// Outputs: Throws if extraction fails, format detection is wrong, or restored contents differ.
TEST_CASE(cpio_roundtrip_extracts_files_and_directories) {
    const auto root = test_temp_dir("cpio-roundtrip");
    const auto input = root / "input";
    std::filesystem::create_directories(input / "dir");
    {
        std::ofstream(input / "dir" / "hello.txt") << "hello from cpio\n";
        std::ofstream(input / "root.txt") << "root file\n";
    }

    const auto archive = root / "sample.cpio";
    const auto compress_stats = superzip::compress_cpio({input}, archive);
    REQUIRE_TRUE(compress_stats.entries >= 3);
    REQUIRE_TRUE(compress_stats.output_bytes > compress_stats.input_bytes);
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Cpio);

    const auto output = root / "output";
    const auto extract_stats = superzip::extract_cpio(archive, output, false);
    REQUIRE_EQ(extract_stats.output_bytes, compress_stats.input_bytes);
    REQUIRE_EQ(read_text_file(output / "input" / "dir" / "hello.txt"), read_text_file(input / "dir" / "hello.txt"));
    REQUIRE_EQ(read_text_file(output / "input" / "root.txt"), read_text_file(input / "root.txt"));
}

// Purpose: Verify `.cpgz` roundtrip through the Gzip-filtered CPIO stream path.
// Inputs: A temporary source tree compressed with the CPIO.GZ adapter.
// Outputs: Throws if detection, extraction, statistics, or restored contents are wrong.
TEST_CASE(cpio_gzip_roundtrip_extracts_files_and_directories) {
    const auto root = test_temp_dir("cpio-gzip-roundtrip");
    const auto input = root / "input";
    std::filesystem::create_directories(input / "dir");
    {
        std::ofstream(input / "dir" / "hello.txt") << "hello from cpgz\n";
        std::ofstream(input / "root.txt") << "compressed cpio\n";
    }

    const auto archive = root / "sample.cpgz";
    const auto compress_stats = superzip::compress_cpio_gzip({input}, archive);
    REQUIRE_TRUE(compress_stats.entries >= 3);
    REQUIRE_TRUE(compress_stats.output_bytes > 0);
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::CpioGzip);

    const auto output = root / "output";
    const auto extract_stats = superzip::extract_cpio_gzip(archive, output, false);
    REQUIRE_EQ(extract_stats.output_bytes, compress_stats.input_bytes);
    REQUIRE_EQ(read_text_file(output / "input" / "dir" / "hello.txt"), read_text_file(input / "dir" / "hello.txt"));
    REQUIRE_EQ(read_text_file(output / "input" / "root.txt"), read_text_file(input / "root.txt"));
}

// Purpose: Verify CPIO extraction rejects traversal metadata during the validation pass.
// Inputs: A handcrafted CPIO archive containing `../escape.txt`.
// Outputs: Throws if extraction accepts the unsafe path or writes any output.
TEST_CASE(cpio_extraction_rejects_traversal_before_output) {
    const auto root = test_temp_dir("cpio-traversal");
    const auto archive = root / "bad.cpio";
    write_one_entry_cpio(archive, "070701", "../escape.txt", kTestCpioRegularMode, 1, "bad", 0);

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_cpio(archive, root / "out", false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify CPIO.GZ extraction rejects traversal metadata during the decompressed validation pass.
// Inputs: A handcrafted unsafe CPIO stream wrapped in a valid Gzip member.
// Outputs: Throws if extraction accepts the unsafe path or writes any output.
TEST_CASE(cpio_gzip_extraction_rejects_traversal_before_output) {
    const auto root = test_temp_dir("cpio-gzip-traversal");
    const auto plain = root / "bad.cpio";
    const auto archive = root / "bad.cpgz";
    write_one_entry_cpio(plain, "070701", "../escape.txt", kTestCpioRegularMode, 1, "bad", 0);
    (void)superzip::compress_gzip_file(plain, archive);

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_cpio_gzip(archive, root / "out", false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify CPIO extraction rejects links and special files instead of creating host filesystem objects.
// Inputs: A handcrafted CPIO archive with a symbolic-link mode entry.
// Outputs: Throws `SecurityError` before any target is created.
TEST_CASE(cpio_extraction_rejects_special_entries) {
    const auto root = test_temp_dir("cpio-special");
    const auto archive = root / "special.cpio";
    write_one_entry_cpio(archive, "070701", "link", kTestCpioSymlinkMode, 1, "target", 0);

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_cpio(archive, root / "out", false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify CPIO extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A CPIO file entry and a preexisting output file of the same name.
// Outputs: Throws if extraction overwrites while `overwrite` is false.
TEST_CASE(cpio_extraction_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("cpio-overwrite");
    const auto archive = root / "overwrite.cpio";
    write_one_entry_cpio(archive, "070701", "file.txt", kTestCpioRegularMode, 1, "new", 0);
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    std::ofstream(output / "file.txt") << "old";

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_cpio(archive, output, false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "file.txt"), "old");
}

// Purpose: Verify CPIO.GZ extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A compressed CPIO file entry and a preexisting output file of the same name.
// Outputs: Throws if extraction overwrites while `overwrite` is false.
TEST_CASE(cpio_gzip_extraction_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("cpio-gzip-overwrite");
    const auto plain = root / "overwrite.cpio";
    const auto archive = root / "overwrite.cpgz";
    write_one_entry_cpio(plain, "070701", "file.txt", kTestCpioRegularMode, 1, "new", 0);
    (void)superzip::compress_gzip_file(plain, archive);
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    std::ofstream(output / "file.txt") << "old";

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_cpio_gzip(archive, output, false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "file.txt"), "old");
}

// Purpose: Verify CRC-format CPIO archives extract when their payload checksum is correct.
// Inputs: A handcrafted `070702` CPIO entry with a valid checksum.
// Outputs: Throws if extraction fails or restored contents differ.
TEST_CASE(cpio_crc_format_extracts_when_checksum_matches) {
    const auto root = test_temp_dir("cpio-crc-ok");
    const auto archive = root / "good-crc.cpio";
    const std::string payload = "payload";
    write_one_entry_cpio(archive, "070702", "payload.txt", kTestCpioRegularMode, 1, payload, cpio_payload_sum(payload));

    const auto output = root / "out";
    const auto stats = superzip::extract_cpio(archive, output, false);
    REQUIRE_EQ(stats.output_bytes, static_cast<std::uint64_t>(payload.size()));
    REQUIRE_EQ(read_text_file(output / "payload.txt"), payload);
}

// Purpose: Verify CRC-format CPIO archives fail closed when the payload checksum is wrong.
// Inputs: A handcrafted `070702` CPIO entry with an intentionally wrong checksum.
// Outputs: Throws before publishing any output file.
TEST_CASE(cpio_crc_format_rejects_checksum_mismatch_before_output) {
    const auto root = test_temp_dir("cpio-crc");
    const auto archive = root / "bad-crc.cpio";
    const std::string payload = "payload";
    write_one_entry_cpio(archive, "070702", "payload.txt", kTestCpioRegularMode, 1, payload, cpio_payload_sum(payload) + 1U);

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_cpio(archive, root / "out", false));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify CRC-format CPIO.GZ archives fail closed when the inner payload checksum is wrong.
// Inputs: A valid Gzip stream around a handcrafted `070702` CPIO entry with an intentionally wrong checksum.
// Outputs: Throws during validation before publishing any output file.
TEST_CASE(cpio_gzip_crc_format_rejects_checksum_mismatch_before_output) {
    const auto root = test_temp_dir("cpio-gzip-crc");
    const auto plain = root / "bad-crc.cpio";
    const auto archive = root / "bad-crc.cpgz";
    const std::string payload = "payload";
    write_one_entry_cpio(plain, "070702", "payload.txt", kTestCpioRegularMode, 1, payload, cpio_payload_sum(payload) + 1U);
    (void)superzip::compress_gzip_file(plain, archive);

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_cpio_gzip(archive, root / "out", false));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify Gzip trailer corruption in a CPIO.GZ archive does not publish output.
// Inputs: A valid compressed CPIO archive whose Gzip trailer CRC is corrupted.
// Outputs: Throws before the second extraction pass and leaves the destination empty.
TEST_CASE(cpio_gzip_rejects_gzip_crc_mismatch_before_output) {
    const auto root = test_temp_dir("cpio-gzip-trailer-crc");
    const auto input = root / "input";
    std::filesystem::create_directories(input);
    std::ofstream(input / "payload.txt") << "payload";
    const auto archive = root / "payload.cpgz";
    (void)superzip::compress_cpio_gzip({input}, archive);

    auto bytes = read_binary_file(archive);
    REQUIRE_TRUE(bytes.size() > 18U);
    bytes[bytes.size() - 8U] ^= 0xFFU;
    write_binary_file(archive, bytes);

    const auto output = root / "out";
    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_cpio_gzip(archive, output, false));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}
