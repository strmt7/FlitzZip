#include "test_util.hpp"

#include "bzip2/bzip2_adapter.hpp"
#include "core/result.hpp"
#include "cpio/cpio_adapter.hpp"
#include "gzip/gzip_adapter.hpp"
#include "tar/tar_adapter.hpp"
#include "zip/zip_adapter.hpp"
#include "zstd/zstd_adapter.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

// Purpose: Write exact text bytes to a fixture path.
// Inputs: `path` is the output file and `payload` is the byte sequence.
// Outputs: Creates parent directories and writes the complete payload.
void write_payload_file(const std::filesystem::path& path, const std::string& payload) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
}

// Purpose: Read a whole text/binary fixture into memory for equality checks.
// Inputs: `path` is the file to read.
// Outputs: Returns the complete file payload.
std::string read_payload_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Purpose: Build deterministic compressible data that exposes level differences.
// Inputs: None.
// Outputs: Returns a repeated payload large enough for streaming adapters.
std::string compression_level_payload() {
    std::string motif;
    motif.reserve(196U * 1024U);
    for (std::size_t i = 0; i < 196U * 1024U; ++i) {
        const auto value = static_cast<char>('A' + ((i * 17U + (i >> 4U)) % 23U));
        motif.push_back(value);
    }
    std::string payload;
    payload.reserve(motif.size() * 8U);
    for (int i = 0; i < 8; ++i) {
        payload += motif;
    }
    return payload;
}

// Purpose: Assert that a compression request throws for an invalid level.
// Inputs: `operation` is the archive command to execute.
// Outputs: Throws through the test harness unless `ArchiveError` is observed.
template <typename Operation> void require_invalid_level_rejected(Operation operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

}  // namespace

