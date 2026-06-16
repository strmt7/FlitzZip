#include "test_util.hpp"

#include "ar/ar_adapter.hpp"
#include "core/archive_format.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

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

// Purpose: Create one fixed-width AR header field.
// Inputs: `header` is the mutable header, `offset`/`width` select the field, and `value` is ASCII metadata.
// Outputs: Copies the value into a space-padded field.
void put_ar_field(std::array<char, 60>& header, std::size_t offset, std::size_t width, std::string_view value) {
    REQUIRE_TRUE(value.size() <= width);
    std::copy(value.begin(), value.end(), header.begin() + static_cast<std::ptrdiff_t>(offset));
}

// Purpose: Build one AR member header for handcrafted fixtures.
// Inputs: `name` is the raw 16-byte name field and `size` is the declared member size.
// Outputs: Returns a complete 60-byte AR header.
std::array<char, 60> make_ar_header(std::string_view name, std::uint64_t size) {
    std::array<char, 60> header{};
    std::fill(header.begin(), header.end(), ' ');
    put_ar_field(header, 0, 16, name);
    put_ar_field(header, 16, 12, "0");
    put_ar_field(header, 28, 6, "0");
    put_ar_field(header, 34, 6, "0");
    put_ar_field(header, 40, 8, "100644");
    put_ar_field(header, 48, 10, std::to_string(size));
    header[58] = '`';
    header[59] = '\n';
    return header;
}

// Purpose: Write one AR member with a raw header name.
// Inputs: `output` is the archive stream, `name` is the header name, and `payload` is the member payload.
// Outputs: Appends the member and even-byte padding when needed.
void write_ar_member(std::ofstream& output, std::string_view name, std::string_view payload) {
    const auto header = make_ar_header(name, payload.size());
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    if ((payload.size() % 2U) != 0) {
        output.put('\n');
    }
}

// Purpose: Write one BSD long-name AR member for parser-security tests.
// Inputs: `output` is the archive stream, `path` is the embedded filename, and `payload` is the file data.
// Outputs: Appends a complete `#1/<len>` member.
void write_bsd_ar_member(std::ofstream& output, std::string_view path, std::string_view payload) {
    const auto header = make_ar_header("#1/" + std::to_string(path.size()), path.size() + payload.size());
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    output.write(path.data(), static_cast<std::streamsize>(path.size()));
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (((path.size() + payload.size()) % 2U) != 0) {
        output.put('\n');
    }
}

// Purpose: Write one complete AR archive with a single BSD long-name member.
// Inputs: `archive` is the output path, `path` is the embedded filename, and `payload` is the file data.
// Outputs: Creates a complete AR file.
void write_one_bsd_ar_archive(const std::filesystem::path& archive, std::string_view path, std::string_view payload) {
    std::ofstream output(archive, std::ios::binary);
    output.write("!<arch>\n", 8);
    write_bsd_ar_member(output, path, payload);
}

// Purpose: Write an AR archive that uses the GNU `//` long-name string table.
// Inputs: `archive` is the output path.
// Outputs: Creates a complete AR file with one extractable GNU long-name member.
void write_gnu_long_name_ar_archive(const std::filesystem::path& archive) {
    std::ofstream output(archive, std::ios::binary);
    output.write("!<arch>\n", 8);
    const std::string table = "dir/gnu-long-name.txt/\n";
    write_ar_member(output, "//", table);
    write_ar_member(output, "/0", "gnu payload");
}

}  // namespace

