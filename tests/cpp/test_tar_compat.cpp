#include "test_util.hpp"

#include "core/result.hpp"
#include "tar/tar_adapter.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <string_view>

namespace {

// Purpose: Write a fixed-size TAR string field for handcrafted fixtures.
// Inputs: `header` is the mutable block, `offset`/`length` select the field, and `value` is ASCII metadata.
// Outputs: Copies `value` into the field; throws through the test harness if it cannot fit.
void put_test_tar_string(std::array<char, 512>& header, std::size_t offset, std::size_t length,
                         std::string_view value) {
    REQUIRE_TRUE(value.size() <= length);
    std::copy(value.begin(), value.end(), header.begin() + static_cast<std::ptrdiff_t>(offset));
}

// Purpose: Write a simple octal TAR numeric field for small handcrafted fixtures.
// Inputs: `header` is the mutable block, `offset`/`length` select the field, and `value` is the small integer to
// encode. Outputs: Encodes a NUL-terminated octal field.
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
    put_test_tar_octal(header, 148, 7, checksum);
    header[155] = ' ';
    return header;
}

// Purpose: Write a one-entry TAR archive fixture.
// Inputs: `archive` is the output TAR path, `path` is the entry name, `typeflag` is the TAR type, and `payload` is file
// or link payload data. Outputs: Creates a complete TAR stream with end markers.
void write_one_entry_tar(const std::filesystem::path& archive, std::string_view path, char typeflag,
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

namespace {

// Purpose: Serialize metadata and file records with independent TAR block padding.
// Inputs: An owned output stream, fixture header and bounded payload. Outputs: Complete padded record.
void append_tar_fixture_record(std::ostream& output, const std::array<char, 512>& header,
                               std::string_view payload = {}) {
    output.write(header.data(), header.size());
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    const std::array<char, 512> zero{};
    output.write(zero.data(), static_cast<std::streamsize>((512U - payload.size() % 512U) % 512U));
    REQUIRE_TRUE(output.good());
}

// Purpose: Test rejection before publication while retaining an unrelated existing destination file.
// Inputs: One malformed TAR and its owned destination. Outputs: Product error with no new paths or changed bytes.
void require_tar_fixture_rejected(const std::filesystem::path& archive, const std::filesystem::path& destination) {
    std::filesystem::create_directories(destination);
    const auto sentinel = destination / "existing.txt";
    {
        std::ofstream output(sentinel);
        output << "retain";
    }
    bool rejected = false;
    try {
        (void)flitzzip::extract_tar(archive, destination, false);
    } catch (const flitzzip::Error&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(destination)) {
        REQUIRE_EQ(entry.path(), sentinel);
        ++entries;
    }
    REQUIRE_EQ(entries, 1U);
    std::ifstream input(sentinel);
    std::string content;
    input >> content;
    REQUIRE_EQ(content, "retain");
}

// Purpose: Recalculate only a fixture's checksum after independently mutating a wire field.
// Inputs: A complete mutable 512-byte header. Outputs: Valid checksum for the intentionally malformed metadata.
void checksum_tar_fixture(std::array<char, 512>& header) {
    std::fill(header.begin() + 148, header.begin() + 156, ' ');
    std::uint32_t sum = 0;
    for (const unsigned char value : header) {
        sum += value;
    }
    put_test_tar_octal(header, 148, 7, sum);
    header[155] = ' ';
}

}  // namespace

// Purpose: Reject malformed PAX framing and ambiguous path policy before any destination publication.
// Inputs: Independently authored lengths, terminators, duplicate/empty paths and unsupported character sets.
// Outputs: Every invalid metadata record fails while retaining an existing destination file.
TEST_CASE(tar_pax_malformed_records_do_not_publish) {
    const auto root = test_temp_dir("tar-pax-malformed");
    const auto archive = root / "input.tar";
    const std::array<std::string_view, 8> metadata{"missing-space", "x path=a\n",
                                                   "0 path=a\n",    "99 path=a\n",
                                                   "10 path=a!",    "10 path=a\n10 path=b\n",
                                                   "8 path=\n",     "22 hdrcharset=UNKNOWN\n"};
    for (const auto value : metadata) {
        std::ofstream output(archive, std::ios::binary | std::ios::trunc);
        append_tar_fixture_record(output, make_test_tar_header("PaxHeader", 'x', value.size()), value);
        append_tar_fixture_record(output, make_test_tar_header("payload", '0', 1U), "x");
        output.close();
        require_tar_fixture_rejected(archive, root / "output");
    }
    std::filesystem::remove_all(root);
}

// Purpose: Keep one-entry PAX and GNU path overrides from leaking into later entries.
// Inputs: A metadata record, overridden empty file and a second ordinary empty file.
// Outputs: Only the first file takes the override, with exact expected destination entries.
TEST_CASE(tar_path_extensions_apply_once) {
    const auto root = test_temp_dir("tar-path-one-shot");
    for (const char type : {'x', 'L'}) {
        const auto archive = root / "input.tar";
        const std::string metadata = type == 'x' ? "17 path=long.txt\n" : std::string("long.txt\0", 9U);
        std::ofstream output(archive, std::ios::binary | std::ios::trunc);
        append_tar_fixture_record(output, make_test_tar_header("metadata", type, metadata.size()), metadata);
        append_tar_fixture_record(output, make_test_tar_header("ignored.txt", '0', 0U));
        append_tar_fixture_record(output, make_test_tar_header("second.txt", '0', 0U));
        output.close();
        const auto destination = root / std::string(1U, type);
        const auto stats = flitzzip::extract_tar(archive, destination, false);
        REQUIRE_EQ(stats.output_bytes, 0U);
        REQUIRE_TRUE(std::filesystem::is_regular_file(destination / "long.txt"));
        REQUIRE_TRUE(std::filesystem::is_regular_file(destination / "second.txt"));
        REQUIRE_TRUE(!std::filesystem::exists(destination / "ignored.txt"));
    }
    std::filesystem::remove_all(root);
}

// Purpose: Validate base-256 sizes independently of the writer and reject hostile numeric representations.
// Inputs: One-byte positive base-256 size, overflowing/negative encodings, invalid octal and a corrupt checksum.
// Outputs: Valid bytes restore exactly; malformed headers leave the destination untouched.
TEST_CASE(tar_numeric_headers_are_bounded_and_checksummed) {
    const auto root = test_temp_dir("tar-numeric-headers");
    const auto archive = root / "input.tar";
    auto positive = make_test_tar_header("payload", '0', 0U);
    std::fill(positive.begin() + 124, positive.begin() + 136, '\0');
    positive[124] = static_cast<char>(0x80U);
    positive[135] = 1;
    checksum_tar_fixture(positive);
    {
        std::ofstream output(archive, std::ios::binary);
        append_tar_fixture_record(output, positive, "z");
    }
    const auto stats = flitzzip::extract_tar(archive, root / "valid", false);
    REQUIRE_EQ(stats.output_bytes, 1U);
    {
        std::ifstream input(root / "valid/payload");
        REQUIRE_EQ(input.get(), 'z');
        REQUIRE_EQ(input.get(), EOF);
    }
    for (int mutation = 0; mutation < 4; ++mutation) {
        auto invalid = positive;
        if (mutation == 0) {
            invalid[124] = static_cast<char>(0xFFU);
        }
        if (mutation == 1) {
            invalid[125] = 1;
        }
        if (mutation == 2) {
            invalid[124] = '8';
        }
        checksum_tar_fixture(invalid);
        if (mutation == 3) {
            invalid[0] ^= 1;
        }
        {
            std::ofstream output(archive, std::ios::binary | std::ios::trunc);
            append_tar_fixture_record(output, invalid, "z");
        }
        require_tar_fixture_rejected(archive, root / "rejected");
    }
    std::filesystem::remove_all(root);
}

// Purpose: Prevent unsupported TAR entry types and empty GNU names from creating filesystem objects.
// Inputs: Link, device, FIFO and unsupported type flags, plus empty path-override metadata.
// Outputs: All cases reject before publication and preserve existing files.
TEST_CASE(tar_special_entries_and_empty_long_names_fail_closed) {
    const auto root = test_temp_dir("tar-special-types");
    const auto archive = root / "input.tar";
    for (const char type : {'1', '2', '3', '4', '6', 'K', 'V', 'L'}) {
        {
            std::ofstream output(archive, std::ios::binary | std::ios::trunc);
            append_tar_fixture_record(output, make_test_tar_header("payload", type, 0U));
        }
        require_tar_fixture_rejected(archive, root / "output");
    }
    std::filesystem::remove_all(root);
}

// Purpose: Check the production checksum's exact wire width and terminators without reusing its formatter.
// Inputs: A created TAR archive with a normal binary file; the checksum oracle sums bytes with a blanked field.
// Outputs: Requires six ASCII octal digits, NUL and space, and an independently reconstructed checksum.
TEST_CASE(tar_writer_checksum_preserves_exact_wire_encoding) {
    const auto root = test_temp_dir("tar-checksum-wire");
    const auto source = root / "payload.bin";
    {
        std::ofstream output(source, std::ios::binary);
        for (unsigned i = 0; i < 256U; ++i) {
            output.put(static_cast<char>(i));
        }
    }
    const auto archive = root / "output.tar";
    (void)flitzzip::compress_tar({source}, archive);
    {
        std::ifstream input(archive, std::ios::binary);
        std::array<char, 512> header{};
        input.read(header.data(), static_cast<std::streamsize>(header.size()));
        REQUIRE_EQ(input.gcount(), static_cast<std::streamsize>(header.size()));
        std::uint32_t encoded = 0;
        for (std::size_t i = 148U; i < 154U; ++i) {
            REQUIRE_TRUE(header[i] >= '0' && header[i] <= '7');
            encoded = encoded * 8U + static_cast<unsigned>(header[i] - '0');
        }
        REQUIRE_EQ(header[154], '\0');
        REQUIRE_EQ(header[155], ' ');
        std::uint32_t expected = 0;
        for (std::size_t i = 0; i < header.size(); ++i) {
            expected += i >= 148U && i < 156U ? 32U : static_cast<unsigned char>(header[i]);
        }
        REQUIRE_EQ(encoded, expected);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Preserve declared PAX encodings while leaving unmarked legacy TAR names unchanged.
// Inputs: An independently serialized PAX record with UTF-8 or BINARY names and an unmarked USTAR control.
// Outputs: Requires exact filename restoration for each declared character set.
TEST_CASE(tar_pax_filename_encoding_contract) {
    const auto root = test_temp_dir("tar-pax-encoding");
    const std::string name = "caf\xC3\xA9.txt";
    for (const auto* charset : {"", "BINARY", "ISO-IR 10646 2000 UTF-8", "legacy"}) {
        const auto archive = root / "input.tar";
        const std::string charset_name(charset);
        std::ofstream output(archive, std::ios::binary);
        if (charset_name != "legacy") {
            const std::string metadata = (charset_name.empty()       ? ""
                                          : charset_name == "BINARY" ? "21 hdrcharset=BINARY\n"
                                                                     : "38 hdrcharset=ISO-IR 10646 2000 UTF-8\n") +
                                         std::string("18 path=") + name + "\n";
            const auto header = make_test_tar_header("PaxHeaders/name", 'x', metadata.size());
            output.write(header.data(), header.size());
            output.write(metadata.data(), static_cast<std::streamsize>(metadata.size()));
            const std::array<char, 512> zero{};
            output.write(zero.data(), static_cast<std::streamsize>((512U - metadata.size() % 512U) % 512U));
        }
        const auto header = make_test_tar_header(charset_name == "legacy" ? name : "fallback", '0', 0);
        output.write(header.data(), header.size());
        const std::array<char, 1024> footer{};
        output.write(footer.data(), footer.size());
        output.close();
        const auto destination = root / (charset_name.empty() ? "default" : charset_name);
        (void)flitzzip::extract_tar(archive, destination, false);
        const auto expected = charset_name == "BINARY" || charset_name == "legacy"
                                  ? std::filesystem::path(name)
                                  : std::filesystem::path(u8"caf\u00e9.txt");
        REQUIRE_TRUE(std::filesystem::is_regular_file(destination / expected));
    }
    std::filesystem::remove_all(root);
}

TEST_CASE(tar_roundtrip_extracts_files_and_directories) {
    const auto root = test_temp_dir("tar-roundtrip");
    const auto input = root / "input";
    std::filesystem::create_directories(input / "dir");
    {
        std::ofstream(input / "dir" / "hello.txt") << "hello from tar\n";
        std::ofstream(input / "root.txt") << "root file\n";
    }
    const auto archive = root / "sample.tar";
    const auto compress_stats = flitzzip::compress_tar({input}, archive);
    REQUIRE_TRUE(compress_stats.entries >= 3);
    REQUIRE_TRUE(compress_stats.output_bytes > compress_stats.input_bytes);

    const auto output = root / "output";
    const auto extract_stats = flitzzip::extract_tar(archive, output, false);
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
        static_cast<void>(flitzzip::extract_tar(archive, root / "out", false));
    } catch (const flitzzip::SecurityError&) {
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
        static_cast<void>(flitzzip::extract_tar(archive, root / "out", false));
    } catch (const flitzzip::SecurityError&) {
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
        static_cast<void>(flitzzip::extract_tar(archive, output, false));
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}
