#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "xxe/xxe_adapter.hpp"

#include <fstream>
#include <iterator>
#include <cstdint>
#include <string>
#include <vector>

namespace {

// Purpose: Write exact binary bytes to disk for XXE tests.
// Inputs: `path` is the output file and `bytes` is the exact payload.
// Outputs: Creates or replaces the file.
void write_binary_file(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Purpose: Write exact text bytes to disk for handcrafted XXE fixtures.
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

// Purpose: Verify native XXEncode compatibility roundtrip for a binary regular file.
// Inputs: A temporary source file encoded through the in-process XXE adapter.
// Outputs: Throws if extraction fails, format detection is wrong, or restored bytes differ.
TEST_CASE(xxe_compat_roundtrip) {
    const auto root = test_temp_dir("xxe-compat");
    const auto input = root / "payload.bin";
    std::vector<unsigned char> payload(513);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<unsigned char>((i * 41U + 17U) & 0xFFU);
    }
    write_binary_file(input, payload);

    const auto archive = root / "payload.xxe";
    const auto compress_stats = superzip::compress_xxe_file(input, archive);
    REQUIRE_TRUE(compress_stats.output_bytes > payload.size());
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Xxe);

    const auto output = root / "out";
    const auto extract_stats = superzip::extract_xxe_file(archive, output, false);
    REQUIRE_EQ(extract_stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(read_binary_file(output / "payload.bin"), payload);
    std::filesystem::remove_all(root);
}

// Purpose: Verify XXEncode compatibility enforces single-file source semantics.
// Inputs: Two temporary files passed as one XXE source set.
// Outputs: Throws if XXE compression accepts multiple sources.
TEST_CASE(xxe_compress_rejects_multiple_sources) {
    const auto root = test_temp_dir("xxe-multiple-sources");
    const auto first = root / "first.txt";
    const auto second = root / "second.txt";
    write_text_file(first, "first");
    write_text_file(second, "second");

    bool rejected = false;
    try {
        (void)superzip::compress_xxe({first, second}, root / "multi.xxe");
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify XXEncode extraction rejects hostile embedded output paths before publishing.
// Inputs: A handcrafted XXE fixture whose begin line attempts traversal.
// Outputs: Throws if extraction succeeds or writes outside the destination.
TEST_CASE(xxe_extract_rejects_unsafe_header_path) {
    const auto root = test_temp_dir("xxe-unsafe-path");
    const auto archive = root / "unsafe.xxe";
    write_text_file(archive, "begin 644 ..\\escape.txt\n+\nend\n");

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_xxe_file(archive, output, false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify malformed XXEncode payloads do not publish partial outputs.
// Inputs: An XXE stream with a valid safe header and an invalid payload character.
// Outputs: Throws if extraction succeeds or leaves the final output file behind.
TEST_CASE(xxe_extract_rejects_malformed_payload_without_output) {
    const auto root = test_temp_dir("xxe-malformed-payload");
    const auto archive = root / "bad.xxe";
    write_text_file(archive, "begin 644 payload.txt\n~\n+\nend\n");

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_xxe_file(archive, output, true);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(output / "payload.txt"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify unsupported historical trailer extensions are rejected rather than silently ignored.
// Inputs: A valid empty XXE stream followed by non-whitespace post-end trailer data.
// Outputs: Throws if extraction succeeds or publishes the final output file.
TEST_CASE(xxe_extract_rejects_post_end_trailer_data) {
    const auto root = test_temp_dir("xxe-post-end-trailer");
    const auto archive = root / "trailer.xxe";
    write_text_file(archive, "begin 644 payload.txt\n+\nend\nbytes 0\n");

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_xxe_file(archive, output, true);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(output / "payload.txt"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify XXEncode extraction accepts the historical space zero-length terminator.
// Inputs: A valid empty XXE stream that uses a space terminator instead of `+`.
// Outputs: Throws if extraction fails or writes unexpected bytes.
TEST_CASE(xxe_extract_accepts_space_zero_length_terminator) {
    const auto root = test_temp_dir("xxe-space-terminator");
    const auto archive = root / "empty.xxe";
    write_text_file(archive, "begin 644 empty.txt\n \nend\n");

    const auto output = root / "out";
    const auto stats = superzip::extract_xxe_file(archive, output, false);
    REQUIRE_EQ(stats.output_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_TRUE(std::filesystem::exists(output / "empty.txt"));
    REQUIRE_EQ(read_binary_file(output / "empty.txt"), std::vector<unsigned char>{});
    std::filesystem::remove_all(root);
}

// Purpose: Verify XXEncode extraction refuses to overwrite existing files by default.
// Inputs: A valid XXE archive extracted over an existing destination file.
// Outputs: Throws if overwrite refusal is not enforced.
TEST_CASE(xxe_extract_rejects_overwrite_by_default) {
    const auto root = test_temp_dir("xxe-overwrite");
    const auto input = root / "sample.txt";
    write_text_file(input, "new payload");
    const auto archive = root / "sample.xxe";
    (void)superzip::compress_xxe_file(input, archive);

    const auto output = root / "out";
    std::filesystem::create_directories(output);
    write_text_file(output / "sample.txt", "old payload");

    bool rejected = false;
    try {
        (void)superzip::extract_xxe_file(archive, output, false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_binary_file(output / "sample.txt"), std::vector<unsigned char>({'o', 'l', 'd', ' ', 'p', 'a', 'y', 'l', 'o', 'a', 'd'}));
    std::filesystem::remove_all(root);
}
