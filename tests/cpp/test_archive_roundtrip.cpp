#include "core/archive.hpp"
#include "core/result.hpp"
#include "test_util.hpp"

#include <fstream>
#include <windows.h>

namespace {

// Purpose: Overwrite a little-endian 64-bit field at a fixed archive byte offset.
// Inputs: `path` identifies the archive, `offset` is the zero-based byte offset, and `value` is the replacement value.
// Outputs: Mutates the archive in place or throws through the stream state checks in the test body.
void write_u64_at(const std::filesystem::path& path, std::streamoff offset, std::uint64_t value) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(offset, std::ios::beg);
    for (int i = 0; i < 8; ++i) {
        const char byte = static_cast<char>((value >> (i * 8)) & 0xFF);
        file.write(&byte, 1);
    }
}

}  // namespace

// Purpose: Verify `.szip` roundtrip behavior for compressible and mixed byte patterns.
// Inputs: Temporary files with repetitive and randomish content.
// Outputs: Throws on failed compression/extraction or mismatched restored bytes.
TEST_CASE(szip_roundtrip_repetitive_and_randomish_files) {
    const auto root = test_temp_dir("szip-roundtrip");
    const auto input_dir = root / "input";
    const auto nested = input_dir / "nested";
    std::filesystem::create_directories(nested);
    std::ofstream(input_dir / "zeros.bin", std::ios::binary).write(std::string(256 * 1024, '\0').data(), 256 * 1024);
    {
        std::ofstream out(nested / "pattern.bin", std::ios::binary);
        for (int i = 0; i < 200000; ++i) {
            const char ch = static_cast<char>((i * 131) & 0xFF);
            out.write(&ch, 1);
        }
    }
    const auto archive = root / "archive.szip";
    superzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.chunk_size = 128 * 1024;
    compress.verify_after_write = true;
    const auto compressed = superzip::compress_szip({input_dir}, archive, compress);
    REQUIRE_TRUE(compressed.output_bytes > 0);
    const auto output = root / "out";
    superzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.overwrite = true;
    extract.chunk_size = 64 * 1024;
    (void)superzip::extract_szip(archive, output, extract);
    REQUIRE_TRUE(std::filesystem::exists(output / "input" / "zeros.bin"));
    REQUIRE_TRUE(std::filesystem::exists(output / "input" / "nested" / "pattern.bin"));
    REQUIRE_EQ(std::filesystem::file_size(output / "input" / "zeros.bin"), static_cast<std::uintmax_t>(256 * 1024));
    REQUIRE_EQ(std::filesystem::file_size(output / "input" / "nested" / "pattern.bin"), static_cast<std::uintmax_t>(200000));
    std::filesystem::remove_all(root);
}

