#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "xar/xar_adapter.hpp"

#include "miniz.h"

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

// Purpose: Append one big-endian unsigned 16-bit field to a byte vector.
// Inputs: `bytes` receives output and `value` is the decoded integer.
// Outputs: Appends two bytes.
void append_be16(std::vector<unsigned char>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
}

// Purpose: Append one big-endian unsigned 32-bit field to a byte vector.
// Inputs: `bytes` receives output and `value` is the decoded integer.
// Outputs: Appends four bytes.
void append_be32(std::vector<unsigned char>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<unsigned char>((value >> 24U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
}

// Purpose: Append one big-endian unsigned 64-bit field to a byte vector.
// Inputs: `bytes` receives output and `value` is the decoded integer.
// Outputs: Appends eight bytes.
void append_be64(std::vector<unsigned char>& bytes, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<unsigned char>((value >> static_cast<unsigned int>(shift)) & 0xFFU));
    }
}

// Purpose: Compress a small fixture buffer with the zlib wrapper used by XAR.
// Inputs: `text` is the source payload.
// Outputs: Returns a zlib-compressed byte buffer.
std::vector<unsigned char> zlib_compress(std::string_view text) {
    auto capacity = mz_compressBound(static_cast<mz_ulong>(text.size()));
    std::vector<unsigned char> output(static_cast<std::size_t>(capacity));
    auto output_size = capacity;
    const auto status = mz_compress2(
        output.data(),
        &output_size,
        reinterpret_cast<const unsigned char*>(text.data()),
        static_cast<mz_ulong>(text.size()),
        MZ_BEST_COMPRESSION);
    REQUIRE_EQ(status, MZ_OK);
    output.resize(static_cast<std::size_t>(output_size));
    return output;
}

