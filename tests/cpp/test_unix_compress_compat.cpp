#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "unix_compress/unix_compress_adapter.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

// Purpose: Write exact text bytes to a fixture file.
// Inputs: `path` is the destination and `text` is the payload.
// Outputs: Creates or replaces the file.
void write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// Purpose: Read a full file as text bytes for equality checks.
// Inputs: `path` is the file to read.
// Outputs: Returns the complete payload.
std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Purpose: Write exact unsigned bytes to a fixture file.
// Inputs: `path` is the destination and `bytes` is the payload.
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

// Purpose: Generate a deterministic mixed payload large enough to grow LZW code width.
// Inputs: `bytes` is the requested byte count.
// Outputs: Returns reproducible binary-looking content.
std::string mixed_payload(std::size_t bytes) {
    std::string payload;
    payload.reserve(bytes);
    for (std::size_t i = 0; i < bytes; ++i) {
        const auto value = static_cast<unsigned char>((i * 37U + (i >> 3U) + 11U) & 0xFFU);
        payload.push_back(static_cast<char>(value));
    }
    return payload;
}

}  // namespace

// Purpose: Verify a minimal handcrafted `.Z` stream decodes without relying on SuperZip's encoder.
// Inputs: A standard block-mode header followed by one 9-bit literal `A` code.
// Outputs: Throws if extraction rejects the fixture or restores wrong bytes.
TEST_CASE(unix_compress_extracts_handcrafted_single_literal) {
    const auto root = test_temp_dir("unix-compress-handcrafted");
    const auto archive = root / "single.Z";
    write_binary_file(archive, {0x1FU, 0x9DU, 0x90U, 0x41U, 0x00U});

    const auto output = root / "out";
    const auto stats = superzip::extract_unix_compress_file(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(read_text_file(output / "single"), "A");
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::UnixCompress);
}

// Purpose: Verify a non-block-mode `.Z` stream decodes with the same bounded reader.
// Inputs: A valid 9-bit non-block header followed by one literal `A` code.
// Outputs: Throws if extraction incorrectly requires block mode.
TEST_CASE(unix_compress_extracts_non_block_single_literal) {
    const auto root = test_temp_dir("unix-compress-non-block");
    const auto archive = root / "single.Z";
    write_binary_file(archive, {0x1FU, 0x9DU, 0x09U, 0x41U, 0x00U});

    const auto output = root / "out";
    const auto stats = superzip::extract_unix_compress_file(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(read_text_file(output / "single"), "A");
}

// Purpose: Verify an empty regular file remains valid through `.Z` compression and extraction.
// Inputs: A zero-byte source file compressed through the native adapter.
// Outputs: Throws if the empty payload is lost, rejected, or restored with extra bytes.
TEST_CASE(unix_compress_roundtrip_empty_file) {
    const auto root = test_temp_dir("unix-compress-empty");
    const auto input = root / "empty.bin";
    write_text_file(input, "");

    const auto archive = root / "empty.bin.Z";
    const auto compress_stats = superzip::compress_unix_compress_file(input, archive);
    REQUIRE_EQ(compress_stats.entries, static_cast<std::uint64_t>(1));

    const auto output = root / "out";
    const auto extract_stats = superzip::extract_unix_compress_file(archive, output, false);
    REQUIRE_EQ(extract_stats.output_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(read_text_file(output / "empty.bin"), "");
}

// Purpose: Verify native `.Z` compatibility roundtrip over enough data to exercise dictionary growth.
// Inputs: A deterministic regular file compressed and extracted through the Unix Compress adapter.
// Outputs: Throws if format detection, extraction, or restored bytes regress.
TEST_CASE(unix_compress_roundtrip_with_dictionary_growth) {
    const auto root = test_temp_dir("unix-compress-roundtrip");
    const auto input = root / "payload.bin";
    const auto payload = mixed_payload(192U * 1024U);
    write_text_file(input, payload);

    const auto archive = root / "payload.bin.Z";
    const auto compress_stats = superzip::compress_unix_compress_file(input, archive);
    REQUIRE_EQ(compress_stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_TRUE(compress_stats.output_bytes > 3U);
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::UnixCompress);

    const auto output = root / "out";
    const auto extract_stats = superzip::extract_unix_compress_file(archive, output, false);
    REQUIRE_EQ(extract_stats.output_bytes, static_cast<std::uint64_t>(payload.size()));
    REQUIRE_EQ(read_text_file(output / "payload.bin"), payload);
}

// Purpose: Verify `.Z` compression enforces single-file source semantics.
// Inputs: Two files passed as one source set.
// Outputs: Throws if the adapter accepts multiple sources.
TEST_CASE(unix_compress_rejects_multiple_sources) {
    const auto root = test_temp_dir("unix-compress-multiple");
    const auto first = root / "first.txt";
    const auto second = root / "second.txt";
    write_text_file(first, "first");
    write_text_file(second, "second");

    bool rejected = false;
    try {
        (void)superzip::compress_unix_compress({first, second}, root / "multi.Z");
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify `.Z` extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A valid `.Z` stream and a preexisting destination file.
// Outputs: Throws if extraction overwrites while `overwrite` is false.
TEST_CASE(unix_compress_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("unix-compress-overwrite");
    const auto input = root / "sample.txt";
    write_text_file(input, "new payload");
    const auto archive = root / "sample.txt.Z";
    (void)superzip::compress_unix_compress_file(input, archive);

    const auto output = root / "out";
    std::filesystem::create_directories(output);
    write_text_file(output / "sample.txt", "old payload");

    bool rejected = false;
    try {
        (void)superzip::extract_unix_compress_file(archive, output, false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "sample.txt"), "old payload");
}

// Purpose: Verify malformed `.Z` headers do not publish output.
// Inputs: A file with the `.Z` extension but an invalid magic header.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(unix_compress_rejects_bad_magic_without_output) {
    const auto root = test_temp_dir("unix-compress-bad-magic");
    const auto archive = root / "bad.Z";
    write_binary_file(archive, {0x1FU, 0x8BU, 0x90U, 0x00U});

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_unix_compress_file(archive, output, false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}

// Purpose: Verify reserved `.Z` header flags are rejected before output publication.
// Inputs: A stream with valid magic but a reserved flag bit in the header byte.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(unix_compress_rejects_reserved_flags_without_output) {
    const auto root = test_temp_dir("unix-compress-reserved-flags");
    const auto archive = root / "bad.Z";
    write_binary_file(archive, {0x1FU, 0x9DU, 0xB0U, 0x41U, 0x00U});

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_unix_compress_file(archive, output, false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}

// Purpose: Verify unsupported `.Z` maxbits values are rejected before output publication.
// Inputs: A stream with valid magic but an 8-bit maxbits declaration.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(unix_compress_rejects_bad_maxbits_without_output) {
    const auto root = test_temp_dir("unix-compress-bad-maxbits");
    const auto archive = root / "bad.Z";
    write_binary_file(archive, {0x1FU, 0x9DU, 0x88U, 0x41U, 0x00U});

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_unix_compress_file(archive, output, false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}