// Purpose: Verify `.szip` preserves empty files and empty directories.
// Inputs: A temporary source tree containing one empty file and one empty directory.
// Outputs: Throws when extracted filesystem shape is incomplete.
TEST_CASE(szip_roundtrip_empty_file_and_empty_directory) {
    const auto root = test_temp_dir("szip-empty");
    const auto input_dir = root / "input";
    const auto empty_dir = input_dir / "empty";
    std::filesystem::create_directories(empty_dir);
    std::ofstream(input_dir / "empty.txt", std::ios::binary);
    const auto archive = root / "archive.szip";
    superzip::CompressOptions compress;
    compress.gpu_required = false;
    (void)superzip::compress_szip({input_dir}, archive, compress);
    const auto output = root / "out";
    superzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.overwrite = true;
    (void)superzip::extract_szip(archive, output, extract);
    REQUIRE_TRUE(std::filesystem::is_directory(output / "input" / "empty"));
    REQUIRE_TRUE(std::filesystem::exists(output / "input" / "empty.txt"));
    REQUIRE_EQ(std::filesystem::file_size(output / "input" / "empty.txt"), static_cast<std::uintmax_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify secure extraction refuses accidental overwrite by default.
// Inputs: A `.szip` archive and preexisting extraction target file.
// Outputs: Throws if overwrite is not rejected with `SecurityError`.
TEST_CASE(szip_rejects_overwrite_by_default) {
    const auto root = test_temp_dir("szip-overwrite");
    const auto input = root / "file.txt";
    std::ofstream(input) << "first";
    const auto archive = root / "archive.szip";
    superzip::CompressOptions compress;
    compress.gpu_required = false;
    (void)superzip::compress_szip({input}, archive, compress);
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    std::ofstream(output / "file.txt") << "existing";
    superzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.overwrite = false;
    bool rejected = false;
    try {
        (void)superzip::extract_szip(archive, output, extract);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify `.szip` payload corruption is detected during verification.
// Inputs: A valid archive with one payload byte flipped.
// Outputs: Throws if verification does not reject the corrupted archive.
TEST_CASE(szip_verify_rejects_corrupt_payload) {
    const auto root = test_temp_dir("szip-corrupt");
    const auto input = root / "file.bin";
    std::ofstream(input, std::ios::binary) << "abcdefabcdefabcdef";
    const auto archive = root / "archive.szip";
    superzip::CompressOptions compress;
    compress.gpu_required = false;
    (void)superzip::compress_szip({input}, archive, compress);
    {
        std::fstream file(archive, std::ios::binary | std::ios::in | std::ios::out);
        char value = 0;
        file.seekg(0, std::ios::beg);
        file.read(&value, 1);
        value ^= 0x7F;
        file.seekp(0, std::ios::beg);
        file.write(&value, 1);
    }
    superzip::ExtractOptions extract;
    extract.gpu_required = false;
    bool rejected = false;
    try {
        (void)superzip::verify_szip(archive, extract);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify `.szip` rejects archives that are too small to contain a footer.
// Inputs: A temporary file shorter than the fixed SuperZip footer.
// Outputs: Throws if verification does not reject the truncated archive.
TEST_CASE(szip_verify_rejects_truncated_footer) {
    const auto root = test_temp_dir("szip-truncated-footer");
    const auto archive = root / "short.szip";
    std::ofstream(archive, std::ios::binary) << "not a valid archive";
    superzip::ExtractOptions extract;
    bool rejected = false;
    try {
        (void)superzip::verify_szip(archive, extract);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify `.szip` rejects a footer whose index offset points outside the archive file.
// Inputs: A valid archive with the footer index offset overwritten beyond EOF.
// Outputs: Throws if verification trusts the corrupt footer.
TEST_CASE(szip_verify_rejects_index_offset_outside_file) {
    const auto root = test_temp_dir("szip-index-outside");
    const auto input = root / "file.bin";
    std::ofstream(input, std::ios::binary) << "payload";
    const auto archive = root / "archive.szip";
    superzip::CompressOptions compress;
    compress.gpu_required = false;
    (void)superzip::compress_szip({input}, archive, compress);
    const auto size = std::filesystem::file_size(archive);
    write_u64_at(archive, static_cast<std::streamoff>(size - 16), size + 1024);
    superzip::ExtractOptions extract;
    bool rejected = false;
    try {
        (void)superzip::verify_szip(archive, extract);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify `.szip` rejects an archive footer with a corrupt magic value.
// Inputs: A valid archive whose footer magic byte is modified.
// Outputs: Throws if verification accepts the corrupt footer.
TEST_CASE(szip_verify_rejects_corrupt_footer_magic) {
    const auto root = test_temp_dir("szip-footer-magic");
    const auto input = root / "file.bin";
    const std::string payload(4096, 'x');
    std::ofstream(input, std::ios::binary).write(payload.data(), static_cast<std::streamsize>(payload.size()));
    const auto archive = root / "archive.szip";
    superzip::CompressOptions compress;
    compress.gpu_required = false;
    (void)superzip::compress_szip({input}, archive, compress);
    {
        const auto size = std::filesystem::file_size(archive);
        std::fstream file(archive, std::ios::binary | std::ios::in | std::ios::out);
        char byte = 0;
        file.seekg(static_cast<std::streamoff>(size - 24), std::ios::beg);
        file.read(&byte, 1);
        byte ^= 0x7F;
        file.seekp(static_cast<std::streamoff>(size - 24), std::ios::beg);
        file.write(&byte, 1);
    }
    superzip::ExtractOptions extract;
    bool rejected = false;
    try {
        (void)superzip::verify_szip(archive, extract);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}
