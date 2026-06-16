#include "test_util.hpp"

#include "bzip2/bzip2_adapter.hpp"
#include "core/archive_format.hpp"
#include "core/result.hpp"

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

// Purpose: Generate deterministic content large enough to span several streaming chunks.
// Inputs: `bytes` is the requested byte count.
// Outputs: Returns reproducible mixed text/binary content.
std::string mixed_payload(std::size_t bytes) {
    std::string payload;
    payload.reserve(bytes);
    for (std::size_t i = 0; i < bytes; ++i) {
        const auto value = static_cast<unsigned char>((i * 17U + (i >> 5U) + 29U) & 0xFFU);
        payload.push_back(static_cast<char>(value));
    }
    return payload;
}

}  // namespace

// Purpose: Verify native `.bz2` compatibility roundtrip over streaming-sized data.
// Inputs: A deterministic regular file compressed and extracted through the Bzip2 adapter.
// Outputs: Throws if format detection, extraction, or restored bytes regress.
TEST_CASE(bzip2_roundtrip_single_file) {
    const auto root = test_temp_dir("bzip2-roundtrip");
    const auto input = root / "payload.bin";
    const auto payload = mixed_payload(256U * 1024U);
    write_text_file(input, payload);

    const auto archive = root / "payload.bin.bz2";
    const auto compress_stats = superzip::compress_bzip2_file(input, archive);
    REQUIRE_EQ(compress_stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_TRUE(compress_stats.output_bytes > 4U);
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Bzip2);

    const auto output = root / "out";
    const auto extract_stats = superzip::extract_bzip2_file(archive, output, false);
    REQUIRE_EQ(extract_stats.output_bytes, static_cast<std::uint64_t>(payload.size()));
    REQUIRE_EQ(read_text_file(output / "payload.bin"), payload);
}

// Purpose: Verify `.bz2` compression enforces single-file source semantics.
// Inputs: Two files passed as one source set.
// Outputs: Throws if the adapter accepts multiple sources.
TEST_CASE(bzip2_rejects_multiple_sources) {
    const auto root = test_temp_dir("bzip2-multiple");
    const auto first = root / "first.txt";
    const auto second = root / "second.txt";
    write_text_file(first, "first");
    write_text_file(second, "second");

    bool rejected = false;
    try {
        (void)superzip::compress_bzip2({first, second}, root / "multi.bz2");
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify `.bz2` extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A valid `.bz2` stream and a preexisting destination file.
// Outputs: Throws if extraction overwrites while `overwrite` is false.
TEST_CASE(bzip2_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("bzip2-overwrite");
    const auto input = root / "sample.txt";
    write_text_file(input, "new payload");
    const auto archive = root / "sample.txt.bz2";
    (void)superzip::compress_bzip2_file(input, archive);

    const auto output = root / "out";
    std::filesystem::create_directories(output);
    write_text_file(output / "sample.txt", "old payload");

    bool rejected = false;
    try {
        (void)superzip::extract_bzip2_file(archive, output, false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "sample.txt"), "old payload");
}

// Purpose: Verify malformed `.bz2` streams do not publish output.
// Inputs: A file with the `.bz2` extension but invalid Bzip2 magic/content.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(bzip2_rejects_bad_magic_without_output) {
    const auto root = test_temp_dir("bzip2-bad-magic");
    const auto archive = root / "bad.bz2";
    write_binary_file(archive, {'B', 'Z', '0', 0x00U});

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_bzip2_file(archive, output, false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}