// Purpose: Verify level-aware compatibility encoders round-trip at the fastest and maximum levels.
// Inputs: Temporary source files are compressed through every level-aware compatibility adapter.
// Outputs: Throws if any archive fails extraction or restores different bytes.
TEST_CASE(compat_compression_levels_roundtrip_level_1_and_9) {
    const auto root = test_temp_dir("compat-level-roundtrip");
    const auto source_dir = root / "input";
    const auto source_file = source_dir / "payload.txt";
    const auto payload = compression_level_payload();
    write_payload_file(source_file, payload);

    for (const int level : {1, 9}) {
        const auto suffix = std::to_string(level);

        const auto zip_archive = root / ("archive-" + suffix + ".zip");
        (void)superzip::compress_zip({source_dir}, zip_archive, level);
        (void)superzip::extract_zip(zip_archive, root / ("zip-out-" + suffix), true);
        REQUIRE_EQ(read_payload_file(root / ("zip-out-" + suffix) / "input" / "payload.txt"), payload);

        const auto gzip_archive = root / ("payload-" + suffix + ".txt.gz");
        (void)superzip::compress_gzip_file(source_file, gzip_archive, level);
        (void)superzip::extract_gzip_file(gzip_archive, root / ("gzip-out-" + suffix), true);
        REQUIRE_EQ(read_payload_file(root / ("gzip-out-" + suffix) / ("payload-" + suffix + ".txt")), payload);

        const auto bzip2_archive = root / ("payload-" + suffix + ".txt.bz2");
        (void)superzip::compress_bzip2_file(source_file, bzip2_archive, level);
        (void)superzip::extract_bzip2_file(bzip2_archive, root / ("bzip2-out-" + suffix), true);
        REQUIRE_EQ(read_payload_file(root / ("bzip2-out-" + suffix) / ("payload-" + suffix + ".txt")), payload);

        const auto zstd_archive = root / ("payload-" + suffix + ".txt.zst");
        (void)superzip::compress_zstd({source_file}, zstd_archive, level);
        (void)superzip::extract_zstd_file(zstd_archive, root / ("zstd-out-" + suffix), true);
        REQUIRE_EQ(read_payload_file(root / ("zstd-out-" + suffix) / ("payload-" + suffix + ".txt")), payload);

        const auto tar_gzip_archive = root / ("archive-" + suffix + ".tar.gz");
        (void)superzip::compress_tar_gzip({source_dir}, tar_gzip_archive, level);
        (void)superzip::extract_tar_gzip(tar_gzip_archive, root / ("tar-gzip-out-" + suffix), true);
        REQUIRE_EQ(read_payload_file(root / ("tar-gzip-out-" + suffix) / "input" / "payload.txt"), payload);

        const auto tar_bzip2_archive = root / ("archive-" + suffix + ".tar.bz2");
        (void)superzip::compress_tar_bzip2({source_dir}, tar_bzip2_archive, level);
        (void)superzip::extract_tar_bzip2(tar_bzip2_archive, root / ("tar-bzip2-out-" + suffix), true);
        REQUIRE_EQ(read_payload_file(root / ("tar-bzip2-out-" + suffix) / "input" / "payload.txt"), payload);

        const auto tar_zstd_archive = root / ("archive-" + suffix + ".tar.zst");
        (void)superzip::compress_tar_zstd({source_dir}, tar_zstd_archive, level);
        (void)superzip::extract_tar_zstd(tar_zstd_archive, root / ("tar-zstd-out-" + suffix), true);
        REQUIRE_EQ(read_payload_file(root / ("tar-zstd-out-" + suffix) / "input" / "payload.txt"), payload);

        const auto cpio_gzip_archive = root / ("archive-" + suffix + ".cpgz");
        (void)superzip::compress_cpio_gzip({source_dir}, cpio_gzip_archive, level);
        (void)superzip::extract_cpio_gzip(cpio_gzip_archive, root / ("cpio-gzip-out-" + suffix), true);
        REQUIRE_EQ(read_payload_file(root / ("cpio-gzip-out-" + suffix) / "input" / "payload.txt"), payload);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Verify representative encoders apply the selected level strongly enough to improve size.
// Inputs: The same deterministic payload is compressed with level 1 and level 9.
// Outputs: Throws if the maximum level is larger than the fastest level.
TEST_CASE(compat_compression_levels_affect_output_size) {
    const auto root = test_temp_dir("compat-level-size");
    const auto source_dir = root / "input";
    const auto source_file = source_dir / "payload.txt";
    write_payload_file(source_file, compression_level_payload());

    const auto zip_fast = superzip::compress_zip({source_dir}, root / "fast.zip", 1);
    const auto zip_max = superzip::compress_zip({source_dir}, root / "max.zip", 9);
    REQUIRE_TRUE(zip_max.output_bytes <= zip_fast.output_bytes);

    const auto gzip_fast = superzip::compress_gzip_file(source_file, root / "fast.txt.gz", 1);
    const auto gzip_max = superzip::compress_gzip_file(source_file, root / "max.txt.gz", 9);
    REQUIRE_TRUE(gzip_max.output_bytes <= gzip_fast.output_bytes);

    const auto bzip2_fast = superzip::compress_bzip2_file(source_file, root / "fast.txt.bz2", 1);
    const auto bzip2_max = superzip::compress_bzip2_file(source_file, root / "max.txt.bz2", 9);
    REQUIRE_TRUE(bzip2_max.output_bytes <= bzip2_fast.output_bytes);

    const auto zstd_fast = superzip::compress_zstd({source_file}, root / "fast.txt.zst", 1);
    const auto zstd_max = superzip::compress_zstd({source_file}, root / "max.txt.zst", 9);
    REQUIRE_TRUE(zstd_max.output_bytes <= zstd_fast.output_bytes);

    std::filesystem::remove_all(root);
}

// Purpose: Verify level-aware compatibility encoders reject invalid levels instead of clamping silently.
// Inputs: Temporary source files are passed with level zero.
// Outputs: Throws if any level-aware adapter accepts the invalid request.
TEST_CASE(compat_compression_levels_reject_invalid_values) {
    const auto root = test_temp_dir("compat-level-invalid");
    const auto source_dir = root / "input";
    const auto source_file = source_dir / "payload.txt";
    write_payload_file(source_file, compression_level_payload());

    require_invalid_level_rejected([&] { (void)superzip::compress_zip({source_dir}, root / "invalid.zip", 0); });
    require_invalid_level_rejected([&] { (void)superzip::compress_gzip_file(source_file, root / "invalid.gz", 0); });
    require_invalid_level_rejected([&] { (void)superzip::compress_bzip2_file(source_file, root / "invalid.bz2", 0); });
    require_invalid_level_rejected([&] { (void)superzip::compress_zstd({source_file}, root / "invalid.zst", 0); });
    require_invalid_level_rejected(
        [&] { (void)superzip::compress_tar_gzip({source_dir}, root / "invalid.tar.gz", 0); });
    require_invalid_level_rejected(
        [&] { (void)superzip::compress_tar_bzip2({source_dir}, root / "invalid.tar.bz2", 0); });
    require_invalid_level_rejected(
        [&] { (void)superzip::compress_tar_zstd({source_dir}, root / "invalid.tar.zst", 0); });
    require_invalid_level_rejected([&] { (void)superzip::compress_cpio_gzip({source_dir}, root / "invalid.cpgz", 0); });

    std::filesystem::remove_all(root);
}
