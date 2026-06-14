#include "core/archive_index.hpp"
#include "core/archive.hpp"
#include "core/checksum.hpp"
#include "core/result.hpp"
#include "test_util.hpp"

#include <array>
#include <fstream>
#include <span>
#include <sstream>
#include <vector>
#include <windows.h>

namespace {

constexpr std::uint32_t kTestFooterMagic = 0x465A5553;  // SUZF

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

// Purpose: Write the native archive footer for handcrafted validation tests.
// Inputs: `file` is positioned after the serialized index and `index_offset`/`index_size` describe that index.
// Outputs: Appends a footer matching the production SUZIP footer layout.
void write_test_footer(std::ostream& file, std::uint64_t index_offset, std::uint64_t index_size) {
    superzip::write_u32(file, kTestFooterMagic);
    superzip::write_u32(file, superzip::kSuperZipVersion);
    superzip::write_u64(file, index_offset);
    superzip::write_u64(file, index_size);
}

}  // namespace

// Purpose: Verify `.suzip` roundtrip behavior for compressible and mixed byte patterns.
// Inputs: Temporary files with repetitive and randomish content.
// Outputs: Throws on failed compression/extraction or mismatched restored bytes.
TEST_CASE(suzip_roundtrip_repetitive_and_randomish_files) {
    const auto root = test_temp_dir("suzip-roundtrip");
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
    const auto archive = root / "archive.suzip";
    superzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.chunk_size = 128 * 1024;
    compress.block_size = 64 * 1024;
    compress.verify_after_write = true;
    const auto compressed = superzip::compress_suzip({input_dir}, archive, compress);
    REQUIRE_TRUE(compressed.output_bytes > 0);
    const auto output = root / "out";
    superzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.overwrite = true;
    extract.chunk_size = 64 * 1024;
    extract.block_size = 64 * 1024;
    (void)superzip::extract_suzip(archive, output, extract);
    REQUIRE_TRUE(std::filesystem::exists(output / "input" / "zeros.bin"));
    REQUIRE_TRUE(std::filesystem::exists(output / "input" / "nested" / "pattern.bin"));
    REQUIRE_EQ(std::filesystem::file_size(output / "input" / "zeros.bin"), static_cast<std::uintmax_t>(256 * 1024));
    REQUIRE_EQ(std::filesystem::file_size(output / "input" / "nested" / "pattern.bin"), static_cast<std::uintmax_t>(200000));
    std::filesystem::remove_all(root);
}

