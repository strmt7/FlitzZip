#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "gzip/gzip_adapter.hpp"
#include "tar/tar_adapter.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Purpose: Write a fixed-size TAR string field for handcrafted `.tar.gz` fixtures.
// Inputs: `header` is the mutable block, `offset`/`length` select the field, and `value` is ASCII metadata.
// Outputs: Copies `value` into the field; throws through the test harness if it cannot fit.
void put_test_tar_string(std::array<char, 512>& header, std::size_t offset, std::size_t length, std::string_view value) {
    REQUIRE_TRUE(value.size() <= length);
    std::copy(value.begin(), value.end(), header.begin() + static_cast<std::ptrdiff_t>(offset));
}

// Purpose: Write a simple octal TAR numeric field for handcrafted `.tar.gz` fixtures.
// Inputs: `header` is the mutable block, `offset`/`length` select the field, and `value` is the small integer to encode.
// Outputs: Encodes a NUL-terminated octal field.
void put_test_tar_octal(std::array<char, 512>& header, std::size_t offset, std::size_t length, std::uint64_t value) {
    std::string encoded(length - 1U, '0');
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const auto index = encoded.size() - 1U - i;
        encoded[index] = static_cast<char>('0' + (value & 7U));
        value >>= 3U;
    }
    REQUIRE_TRUE(value == 0);
    std::copy(encoded.begin(), encoded.end(), header.begin() + static_cast<std::ptrdiff_t>(offset));
    header[offset + length - 1U] = '\0';
}

// Purpose: Create a small TAR header for compressed TAR parser-security tests.
// Inputs: `path` is the archive entry path, `typeflag` is the TAR type, and `size` is payload length.
// Outputs: Returns a checksummed USTAR header block.
std::array<char, 512> make_test_tar_header(std::string_view path, char typeflag, std::uint64_t size) {
    std::array<char, 512> header{};
    put_test_tar_string(header, 0, 100, path);
    put_test_tar_octal(header, 100, 8, typeflag == '5' ? 0755 : 0644);
    put_test_tar_octal(header, 108, 8, 0);
    put_test_tar_octal(header, 116, 8, 0);
    put_test_tar_octal(header, 124, 12, size);
    put_test_tar_octal(header, 136, 12, 0);
    std::fill(header.begin() + 148, header.begin() + 156, ' ');
    header[156] = typeflag;
    put_test_tar_string(header, 257, 6, "ustar");
    put_test_tar_string(header, 263, 2, "00");
    std::uint32_t checksum = 0;
    for (const auto ch : header) {
        checksum += static_cast<unsigned char>(ch);
    }
    std::array<char, 8> encoded{};
    std::snprintf(encoded.data(), encoded.size(), "%06o", checksum);
    std::copy(encoded.begin(), encoded.begin() + 6, header.begin() + 148);
    header[154] = '\0';
    header[155] = ' ';
    return header;
}

// Purpose: Write a one-entry uncompressed TAR archive fixture.
// Inputs: `archive` is the output TAR path, `path` is the entry name, `typeflag` is the TAR type, and `payload` is file or link payload data.
// Outputs: Creates a complete TAR stream with end markers.
void write_one_entry_tar(
    const std::filesystem::path& archive,
    std::string_view path,
    char typeflag,
    std::string_view payload) {
    std::ofstream output(archive, std::ios::binary);
    const auto header = make_test_tar_header(path, typeflag, typeflag == '5' ? 0 : payload.size());
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (typeflag != '5') {
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        const auto padding = (512U - (payload.size() % 512U)) % 512U;
        std::array<char, 512> zero{};
        output.write(zero.data(), static_cast<std::streamsize>(padding));
    }
    std::array<char, 512> zero{};
    output.write(zero.data(), static_cast<std::streamsize>(zero.size()));
    output.write(zero.data(), static_cast<std::streamsize>(zero.size()));
}