// Purpose: Write a complete XAR archive fixture.
// Inputs: `archive` is the destination, `toc` is uncompressed XML, and `heap` is the archive heap.
// Outputs: Creates or replaces the XAR file.
void write_xar_fixture(
    const std::filesystem::path& archive,
    const std::string& toc,
    std::span<const unsigned char> heap,
    std::uint32_t checksum_algorithm = 0U) {
    const auto compressed_toc = zlib_compress(toc);
    std::vector<unsigned char> bytes;
    bytes.reserve(28U + compressed_toc.size() + heap.size());
    append_be32(bytes, 0x78617221U);
    append_be16(bytes, 28U);
    append_be16(bytes, 1U);
    append_be64(bytes, static_cast<std::uint64_t>(compressed_toc.size()));
    append_be64(bytes, static_cast<std::uint64_t>(toc.size()));
    append_be32(bytes, checksum_algorithm);
    bytes.insert(bytes.end(), compressed_toc.begin(), compressed_toc.end());
    bytes.insert(bytes.end(), heap.begin(), heap.end());

    std::ofstream output(archive, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Purpose: Create a nested XAR fixture with one directory and one file.
// Inputs: `archive` is the destination and `payload` is the regular-file text.
// Outputs: Writes a zlib-compressed XAR payload fixture.
void write_nested_xar_fixture(const std::filesystem::path& archive, std::string_view payload) {
    const auto compressed_payload = zlib_compress(payload);
    const std::string toc =
        "<?xml version=\"1.0\"?>"
        "<xar><toc>"
        "<file id=\"1\"><name>subdir</name><type>directory</type>"
        "<file id=\"2\"><name>hello.txt</name><type>file</type>"
        "<data><length>" + std::to_string(payload.size()) + "</length>"
        "<offset>0</offset>"
        "<size>" + std::to_string(compressed_payload.size()) + "</size>"
        "<encoding style=\"application/x-gzip\"/>"
        "</data></file>"
        "</file>"
        "</toc></xar>";
    write_xar_fixture(archive, toc, compressed_payload);
}

// Purpose: Read a full text file for extraction assertions.
// Inputs: `path` is the extracted file path.
// Outputs: Returns its full binary text payload.
std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Purpose: Count regular files below a directory for no-output assertions.
// Inputs: `root` is the directory to inspect.
// Outputs: Returns zero for missing roots or the number of regular files.
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

// Purpose: Verify native `.xar` extraction reads a nested zlib-compressed payload.
// Inputs: A handcrafted XAR archive with `subdir/hello.txt`.
// Outputs: Throws if detection, extraction stats, or payload bytes regress.
TEST_CASE(xar_extraction_reads_nested_zlib_payload) {
    const auto root = test_temp_dir("xar-basic");
    const auto archive = root / "sample.xar";
    write_nested_xar_fixture(archive, "hello xar\n");

    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Xar);
    const auto info = superzip::archive_format_info(superzip::ArchiveFormat::Xar);
    REQUIRE_TRUE(!info.can_create);
    REQUIRE_TRUE(info.can_extract);
    REQUIRE_TRUE(info.bundled_native);
    REQUIRE_TRUE(!info.gpu_native);

    const auto output = root / "out";
    const auto stats = superzip::extract_xar(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(2));
    REQUIRE_EQ(stats.output_bytes, static_cast<std::uint64_t>(std::string_view("hello xar\n").size()));
    REQUIRE_TRUE(!stats.gpu_used);
    REQUIRE_EQ(read_text_file(output / "subdir" / "hello.txt"), "hello xar\n");
}

// Purpose: Verify XAR extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A nested XAR member and a preexisting destination file of the same name.
// Outputs: Throws `SecurityError` and preserves the original file.
TEST_CASE(xar_extraction_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("xar-overwrite");
    const auto archive = root / "sample.xar";
    write_nested_xar_fixture(archive, "hello xar\n");
    const auto output = root / "out";
    std::filesystem::create_directories(output / "subdir");
    std::ofstream(output / "subdir" / "hello.txt") << "old";

    bool rejected = false;
    try {
        (void)superzip::extract_xar(archive, output, false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "subdir" / "hello.txt"), "old");
}

// Purpose: Verify parent-directory XAR paths are rejected before output publication.
// Inputs: A XAR TOC containing `../escape.txt`.
// Outputs: Throws `SecurityError` and produces no extracted files.
TEST_CASE(xar_extraction_rejects_parent_directory_paths) {
    const auto root = test_temp_dir("xar-dotdot");
    const auto archive = root / "dotdot.xar";
    const std::string payload = "bad\n";
    const auto compressed_payload = zlib_compress(payload);
    const std::string toc =
        "<xar><toc><file id=\"1\"><name>../escape.txt</name><type>file</type>"
        "<data><length>" + std::to_string(payload.size()) + "</length><offset>0</offset>"
        "<size>" + std::to_string(compressed_payload.size()) + "</size>"
        "<encoding style=\"application/x-gzip\"/></data></file></toc></xar>";
    write_xar_fixture(archive, toc, compressed_payload);

    bool rejected = false;
    try {
        (void)superzip::extract_xar(archive, root / "out", false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
}

// Purpose: Verify XAR symbolic links are rejected instead of materialized.
// Inputs: A XAR TOC entry with type `symlink`.
// Outputs: Throws `SecurityError` and creates no output files.
TEST_CASE(xar_extraction_rejects_symbolic_links) {
    const auto root = test_temp_dir("xar-symlink");
    const auto archive = root / "symlink.xar";
    const std::string toc =
        "<xar><toc><file id=\"1\"><name>link.txt</name><type>symlink</type></file></toc></xar>";
    const std::array<unsigned char, 1> empty_heap{0};
    write_xar_fixture(archive, toc, std::span<const unsigned char>(empty_heap.data(), 0));

    bool rejected = false;
    try {
        (void)superzip::extract_xar(archive, root / "out", false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify unsupported XAR heap encodings fail closed.
// Inputs: A XAR file entry declaring Bzip2 payload encoding.
// Outputs: Throws `ArchiveError` before output publication.
TEST_CASE(xar_extraction_rejects_unsupported_encoding) {
    const auto root = test_temp_dir("xar-unsupported-encoding");
    const auto archive = root / "unsupported.xar";
    const std::string payload = "plain";
    const std::string toc =
        "<xar><toc><file id=\"1\"><name>payload.txt</name><type>file</type>"
        "<data><length>" + std::to_string(payload.size()) + "</length><offset>0</offset>"
        "<size>" + std::to_string(payload.size()) + "</size>"
        "<encoding style=\"application/x-bzip2\"/></data></file></toc></xar>";
    write_xar_fixture(
        archive,
        toc,
        std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(payload.data()), payload.size()));

    bool rejected = false;
    try {
        (void)superzip::extract_xar(archive, root / "out", false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify XAR archives declaring TOC checksum modes fail closed until checksum verification is implemented.
// Inputs: A valid XAR body whose header declares SHA-1 TOC checksum metadata.
// Outputs: Throws `ArchiveError` before parsing or extracting archive entries.
TEST_CASE(xar_extraction_rejects_unverified_toc_checksum_algorithm) {
    const auto root = test_temp_dir("xar-toc-checksum");
    const auto archive = root / "checksum.xar";
    const std::string payload = "plain";
    const std::string toc =
        "<xar><toc><file id=\"1\"><name>payload.txt</name><type>file</type>"
        "<data><length>" + std::to_string(payload.size()) + "</length><offset>0</offset>"
        "<size>" + std::to_string(payload.size()) + "</size></data></file></toc></xar>";
    write_xar_fixture(
        archive,
        toc,
        std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(payload.data()), payload.size()),
        1U);

    bool rejected = false;
    try {
        (void)superzip::extract_xar(archive, root / "out", false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify corrupt compressed XAR payloads are rejected before publication.
// Inputs: A valid XAR fixture with one heap byte flipped.
// Outputs: Throws `ArchiveError` and creates no regular files.
TEST_CASE(xar_extraction_rejects_corrupt_zlib_payload_before_output) {
    const auto root = test_temp_dir("xar-corrupt-payload");
    const auto archive = root / "corrupt.xar";
    const std::string payload = "hello xar\n";
    auto compressed_payload = zlib_compress(payload);
    compressed_payload.back() ^= 0x01U;
    const std::string toc =
        "<xar><toc><file id=\"1\"><name>payload.txt</name><type>file</type>"
        "<data><length>" + std::to_string(payload.size()) + "</length><offset>0</offset>"
        "<size>" + std::to_string(compressed_payload.size()) + "</size>"
        "<encoding style=\"application/x-gzip\"/></data></file></toc></xar>";
    write_xar_fixture(archive, toc, compressed_payload);

    bool rejected = false;
    try {
        (void)superzip::extract_xar(archive, root / "out", false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}