// Purpose: Verify `.suzip` preserves empty files and empty directories.
// Inputs: A temporary source tree containing one empty file and one empty directory.
// Outputs: Throws when extracted filesystem shape is incomplete.
TEST_CASE(suzip_roundtrip_empty_file_and_empty_directory) {
    const auto root = test_temp_dir("suzip-empty");
    const auto input_dir = root / "input";
    const auto empty_dir = input_dir / "empty";
    std::filesystem::create_directories(empty_dir);
    std::ofstream(input_dir / "empty.txt", std::ios::binary);
    const auto archive = root / "archive.suzip";
    superzip::CompressOptions compress;
    compress.gpu_required = false;
    (void)superzip::compress_suzip({input_dir}, archive, compress);
    const auto output = root / "out";
    superzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.overwrite = true;
    (void)superzip::extract_suzip(archive, output, extract);
    REQUIRE_TRUE(std::filesystem::is_directory(output / "input" / "empty"));
    REQUIRE_TRUE(std::filesystem::exists(output / "input" / "empty.txt"));
    REQUIRE_EQ(std::filesystem::file_size(output / "input" / "empty.txt"), static_cast<std::uintmax_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify forced-CPU diagnostics bypass AMD HIP without changing archive correctness.
// Inputs: A temporary source file compressed, verified, and extracted with `force_cpu` enabled.
// Outputs: Throws if any operation reports GPU usage or if extracted content differs.
TEST_CASE(suzip_force_cpu_roundtrip_reports_no_gpu_usage) {
    const auto root = test_temp_dir("suzip-force-cpu");
    const auto input = root / "input.bin";
    std::ofstream(input, std::ios::binary) << "force cpu benchmark path";
    const auto archive = root / "archive.suzip";

    superzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.force_cpu = true;
    const auto compressed = superzip::compress_suzip({input}, archive, compress);
    REQUIRE_TRUE(!compressed.gpu_used);

    superzip::ExtractOptions verify;
    verify.gpu_required = false;
    verify.force_cpu = true;
    const auto verified = superzip::verify_suzip(archive, verify);
    REQUIRE_TRUE(!verified.gpu_used);

    const auto output = root / "out";
    superzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.force_cpu = true;
    extract.overwrite = true;
    const auto extracted = superzip::extract_suzip(archive, output, extract);
    REQUIRE_TRUE(!extracted.gpu_used);
    REQUIRE_TRUE(std::filesystem::exists(output / "input.bin"));
    REQUIRE_EQ(std::filesystem::file_size(output / "input.bin"), static_cast<std::uintmax_t>(24));
    std::filesystem::remove_all(root);
}

// Purpose: Verify native SUZIP uses deflated blocks for text-heavy data instead of storing everything raw.
// Inputs: A repetitive log-like file large enough to amortize archive metadata.
// Outputs: Throws if the archive is not smaller than the source data or if verification fails.
TEST_CASE(suzip_compresses_text_heavy_payload) {
    const auto root = test_temp_dir("suzip-text-compression");
    const auto input = root / "log.txt";
    {
        std::ofstream out(input, std::ios::binary);
        for (int i = 0; i < 32768; ++i) {
            out << "SuperZip structured benchmark line with repeated fields and timestamps 2026-06-14\n";
        }
    }
    const auto source_size = std::filesystem::file_size(input);
    const auto archive = root / "archive.suzip";

    superzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.force_cpu = true;
    const auto compressed = superzip::compress_suzip({input}, archive, compress);
    REQUIRE_TRUE(compressed.output_bytes < source_size / 4U);

    superzip::ExtractOptions verify;
    verify.gpu_required = false;
    verify.force_cpu = true;
    (void)superzip::verify_suzip(archive, verify);
    std::filesystem::remove_all(root);
}

// Purpose: Verify secure extraction refuses accidental overwrite by default.
// Inputs: A `.suzip` archive and preexisting extraction target file.
// Outputs: Throws if overwrite is not rejected with `SecurityError`.
TEST_CASE(suzip_rejects_overwrite_by_default) {
    const auto root = test_temp_dir("suzip-overwrite");
    const auto input = root / "file.txt";
    std::ofstream(input) << "first";
    const auto archive = root / "archive.suzip";
    superzip::CompressOptions compress;
    compress.gpu_required = false;
    (void)superzip::compress_suzip({input}, archive, compress);
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    std::ofstream(output / "file.txt") << "existing";
    superzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.overwrite = false;
    bool rejected = false;
    try {
        (void)superzip::extract_suzip(archive, output, extract);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify `.suzip` payload corruption is detected during verification.
// Inputs: A valid archive with one payload byte flipped.
// Outputs: Throws if verification does not reject the corrupted archive.
TEST_CASE(suzip_verify_rejects_corrupt_payload) {
    const auto root = test_temp_dir("suzip-corrupt");
    const auto input = root / "file.bin";
    std::ofstream(input, std::ios::binary) << "abcdefabcdefabcdef";
    const auto archive = root / "archive.suzip";
    superzip::CompressOptions compress;
    compress.gpu_required = false;
    (void)superzip::compress_suzip({input}, archive, compress);
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
        (void)superzip::verify_suzip(archive, extract);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify `.suzip` rejects archives that are too small to contain a footer.
// Inputs: A temporary file shorter than the fixed SuperZip footer.
// Outputs: Throws if verification does not reject the truncated archive.
TEST_CASE(suzip_verify_rejects_truncated_footer) {
    const auto root = test_temp_dir("suzip-truncated-footer");
    const auto archive = root / "short.suzip";
    std::ofstream(archive, std::ios::binary) << "not a valid archive";
    superzip::ExtractOptions extract;
    bool rejected = false;
    try {
        (void)superzip::verify_suzip(archive, extract);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify `.suzip` rejects a footer whose index offset points outside the archive file.
// Inputs: A valid archive with the footer index offset overwritten beyond EOF.
// Outputs: Throws if verification trusts the corrupt footer.
TEST_CASE(suzip_verify_rejects_index_offset_outside_file) {
    const auto root = test_temp_dir("suzip-index-outside");
    const auto input = root / "file.bin";
    std::ofstream(input, std::ios::binary) << "payload";
    const auto archive = root / "archive.suzip";
    superzip::CompressOptions compress;
    compress.gpu_required = false;
    (void)superzip::compress_suzip({input}, archive, compress);
    const auto size = std::filesystem::file_size(archive);
    write_u64_at(archive, static_cast<std::streamoff>(size - 16), size + 1024);
    superzip::ExtractOptions extract;
    bool rejected = false;
    try {
        (void)superzip::verify_suzip(archive, extract);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify `.suzip` rejects an archive footer with a corrupt magic value.
// Inputs: A valid archive whose footer magic byte is modified.
// Outputs: Throws if verification accepts the corrupt footer.
TEST_CASE(suzip_verify_rejects_corrupt_footer_magic) {
    const auto root = test_temp_dir("suzip-footer-magic");
    const auto input = root / "file.bin";
    const std::string payload(4096, 'x');
    std::ofstream(input, std::ios::binary).write(payload.data(), static_cast<std::streamsize>(payload.size()));
    const auto archive = root / "archive.suzip";
    superzip::CompressOptions compress;
    compress.gpu_required = false;
    (void)superzip::compress_suzip({input}, archive, compress);
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
        (void)superzip::verify_suzip(archive, extract);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify impossible archive-index entry counts are rejected before large allocations.
// Inputs: A seekable in-memory index header whose declared entry count cannot fit in the remaining bytes.
// Outputs: Throws `ArchiveError` instead of reserving attacker-controlled entry capacity.
TEST_CASE(suzip_index_parser_rejects_entry_count_exceeding_buffer) {
    std::stringstream input(std::ios::in | std::ios::out | std::ios::binary);
    superzip::write_u32(input, superzip::kSuperZipMagic);
    superzip::write_u32(input, superzip::kSuperZipVersion);
    superzip::write_u32(input, 1'000'000U);
    input.seekg(0, std::ios::beg);

    bool rejected = false;
    try {
        (void)superzip::read_archive_index(input);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify impossible per-entry block counts are rejected before large allocations.
// Inputs: A seekable in-memory index with one file entry declaring more blocks than the bytes can encode.
// Outputs: Throws `ArchiveError` before reserving attacker-controlled block capacity.
TEST_CASE(suzip_index_parser_rejects_block_count_exceeding_buffer) {
    std::stringstream input(std::ios::in | std::ios::out | std::ios::binary);
    superzip::write_u32(input, superzip::kSuperZipMagic);
    superzip::write_u32(input, superzip::kSuperZipVersion);
    superzip::write_u32(input, 1U);
    superzip::write_u16(input, 1U);
    input.put('a');
    input.put('\0');
    superzip::write_u64(input, 0U);
    superzip::write_u64(input, 0U);
    superzip::write_u64(input, 0U);
    superzip::write_u32(input, 0U);
    superzip::write_u32(input, 4'000'000U);
    input.seekg(0, std::ios::beg);

    bool rejected = false;
    try {
        (void)superzip::read_archive_index(input);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify compression rejects invalid resource options before filesystem work begins.
// Inputs: A small source file and a zero chunk size that would otherwise risk an infinite streaming loop.
// Outputs: Throws if invalid chunking options are not rejected.
TEST_CASE(suzip_rejects_zero_chunk_size) {
    const auto root = test_temp_dir("suzip-zero-chunk");
    const auto input = root / "input.bin";
    std::ofstream(input, std::ios::binary) << "bounded resources";
    superzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.chunk_size = 0;

    bool rejected = false;
    try {
        (void)superzip::compress_suzip({input}, root / "archive.suzip", compress);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify compression rejects invalid deflate levels before archive output is created.
// Inputs: A small source file and a compression level outside the supported miniz range.
// Outputs: Throws if invalid compression effort is accepted.
TEST_CASE(suzip_rejects_invalid_compression_level) {
    const auto root = test_temp_dir("suzip-invalid-compression-level");
    const auto input = root / "input.bin";
    std::ofstream(input, std::ios::binary) << "bounded compression level";
    superzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.compression_level = 10;

    bool rejected = false;
    try {
        (void)superzip::compress_suzip({input}, root / "archive.suzip", compress);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify explicit in-flight chunk settings cannot bypass the archive memory guard.
// Inputs: A small source file and an in-flight chunk count above the supported ceiling.
// Outputs: Throws if compression accepts an unbounded buffering request.
TEST_CASE(suzip_rejects_excessive_inflight_chunks) {
    const auto root = test_temp_dir("suzip-excessive-inflight");
    const auto input = root / "input.bin";
    std::ofstream(input, std::ios::binary) << "bounded in-flight chunks";
    superzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.max_inflight_chunks = superzip::kMaxInflightArchiveChunks + 1U;

    bool rejected = false;
    try {
        (void)superzip::compress_suzip({input}, root / "archive.suzip", compress);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify sparse raw payload metadata is rejected before allocating a large payload window.
// Inputs: A handcrafted archive whose second raw block starts far after the first raw block.
// Outputs: Throws if verification accepts sparse or overlapping raw block layout.
TEST_CASE(suzip_verify_rejects_sparse_raw_payload_layout) {
    const auto root = test_temp_dir("suzip-sparse-payload");
    const auto archive = root / "archive.suzip";
    {
        std::ofstream file(archive, std::ios::binary | std::ios::trunc);
        constexpr std::uint32_t block_size = superzip::kDefaultArchiveBlockBytes;
        const std::string payload((static_cast<std::size_t>(block_size) * 2U) + 1U, 'x');
        file.write(payload.data(), static_cast<std::streamsize>(payload.size()));

        superzip::ArchiveIndex index;
        superzip::ArchiveEntry entry;
        entry.path = "file.bin";
        entry.uncompressed_size = static_cast<std::uint64_t>(block_size) * 2U;
        entry.payload_offset = 0;
        entry.payload_size = payload.size();
        entry.blocks.push_back(superzip::BlockDescriptor{
            .kind = superzip::BlockKind::Raw,
            .fill_value = 0,
            .uncompressed_len = block_size,
            .encoded_offset = 0,
            .encoded_len = block_size,
        });
        entry.blocks.push_back(superzip::BlockDescriptor{
            .kind = superzip::BlockKind::Raw,
            .fill_value = 0,
            .uncompressed_len = block_size,
            .encoded_offset = static_cast<std::uint64_t>(block_size) + 1U,
            .encoded_len = block_size,
        });
        index.entries.push_back(std::move(entry));
        const auto index_offset = static_cast<std::uint64_t>(file.tellp());
        superzip::write_archive_index(file, index);
        const auto index_size = static_cast<std::uint64_t>(file.tellp()) - index_offset;
        write_test_footer(file, index_offset, index_size);
    }

    superzip::ExtractOptions verify;
    verify.gpu_required = false;
    bool rejected = false;
    try {
        (void)superzip::verify_suzip(archive, verify);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify CPU extraction can read compact pattern blocks emitted by the AMD HIP encoder.
// Inputs: A handcrafted `.suzip` archive with one repeated four-byte pattern block.
// Outputs: Throws if forced-CPU extraction fails or expands incorrect content.
TEST_CASE(suzip_cpu_extracts_gpu_pattern_block) {
    const auto root = test_temp_dir("suzip-pattern-block");
    const auto archive = root / "archive.suzip";
    const std::array<std::byte, 4> pattern{
        std::byte{0x41},
        std::byte{0x42},
        std::byte{0x43},
        std::byte{0x44},
    };
    std::vector<std::byte> expanded(4096);
    for (std::size_t i = 0; i < expanded.size(); ++i) {
        expanded[i] = pattern[i % pattern.size()];
    }

    {
        std::ofstream file(archive, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(pattern.data()), static_cast<std::streamsize>(pattern.size()));

        superzip::ArchiveIndex index;
        superzip::ArchiveEntry entry;
        entry.path = "pattern.bin";
        entry.uncompressed_size = expanded.size();
        entry.payload_offset = 0;
        entry.payload_size = pattern.size();
        entry.crc32 = superzip::crc32(std::span<const std::byte>(expanded.data(), expanded.size()));
        entry.blocks.push_back(superzip::BlockDescriptor{
            .kind = superzip::BlockKind::Pattern,
            .fill_value = 0,
            .uncompressed_len = static_cast<std::uint32_t>(expanded.size()),
            .encoded_offset = 0,
            .encoded_len = static_cast<std::uint32_t>(pattern.size()),
        });
        index.entries.push_back(std::move(entry));
        const auto index_offset = static_cast<std::uint64_t>(file.tellp());
        superzip::write_archive_index(file, index);
        const auto index_size = static_cast<std::uint64_t>(file.tellp()) - index_offset;
        write_test_footer(file, index_offset, index_size);
    }

    const auto output = root / "out";
    superzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.force_cpu = true;
    extract.overwrite = true;
    (void)superzip::extract_suzip(archive, output, extract);
    std::ifstream restored(output / "pattern.bin", std::ios::binary);
    std::vector<std::byte> actual(expanded.size());
    restored.read(reinterpret_cast<char*>(actual.data()), static_cast<std::streamsize>(actual.size()));
    REQUIRE_EQ(restored.gcount(), static_cast<std::streamsize>(expanded.size()));
    restored.close();
    REQUIRE_TRUE(actual == expanded);
    std::filesystem::remove_all(root);
}

// Purpose: Verify invalid compact pattern metadata is rejected before decode.
// Inputs: A handcrafted archive whose pattern payload length is too small to be a valid repeated pattern.
// Outputs: Throws if verification accepts malformed GPU pattern metadata.
TEST_CASE(suzip_verify_rejects_invalid_gpu_pattern_metadata) {
    const auto root = test_temp_dir("suzip-invalid-pattern-block");
    const auto archive = root / "archive.suzip";
    {
        std::ofstream file(archive, std::ios::binary | std::ios::trunc);
        const char pattern = 'x';
        file.write(&pattern, 1);

        superzip::ArchiveIndex index;
        superzip::ArchiveEntry entry;
        entry.path = "pattern.bin";
        entry.uncompressed_size = 4096;
        entry.payload_offset = 0;
        entry.payload_size = 1;
        entry.blocks.push_back(superzip::BlockDescriptor{
            .kind = superzip::BlockKind::Pattern,
            .fill_value = 0,
            .uncompressed_len = 4096,
            .encoded_offset = 0,
            .encoded_len = 1,
        });
        index.entries.push_back(std::move(entry));
        const auto index_offset = static_cast<std::uint64_t>(file.tellp());
        superzip::write_archive_index(file, index);
        const auto index_size = static_cast<std::uint64_t>(file.tellp()) - index_offset;
        write_test_footer(file, index_offset, index_size);
    }

    superzip::ExtractOptions verify;
    verify.gpu_required = false;
    verify.force_cpu = true;
    bool rejected = false;
    try {
        (void)superzip::verify_suzip(archive, verify);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}
