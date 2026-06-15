#include "test_util.hpp"

#include "core/result.hpp"
#include "tar/tar_adapter.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string_view>

namespace {

// Purpose: Write a fixed-size TAR string field for handcrafted fixtures.
// Inputs: `header` is the mutable block, `offset`/`length` select the field, and `value` is ASCII metadata.
// Outputs: Copies `value` into the field; throws through the test harness if it cannot fit.
void put_test_tar_string(std::array<char, 512>& header, std::size_t offset, std::size_t length, std::string_view value) {
    REQUIRE_TRUE(value.size() <= length);
    std::copy(value.begin(), value.end(), header.begin() + static_cast<std::ptrdiff_t>(offset));
}

// Purpose: Write a simple octal TAR numeric field for small handcrafted fixtures.
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

// Purpose: Create a small TAR header for parser-security tests.
// Inputs: `path` is the archive entry path, `typeflag` is the TAR entry kind, and `size` is payload length.
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

// Purpose: Write a one-entry TAR archive fixture.
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

}  // namespace

TEST_CASE(tar_roundtrip_extracts_files_and_directories) {
    const auto root = test_temp_dir("tar-roundtrip");
    const auto input = root / "input";
    std::filesystem::create_directories(input / "dir");
    {
        std::ofstream(input / "dir" / "hello.txt") << "hello from tar\n";
        std::ofstream(input / "root.txt") << "root file\n";
    }
    const auto archive = root / "sample.tar";
    const auto compress_stats = superzip::compress_tar({input}, archive);
    REQUIRE_TRUE(compress_stats.entries >= 3);
    REQUIRE_TRUE(compress_stats.output_bytes > compress_stats.input_bytes);

    const auto output = root / "output";
    const auto extract_stats = superzip::extract_tar(archive, output, false);
    REQUIRE_EQ(extract_stats.output_bytes, compress_stats.input_bytes);
    REQUIRE_TRUE(std::filesystem::exists(output / "input" / "dir" / "hello.txt"));
    REQUIRE_TRUE(std::filesystem::exists(output / "input" / "root.txt"));
}

TEST_CASE(tar_extraction_rejects_traversal_before_output) {
    const auto root = test_temp_dir("tar-traversal");
    const auto archive = root / "bad.tar";
    write_one_entry_tar(archive, "../escape.txt", '0', "bad");
    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_tar(archive, root / "out", false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
}

TEST_CASE(tar_extraction_rejects_link_entries) {
    const auto root = test_temp_dir("tar-link");
    const auto archive = root / "link.tar";
    write_one_entry_tar(archive, "link", '2', "target");
    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_tar(archive, root / "out", false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

TEST_CASE(tar_extraction_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("tar-overwrite");
    const auto archive = root / "overwrite.tar";
    write_one_entry_tar(archive, "file.txt", '0', "new");
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    std::ofstream(output / "file.txt") << "old";

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_tar(archive, output, false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}