// Purpose: Verify native `.ar` compatibility roundtrip for files in a directory tree.
// Inputs: A temporary source tree compressed through the AR adapter.
// Outputs: Throws if extraction fails, format detection is wrong, or restored contents differ.
TEST_CASE(ar_roundtrip_extracts_files_from_directories) {
    const auto root = test_temp_dir("ar-roundtrip");
    const auto input = root / "input";
    std::filesystem::create_directories(input / "dir");
    {
        std::ofstream(input / "dir" / "hello.txt") << "hello from ar\n";
        std::ofstream(input / "root.txt") << "root file\n";
    }

    const auto archive = root / "sample.ar";
    const auto compress_stats = superzip::compress_ar({input}, archive);
    REQUIRE_EQ(compress_stats.entries, static_cast<std::uint64_t>(2));
    REQUIRE_TRUE(compress_stats.output_bytes > compress_stats.input_bytes);
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Ar);

    const auto output = root / "output";
    const auto extract_stats = superzip::extract_ar(archive, output, false);
    REQUIRE_EQ(extract_stats.output_bytes, compress_stats.input_bytes);
    REQUIRE_EQ(read_text_file(output / "input" / "dir" / "hello.txt"), read_text_file(input / "dir" / "hello.txt"));
    REQUIRE_EQ(read_text_file(output / "input" / "root.txt"), read_text_file(input / "root.txt"));
}

// Purpose: Verify GNU string-table AR names are resolved during extraction.
// Inputs: A handcrafted AR archive containing a `//` string table and `/0` member reference.
// Outputs: Throws if the long-name member is not restored at the expected path.
TEST_CASE(ar_extracts_gnu_string_table_names) {
    const auto root = test_temp_dir("ar-gnu-long-name");
    const auto archive = root / "gnu.ar";
    write_gnu_long_name_ar_archive(archive);

    const auto output = root / "out";
    const auto stats = superzip::extract_ar(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(read_text_file(output / "dir" / "gnu-long-name.txt"), "gnu payload");
}

// Purpose: Verify `.deb` files use the native AR extraction path for outer members.
// Inputs: A Debian-package-named AR container with one regular member.
// Outputs: Throws if detection, registry support, or extraction routing assumptions regress.
TEST_CASE(deb_outer_container_extracts_with_native_ar_adapter) {
    const auto root = test_temp_dir("deb-outer-container");
    const auto archive = root / "package.deb";
    write_one_bsd_ar_archive(archive, "debian-binary", "2.0\n");

    const auto info = superzip::archive_format_info(superzip::ArchiveFormat::Deb);
    REQUIRE_TRUE(info.can_extract);
    REQUIRE_TRUE(!info.can_create);
    REQUIRE_TRUE(info.bundled_native);
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Deb);

    const auto output = root / "out";
    const auto stats = superzip::extract_ar(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(read_text_file(output / "debian-binary"), "2.0\n");
}

// Purpose: Verify AR extraction rejects traversal metadata during the validation pass.
// Inputs: A handcrafted BSD long-name AR member containing `../escape.txt`.
// Outputs: Throws if extraction accepts the unsafe path or writes any output.
TEST_CASE(ar_extraction_rejects_traversal_before_output) {
    const auto root = test_temp_dir("ar-traversal");
    const auto archive = root / "bad.ar";
    write_one_bsd_ar_archive(archive, "../escape.txt", "bad");

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_ar(archive, root / "out", false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify AR scanning rejects declared member extents past EOF before creating output.
// Inputs: A handcrafted archive whose header declares more bytes than the file contains.
// Outputs: Throws `ArchiveError` and leaves the extraction root without regular files.
TEST_CASE(ar_extraction_rejects_truncated_member_before_output) {
    const auto root = test_temp_dir("ar-truncated-member");
    const auto archive = root / "truncated.ar";
    {
        std::ofstream output(archive, std::ios::binary);
        output.write("!<arch>\n", 8);
        const auto header = make_ar_header("file.txt/", 100);
        output.write(header.data(), static_cast<std::streamsize>(header.size()));
        output.write("tiny", 4);
    }

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_ar(archive, root / "out", false));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify AR extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: An AR file entry and a preexisting output file of the same name.
// Outputs: Throws if extraction overwrites while `overwrite` is false.
TEST_CASE(ar_extraction_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("ar-overwrite");
    const auto archive = root / "overwrite.ar";
    write_one_bsd_ar_archive(archive, "file.txt", "new");
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    std::ofstream(output / "file.txt") << "old";

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_ar(archive, output, false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "file.txt"), "old");
}
