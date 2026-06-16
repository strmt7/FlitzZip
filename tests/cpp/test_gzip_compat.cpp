#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "gzip/gzip_adapter.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

// Purpose: Write an exact text fixture to disk for Gzip roundtrip tests.
// Inputs: `path` is the destination file and `text` is the payload.
// Outputs: Creates or replaces the file.
void write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// Purpose: Read a full file as text bytes for equality checks.
// Inputs: `path` is the file to read.
// Outputs: Returns the complete file payload.
std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Purpose: Read a full file as unsigned bytes for handcrafted Gzip metadata tests.
// Inputs: `path` is the file to read.
// Outputs: Returns the complete file payload.
std::vector<unsigned char> read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<unsigned char> bytes;
    char value = 0;
    while (input.get(value)) {
        bytes.push_back(static_cast<unsigned char>(value));
    }
    return bytes;
}

// Purpose: Write unsigned binary bytes to disk for handcrafted Gzip metadata tests.
// Inputs: `path` is the output file and `bytes` is the exact payload.
// Outputs: Creates or replaces the file.
void write_binary_file(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
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

// Purpose: Verify native Gzip compatibility roundtrip for a regular file.
// Inputs: A temporary source file compressed through the miniz raw-deflate adapter.
// Outputs: Throws if extraction fails, format detection is wrong, or restored contents differ.
TEST_CASE(gzip_compat_roundtrip) {
    const auto root = test_temp_dir("gzip-compat");
    const auto input = root / "hello.txt";
    const std::string payload = "hello gzip\n" + std::string(128 * 1024, 'x');
    write_text_file(input, payload);

    const auto archive = root / "hello.txt.gz";
    const auto compress_stats = superzip::compress_gzip_file(input, archive);
    REQUIRE_TRUE(compress_stats.output_bytes > 0);
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Gzip);

    const auto output = root / "out";
    const auto extract_stats = superzip::extract_gzip_file(archive, output, false);
    REQUIRE_EQ(extract_stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(read_text_file(output / "hello.txt"), payload);
    std::filesystem::remove_all(root);
}

// Purpose: Verify Gzip compatibility enforces single-file source semantics.
// Inputs: Two temporary files passed as one Gzip source set.
// Outputs: Throws if Gzip compression accepts multiple sources.
TEST_CASE(gzip_compress_rejects_multiple_sources) {
    const auto root = test_temp_dir("gzip-multiple-sources");
    const auto first = root / "first.txt";
    const auto second = root / "second.txt";
    write_text_file(first, "first");
    write_text_file(second, "second");

    bool rejected = false;
    try {
        (void)superzip::compress_gzip({first, second}, root / "multi.gz");
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify Gzip CRC failures do not publish partial output.
// Inputs: A valid Gzip stream whose trailer CRC is corrupted.
// Outputs: Throws if extraction succeeds or leaves a final output file.
TEST_CASE(gzip_extract_rejects_crc_mismatch_without_output) {
    const auto root = test_temp_dir("gzip-crc");
    const auto input = root / "payload.bin";
    write_text_file(input, std::string(96 * 1024, 'c'));
    const auto archive = root / "payload.bin.gz";
    (void)superzip::compress_gzip_file(input, archive);

    auto bytes = read_binary_file(archive);
    REQUIRE_TRUE(bytes.size() > 18U);
    bytes[bytes.size() - 8U] ^= 0xFFU;
    write_binary_file(archive, bytes);

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_gzip_file(archive, output, true);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(output / "payload.bin"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify Gzip original-name metadata is not trusted as an extraction path.
// Inputs: A valid Gzip payload rewritten with hostile FNAME metadata.
// Outputs: Extracts to the archive-derived filename and does not create the hostile path.
TEST_CASE(gzip_extract_ignores_embedded_original_name) {
    const auto root = test_temp_dir("gzip-fname");
    const auto input = root / "safe";
    const std::string payload = "gzip embedded names are metadata only";
    write_text_file(input, payload);
    const auto original_archive = root / "original.gz";
    (void)superzip::compress_gzip_file(input, original_archive);

    const auto original = read_binary_file(original_archive);
    REQUIRE_TRUE(original.size() > 18U);
    std::vector<unsigned char> modified;
    modified.reserve(original.size() + 16U);
    modified.insert(modified.end(), original.begin(), original.begin() + 10);
    modified[3] |= 0x08U;
    const std::string hostile_name = "../escape.txt";
    modified.insert(modified.end(), hostile_name.begin(), hostile_name.end());
    modified.push_back(0U);
    modified.insert(modified.end(), original.begin() + 10, original.end());

    const auto archive = root / "safe.gz";
    write_binary_file(archive, modified);
    const auto output = root / "out";
    (void)superzip::extract_gzip_file(archive, output, false);
    REQUIRE_EQ(read_text_file(output / "safe"), payload);
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
    std::filesystem::remove_all(root);
}
