#include "test_util.hpp"

#include "base64/base64_adapter.hpp"
#include "core/archive_format.hpp"
#include "core/result.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

// Purpose: Write exact binary bytes to disk for Base64 tests.
// Inputs: `path` is the output file and `bytes` is the exact payload.
// Outputs: Creates or replaces the file.
void write_binary_file(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Purpose: Write exact text bytes to disk for handcrafted Base64 fixtures.
// Inputs: `path` is the output file and `text` is the payload.
// Outputs: Creates or replaces the file.
void write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// Purpose: Read a full file as unsigned binary bytes for equality checks.
// Inputs: `path` is the file to read.
// Outputs: Returns the complete file payload.
std::vector<unsigned char> read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<unsigned char>(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
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

// Purpose: Verify native Base64 compatibility roundtrip for a binary regular file.
// Inputs: A temporary source file encoded through the in-process Base64 adapter.
// Outputs: Throws if extraction fails, format detection is wrong, or restored bytes differ.
TEST_CASE(base64_compat_roundtrip) {
    const auto root = test_temp_dir("base64-compat");
    const auto input = root / "payload.bin";
    std::vector<unsigned char> payload(777);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<unsigned char>((i * 19U + 5U) & 0xFFU);
    }
    write_binary_file(input, payload);

    const auto archive = root / "payload.b64";
    const auto compress_stats = superzip::compress_base64_file(input, archive);
    REQUIRE_TRUE(compress_stats.output_bytes > payload.size());
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Base64);

    const auto output = root / "out";
    const auto extract_stats = superzip::extract_base64_file(archive, output, false);
    REQUIRE_EQ(extract_stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(read_binary_file(output / "payload.bin"), payload);
    std::filesystem::remove_all(root);
}

// Purpose: Verify raw `.b64` payloads without a wrapper derive a safe output filename.
// Inputs: A raw RFC-style Base64 payload with line whitespace.
// Outputs: Throws if raw extraction fails or produces the wrong derived file.
TEST_CASE(base64_extracts_raw_payload_by_extension) {
    const auto root = test_temp_dir("base64-raw");
    const auto archive = root / "hello.txt.b64";
    write_text_file(archive, "U3VwZXJa\naXAgcmF3\nIGI2NA==\n");

    const auto output = root / "out";
    const auto stats = superzip::extract_base64_file(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(read_binary_file(output / "hello.txt"), std::vector<unsigned char>({
        'S', 'u', 'p', 'e', 'r', 'Z', 'i', 'p', ' ', 'r', 'a', 'w', ' ', 'b', '6', '4',
    }));
    std::filesystem::remove_all(root);
}

// Purpose: Verify Base64 extraction rejects hostile embedded output paths before publishing.
// Inputs: A handcrafted wrapped Base64 fixture whose begin line attempts traversal.
// Outputs: Throws if extraction succeeds or writes outside the destination.
TEST_CASE(base64_extract_rejects_unsafe_header_path) {
    const auto root = test_temp_dir("base64-unsafe-path");
    const auto archive = root / "unsafe.b64";
    write_text_file(archive, "begin-base64 644 ..\\escape.txt\nU2FmZQ==\n====\n");

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_base64_file(archive, output, false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify malformed Base64 payloads do not publish partial outputs.
// Inputs: A wrapped stream with valid safe header and invalid payload after padding.
// Outputs: Throws if extraction succeeds or leaves the final output file behind.
TEST_CASE(base64_extract_rejects_malformed_payload_without_output) {
    const auto root = test_temp_dir("base64-malformed-payload");
    const auto archive = root / "bad.b64";
    write_text_file(archive, "begin-base64 644 payload.txt\nU2FmZQ==QQ==\n====\n");

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_base64_file(archive, output, true);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(output / "payload.txt"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify Base64 extraction refuses to overwrite existing files by default.
// Inputs: A valid Base64 archive extracted over an existing destination file.
// Outputs: Throws if overwrite refusal is not enforced.
TEST_CASE(base64_extract_rejects_overwrite_by_default) {
    const auto root = test_temp_dir("base64-overwrite");
    const auto input = root / "sample.txt";
    write_text_file(input, "new payload");
    const auto archive = root / "sample.b64";
    (void)superzip::compress_base64_file(input, archive);

    const auto output = root / "out";
    std::filesystem::create_directories(output);
    write_text_file(output / "sample.txt", "old payload");

    bool rejected = false;
    try {
        (void)superzip::extract_base64_file(archive, output, false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_binary_file(output / "sample.txt"), std::vector<unsigned char>({
        'o', 'l', 'd', ' ', 'p', 'a', 'y', 'l', 'o', 'a', 'd',
    }));
    std::filesystem::remove_all(root);
}