// Purpose: Read a full file as binary bytes for corruption tests.
// Inputs: `path` is the file to read.
// Outputs: Returns the complete file payload.
std::vector<unsigned char> read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Purpose: Write exact binary bytes to disk for corruption tests.
// Inputs: `path` is the output file and `bytes` is the exact payload.
// Outputs: Creates or replaces the file.
void write_binary_file(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Purpose: Read a full text file for roundtrip equality checks.
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

// Purpose: Verify in-process `.tar.gz` compatibility roundtrip for files and directories.
// Inputs: A temporary source tree compressed through the streaming TAR+Gzip adapter.
// Outputs: Throws if extraction fails, format detection is wrong, or restored contents differ.
TEST_CASE(tar_gzip_roundtrip_extracts_files_and_directories) {
    const auto root = test_temp_dir("tar-gzip-roundtrip");
    const auto input = root / "input";
    std::filesystem::create_directories(input / "dir");
    {
        std::ofstream(input / "dir" / "hello.txt") << "hello from tar.gz\n";
        std::ofstream(input / "root.txt") << std::string(128 * 1024, 'r');
    }

    const auto archive = root / "sample.tar.gz";
    const auto compress_stats = superzip::compress_tar_gzip({input}, archive);
    REQUIRE_TRUE(compress_stats.entries >= 3);
    REQUIRE_TRUE(compress_stats.output_bytes > 0);
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::TarGzip);

    const auto output = root / "output";
    const auto extract_stats = superzip::extract_tar_gzip(archive, output, false);
    REQUIRE_EQ(extract_stats.output_bytes, compress_stats.input_bytes);
    REQUIRE_EQ(read_text_file(output / "input" / "dir" / "hello.txt"), read_text_file(input / "dir" / "hello.txt"));
    REQUIRE_EQ(std::filesystem::file_size(output / "input" / "root.txt"), static_cast<std::uintmax_t>(128 * 1024));
}

// Purpose: Verify `.tar.gz` extraction rejects traversal metadata during the validation pass.
// Inputs: A handcrafted TAR containing `../escape.txt`, then compressed as Gzip.
// Outputs: Throws if extraction accepts the unsafe path or writes any output.
TEST_CASE(tar_gzip_extraction_rejects_traversal_before_output) {
    const auto root = test_temp_dir("tar-gzip-traversal");
    const auto tar = root / "bad.tar";
    const auto archive = root / "bad.tar.gz";
    write_one_entry_tar(tar, "../escape.txt", '0', "bad");
    (void)superzip::compress_gzip_file(tar, archive);

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_tar_gzip(archive, root / "out", false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify `.tar.gz` extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A compressed TAR with one file and a preexisting output file of the same name.
// Outputs: Throws if extraction overwrites while `overwrite` is false.
TEST_CASE(tar_gzip_extraction_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("tar-gzip-overwrite");
    const auto tar = root / "overwrite.tar";
    const auto archive = root / "overwrite.tgz";
    write_one_entry_tar(tar, "file.txt", '0', "new");
    (void)superzip::compress_gzip_file(tar, archive);
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    std::ofstream(output / "file.txt") << "old";

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_tar_gzip(archive, output, false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "file.txt"), "old");
}

// Purpose: Verify corrupted Gzip wrappers prevent `.tar.gz` extraction output.
// Inputs: A valid `.tar.gz` archive with a corrupted Gzip trailer CRC byte.
// Outputs: Throws if first-pass validation succeeds or any output file is published.
TEST_CASE(tar_gzip_rejects_gzip_crc_mismatch_before_output) {
    const auto root = test_temp_dir("tar-gzip-crc");
    const auto input = root / "input";
    std::filesystem::create_directories(input);
    std::ofstream(input / "payload.txt") << "payload";
    const auto archive = root / "payload.tar.gz";
    (void)superzip::compress_tar_gzip({input}, archive);

    auto bytes = read_binary_file(archive);
    REQUIRE_TRUE(bytes.size() > 18U);
    bytes[bytes.size() - 8U] ^= 0xFFU;
    write_binary_file(archive, bytes);

    const auto output = root / "out";
    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_tar_gzip(archive, output, true));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}
