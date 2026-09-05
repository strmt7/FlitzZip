#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "zstd/zstd_adapter.hpp"
#include "zstd/zstd_stream.hpp"

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
    std::filesystem::create_directories(path.parent_path());
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
        const auto value = static_cast<unsigned char>((i * 31U + (i >> 3U) + 11U) & 0xFFU);
        payload.push_back(static_cast<char>(value));
    }
    return payload;
}

}  // namespace

// Purpose: Verify native `.zst` compatibility roundtrip over streaming-sized data.
// Inputs: A deterministic regular file compressed and extracted through the Zstandard adapter.
// Outputs: Throws if format detection, extraction, or restored bytes regress.
TEST_CASE(zstd_roundtrip_single_file) {
    const auto root = test_temp_dir("zstd-roundtrip");
    const auto input = root / "payload.bin";
    const auto payload = mixed_payload(320U * 1024U);
    write_text_file(input, payload);

    const auto archive = root / "payload.bin.zst";
    const auto compress_stats = superzip::compress_zstd({input}, archive);
    REQUIRE_EQ(compress_stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_TRUE(compress_stats.output_bytes > 8U);
    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Zstd);

    const auto output = root / "out";
    const auto extract_stats = superzip::extract_zstd_file(archive, output, false);
    REQUIRE_EQ(extract_stats.output_bytes, static_cast<std::uint64_t>(payload.size()));
    REQUIRE_EQ(read_text_file(output / "payload.bin"), payload);
}

// Purpose: Verify every supported compression effort roundtrips through the bounded decoder.
// Inputs: Each supported product effort and a bounded deterministic payload.
// Outputs: Throws if any effort creates a frame rejected by the bounded decoder.
TEST_CASE(zstd_all_compression_levels_roundtrip) {
    const auto root = test_temp_dir("zstd-levels");
    const auto input = root / "payload.bin";
    const auto payload = mixed_payload(4096);
    write_text_file(input, payload);
    for (int level = 1; level <= 9; ++level) {
        const auto archive = root / ("level-" + std::to_string(level) + ".zst");
        (void)superzip::compress_zstd({input}, archive, level);
        const auto output = root / ("out-" + std::to_string(level));
        (void)superzip::extract_zstd_file(archive, output, false);
        REQUIRE_EQ(read_text_file(output / archive.stem()), payload);
    }
}

// Purpose: Prove Maximum recovers long-distance redundancy missed by Balanced.
// Inputs: An 8 MiB stream containing a seeded 4 MiB pseudorandom region repeated exactly twice.
// Outputs: Requires at least 25 percent fewer encoded bytes and a byte-exact bounded-decoder roundtrip.
TEST_CASE(zstd_maximum_compacts_long_distance_repeats) {
    const auto root = test_temp_dir("zstd-long-distance");
    std::string payload(4U * 1024U * 1024U, '\0');
    std::uint32_t state = 0x13579BDFU;
    for (auto& byte : payload) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        byte = static_cast<char>(state & 0xFFU);
    }
    payload += payload;
    const auto input = root / "payload.bin";
    write_text_file(input, payload);
    const auto balanced = superzip::compress_zstd({input}, root / "balanced.zst", 5);
    const auto maximum = superzip::compress_zstd({input}, root / "maximum.zst", 9);
    REQUIRE_EQ(balanced.input_bytes, static_cast<std::uint64_t>(payload.size()));
    REQUIRE_EQ(maximum.input_bytes, balanced.input_bytes);
    REQUIRE_TRUE(maximum.output_bytes < balanced.output_bytes * 3U / 4U);
    const auto output = root / "out";
    (void)superzip::extract_zstd_file(root / "maximum.zst", output, false);
    REQUIRE_EQ(read_text_file(output / "maximum"), payload);
}

// Purpose: Verify stream lifecycle rejects invalid efforts and writes after finalization.
// Inputs: Invalid level values and a directly constructed output stream.
// Outputs: Throws if invalid construction succeeds, repeated close fails, or a closed stream accepts bytes.
TEST_CASE(zstd_stream_validates_effort_and_close_state) {
    const auto root = test_temp_dir("zstd-lifecycle");
    for (const int level : {0, 10}) {
        bool rejected = false;
        try {
            superzip::ZstdOutputStream stream(root / "invalid.zst", level);
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    superzip::ZstdOutputStream stream(root / "closed.zst");
    stream << "payload";
    stream.close();
    stream.close();
    stream << "late write";
    REQUIRE_TRUE(!stream.good());
}

// Purpose: Verify `.zstd` extension aliases derive a safe output filename.
// Inputs: A regular file compressed to a `.zstd` path and extracted through auto-detected Zstandard handling.
// Outputs: Throws if alias detection or output filename derivation regresses.
TEST_CASE(zstd_extension_alias_roundtrip) {
    const auto root = test_temp_dir("zstd-extension-alias");
    const auto input = root / "sample.txt";
    write_text_file(input, "zstandard alias payload");
    const auto archive = root / "sample.txt.zstd";
    (void)superzip::compress_zstd({input}, archive);

    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Zstd);
    const auto output = root / "out";
    (void)superzip::extract_zstd_file(archive, output, false);
    REQUIRE_EQ(read_text_file(output / "sample.txt"), "zstandard alias payload");
}

// Purpose: Verify `.zst` compression enforces single-file source semantics.
// Inputs: Two files passed as one source set.
// Outputs: Throws if the adapter accepts multiple sources.
TEST_CASE(zstd_rejects_multiple_sources) {
    const auto root = test_temp_dir("zstd-multiple");
    const auto first = root / "first.txt";
    const auto second = root / "second.txt";
    write_text_file(first, "first");
    write_text_file(second, "second");

    bool rejected = false;
    try {
        (void)superzip::compress_zstd({first, second}, root / "multi.zst");
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify `.zst` extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A valid `.zst` stream and a preexisting destination file.
// Outputs: Throws if extraction overwrites while `overwrite` is false.
TEST_CASE(zstd_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("zstd-overwrite");
    const auto input = root / "sample.txt";
    write_text_file(input, "new payload");
    const auto archive = root / "sample.txt.zst";
    (void)superzip::compress_zstd({input}, archive);

    const auto output = root / "out";
    write_text_file(output / "sample.txt", "old payload");

    bool rejected = false;
    try {
        (void)superzip::extract_zstd_file(archive, output, false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "sample.txt"), "old payload");
}

// Purpose: Verify malformed `.zst` streams do not publish output.
// Inputs: A file with the `.zst` extension but invalid Zstandard content.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(zstd_rejects_bad_magic_without_output) {
    const auto root = test_temp_dir("zstd-bad-magic");
    const auto archive = root / "bad.zst";
    write_text_file(archive, "not zstandard");

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_zstd_file(archive, output, false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}

// Purpose: Verify trailing garbage after a complete Zstandard frame is rejected before publication.
// Inputs: A valid `.zst` stream with extra non-frame bytes appended.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(zstd_rejects_trailing_garbage_without_output) {
    const auto root = test_temp_dir("zstd-trailing");
    const auto input = root / "sample.txt";
    write_text_file(input, "payload");
    const auto archive = root / "sample.txt.zst";
    (void)superzip::compress_zstd({input}, archive);
    {
        std::ofstream output(archive, std::ios::binary | std::ios::app);
        output << "trailing";
    }

    const auto output_dir = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_zstd_file(archive, output_dir, false);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output_dir), static_cast<std::uint64_t>(0));
}
