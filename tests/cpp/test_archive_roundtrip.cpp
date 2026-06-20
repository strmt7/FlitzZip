#include "core/archive_index.hpp"
#include "core/archive.hpp"
#include "core/checksum.hpp"
#include "core/result.hpp"
#include "core/resource_limits.hpp"
#include "test_suzip_helpers.hpp"
#include "test_util.hpp"

#include <fstream>
#include <span>
#include <string>
#include <vector>
#include <windows.h>

namespace {

using namespace superzip_test;

struct RawArchiveTestEntry {
    std::string path;
    std::string payload;
};

// Purpose: Write a compact handcrafted SUZIP archive with raw file entries.
// Inputs: `path` is the archive to create and `entries` supplies normalized or intentionally malformed entry names plus
// payload bytes. Outputs: Writes payloads, index, and footer so validation tests can exercise archive metadata without
// relying on production compression.
void write_raw_test_archive(const std::filesystem::path& path, const std::vector<RawArchiveTestEntry>& entries) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    superzip::ArchiveIndex index;
    for (const auto& source : entries) {
        superzip::ArchiveEntry entry;
        entry.path = source.path;
        entry.uncompressed_size = source.payload.size();
        entry.payload_offset = static_cast<std::uint64_t>(file.tellp());
        entry.payload_size = source.payload.size();
        entry.crc32 =
            superzip::crc32(std::as_bytes(std::span<const char>(source.payload.data(), source.payload.size())));
        file.write(source.payload.data(), static_cast<std::streamsize>(source.payload.size()));
        entry.blocks.push_back(superzip::BlockDescriptor{
            .kind = superzip::BlockKind::Raw,
            .fill_value = 0,
            .uncompressed_len = static_cast<std::uint32_t>(source.payload.size()),
            .encoded_offset = 0,
            .encoded_len = static_cast<std::uint32_t>(source.payload.size()),
        });
        index.entries.push_back(std::move(entry));
    }
    const auto index_offset = static_cast<std::uint64_t>(file.tellp());
    superzip::write_archive_index(file, index);
    const auto index_size = static_cast<std::uint64_t>(file.tellp()) - index_offset;
    write_test_footer(file, index_offset, index_size);
}

}  // namespace

// Purpose: Verify every product block-size option is a real compression setting.
// Inputs: A deterministic 3 MiB file compressed with each offered block size.
// Outputs: Extracted payload matches the source and archive metadata never exceeds the selected block size.
TEST_CASE(suzip_supported_block_sizes_roundtrip_and_bound_metadata) {
    const auto root = test_temp_dir("suzip-block-sizes");
    const auto source = root / "source";
    std::filesystem::create_directories(source);
    const auto input = source / "payload.bin";
    {
        std::ofstream out(input, std::ios::binary);
        for (std::size_t i = 0; i < (3U * 1024U * 1024U); ++i) {
            const auto value = static_cast<unsigned char>((i * 131U + (i / 17U)) & 0xFFU);
            out.put(static_cast<char>(value));
        }
    }

    constexpr std::array<std::uint32_t, 7> block_sizes{
        256U * 1024U,       512U * 1024U,       superzip::kDefaultArchiveBlockBytes, 2U * 1024U * 1024U,
        4U * 1024U * 1024U, 8U * 1024U * 1024U, superzip::kMaxArchiveBlockBytes,
    };
    for (const auto block_size : block_sizes) {
        const auto archive = root / ("archive-" + std::to_string(block_size) + ".suzip");
        const auto output = root / ("out-" + std::to_string(block_size));
        superzip::CompressOptions compress;
        compress.force_cpu = true;
        compress.gpu_required = false;
        compress.block_size = block_size;
        const auto compressed = superzip::compress_suzip({source}, archive, compress);
        REQUIRE_TRUE(!compressed.gpu_used);

        const auto index = read_test_archive_index(archive);
        bool saw_payload = false;
        for (const auto& entry : index.entries) {
            if (entry.directory) {
                continue;
            }
            saw_payload = true;
            for (const auto& block : entry.blocks) {
                REQUIRE_TRUE(block.uncompressed_len <= block_size);
            }
        }
        REQUIRE_TRUE(saw_payload);

        superzip::ExtractOptions extract;
        extract.force_cpu = true;
        extract.gpu_required = false;
        const auto extracted = superzip::extract_suzip(archive, output, extract);
        REQUIRE_TRUE(!extracted.gpu_used);
        const auto restored = output / "source" / "payload.bin";
        REQUIRE_TRUE(std::filesystem::exists(restored));
        REQUIRE_EQ(std::filesystem::file_size(restored), std::filesystem::file_size(input));
    }
    std::filesystem::remove_all(root);
}

// Purpose: Verify the production default compression level is balanced, not fastest-only.
// Inputs: A repetitive temporary payload compressed with explicit level 1 and default options.
// Outputs: Default compression must not produce a larger archive than explicit fastest mode.
TEST_CASE(suzip_default_compression_level_is_balanced) {
    const auto root = test_temp_dir("suzip-default-level");
    const auto input = root / "repetitive.txt";
    {
        std::ofstream out(input, std::ios::binary);
        for (int i = 0; i < 120000; ++i) {
            out << "SuperZip compression level regression payload " << (i % 17) << "\n";
        }
    }

    superzip::CompressOptions fastest;
    fastest.force_cpu = true;
    fastest.gpu_required = false;
    fastest.compression_level = superzip::kMinCompressionLevel;
    const auto fastest_archive = root / "fastest.suzip";
    const auto fastest_stats = superzip::compress_suzip({input}, fastest_archive, fastest);

    superzip::CompressOptions balanced;
    balanced.force_cpu = true;
    balanced.gpu_required = false;
    const auto balanced_archive = root / "balanced.suzip";
    const auto balanced_stats = superzip::compress_suzip({input}, balanced_archive, balanced);

    REQUIRE_TRUE(balanced.compression_level == superzip::kDefaultCompressionLevel);
    REQUIRE_TRUE(balanced_stats.output_bytes <= fastest_stats.output_bytes);
    std::filesystem::remove_all(root);
}

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
    REQUIRE_EQ(std::filesystem::file_size(output / "input" / "nested" / "pattern.bin"),
               static_cast<std::uintmax_t>(200000));
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

// Purpose: Verify extraction handles archives whose encoded block size is larger than the runtime chunk preference.
// Inputs: A force-CPU archive written with 64 KiB blocks and extracted with 4 KiB runtime chunks.
// Outputs: Throws if decode memory budgeting rejects the archive or restores different bytes.
TEST_CASE(suzip_extract_supports_smaller_runtime_chunk_than_archive_block) {
    const auto root = test_temp_dir("suzip-small-runtime-chunk");
    const auto input = root / "input.bin";
    std::vector<char> payload(96 * 1024);
    std::uint32_t state = 0x31415926U;
    for (auto& byte : payload) {
        state = (state * 1664525U) + 1013904223U;
        byte = static_cast<char>((state >> 24U) & 0xFFU);
    }
    std::ofstream(input, std::ios::binary).write(payload.data(), static_cast<std::streamsize>(payload.size()));

    const auto archive = root / "archive.suzip";
    superzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.force_cpu = true;
    compress.chunk_size = 128 * 1024;
    compress.block_size = 64 * 1024;
    (void)superzip::compress_suzip({input}, archive, compress);

    superzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.force_cpu = true;
    extract.overwrite = true;
    extract.chunk_size = superzip::kMinArchiveBlockBytes;
    extract.block_size = superzip::kMinArchiveBlockBytes;
    const auto verified = superzip::verify_suzip(archive, extract);
    REQUIRE_EQ(verified.output_bytes, static_cast<std::uint64_t>(payload.size()));

    const auto output = root / "out";
    (void)superzip::extract_suzip(archive, output, extract);
    std::ifstream restored(output / "input.bin", std::ios::binary);
    std::vector<char> actual(payload.size());
    restored.read(actual.data(), static_cast<std::streamsize>(actual.size()));
    REQUIRE_EQ(restored.gcount(), static_cast<std::streamsize>(payload.size()));
    restored.close();
    REQUIRE_TRUE(actual == payload);
    std::filesystem::remove_all(root);
}

// Purpose: Verify optional GPU fallback does not publish failed HIP attempts as completed GPU telemetry.
// Inputs: A small source file compressed with optional GPU use on hosts with no available AMD GPU.
// Outputs: Throws if CPU fallback reports GPU usage, chunks, kernels, transfer bytes, or device allocations.
TEST_CASE(suzip_optional_gpu_fallback_reports_zero_gpu_telemetry_without_device) {
    if (superzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("suzip-optional-gpu-fallback-telemetry");
    const auto input = root / "input.bin";
    {
        std::ofstream out(input, std::ios::binary);
        for (int i = 0; i < 8192; ++i) {
            out << "optional gpu fallback telemetry should stay CPU-visible only\n";
        }
    }
    const auto archive = root / "archive.suzip";

    superzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.force_cpu = false;
    compress.verify_after_write = true;
    const auto compressed = superzip::compress_suzip({input}, archive, compress);
    REQUIRE_TRUE(!compressed.gpu_used);
    REQUIRE_EQ(compressed.gpu_runtime.encode_chunks, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(compressed.gpu_runtime.decode_chunks, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(compressed.gpu_runtime.kernel_launches, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(compressed.gpu_runtime.h2d_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(compressed.gpu_runtime.d2h_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(compressed.gpu_runtime.device_allocation_bytes, static_cast<std::uint64_t>(0));

    superzip::ExtractOptions verify;
    verify.gpu_required = false;
    verify.force_cpu = false;
    const auto verified = superzip::verify_suzip(archive, verify);
    REQUIRE_TRUE(!verified.gpu_used);
    REQUIRE_EQ(verified.gpu_runtime.encode_chunks, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(verified.gpu_runtime.decode_chunks, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(verified.gpu_runtime.kernel_launches, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(verified.gpu_runtime.h2d_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(verified.gpu_runtime.d2h_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(verified.gpu_runtime.device_allocation_bytes, static_cast<std::uint64_t>(0));

    const auto output = root / "out";
    superzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.force_cpu = false;
    extract.overwrite = true;
    const auto extracted = superzip::extract_suzip(archive, output, extract);
    REQUIRE_TRUE(!extracted.gpu_used);
    REQUIRE_EQ(extracted.gpu_runtime.encode_chunks, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(extracted.gpu_runtime.decode_chunks, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(extracted.gpu_runtime.kernel_launches, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(extracted.gpu_runtime.h2d_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(extracted.gpu_runtime.d2h_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(extracted.gpu_runtime.device_allocation_bytes, static_cast<std::uint64_t>(0));

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

// Purpose: Verify the required-HIP encoder produces GPU-supported SUZIP blocks without CPU deflate.
// Inputs: A repetitive payload compressed with `gpu_required` on an AMD HIP host.
// Outputs: Throws if required-HIP compression emits deflate blocks or cannot verify/extract through HIP.
TEST_CASE(suzip_required_gpu_encoder_emits_no_cpu_deflate_blocks) {
    if (!superzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("suzip-required-gpu-no-deflate");
    const auto input = root / "gpu.txt";
    {
        std::ofstream out(input, std::ios::binary);
        for (int i = 0; i < 32768; ++i) {
            out << "SuperZip required HIP codec should stay separate from CPU deflate.\n";
        }
    }
    const auto archive = root / "archive.suzip";

    superzip::CompressOptions compress;
    compress.gpu_required = true;
    compress.force_cpu = false;
    compress.chunk_size = 128 * 1024;
    compress.block_size = 64 * 1024;
    compress.verify_after_write = true;
    const auto compressed = superzip::compress_suzip({input}, archive, compress);
    REQUIRE_TRUE(compressed.gpu_used);
    REQUIRE_TRUE(compressed.gpu_runtime.encode_chunks > 0);
    REQUIRE_TRUE(compressed.gpu_runtime.kernel_launches > 0);

    const auto index = read_test_archive_index(archive);
    REQUIRE_TRUE(!archive_contains_block_kind(index, superzip::BlockKind::Deflate));

    superzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 128 * 1024;
    verify.block_size = 64 * 1024;
    const auto verified = superzip::verify_suzip(archive, verify);
    REQUIRE_TRUE(verified.gpu_used);

    const auto output = root / "out";
    verify.overwrite = true;
    const auto extracted = superzip::extract_suzip(archive, output, verify);
    REQUIRE_TRUE(extracted.gpu_used);
    REQUIRE_EQ(std::filesystem::file_size(output / "gpu.txt"), std::filesystem::file_size(input));
    std::filesystem::remove_all(root);
}

// Purpose: Verify required-HIP verification refuses archives that need CPU deflate.
// Inputs: A force-CPU text archive that intentionally contains miniz deflate blocks.
// Outputs: Throws if `gpu_required` silently invokes the CPU deflate codec.
TEST_CASE(suzip_required_gpu_rejects_cpu_deflate_archive) {
    if (!superzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("suzip-required-gpu-rejects-deflate");
    const auto input = root / "cpu-deflate.txt";
    {
        std::ofstream out(input, std::ios::binary);
        for (int i = 0; i < 32768; ++i) {
            out << "CPU deflate archive block for required HIP rejection.\n";
        }
    }
    const auto archive = root / "archive.suzip";

    superzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.force_cpu = true;
    compress.chunk_size = 128 * 1024;
    compress.block_size = 64 * 1024;
    const auto compressed = superzip::compress_suzip({input}, archive, compress);
    REQUIRE_TRUE(!compressed.gpu_used);
    REQUIRE_TRUE(compressed.output_bytes < std::filesystem::file_size(input) / 4U);

    const auto index = read_test_archive_index(archive);
    REQUIRE_TRUE(archive_contains_block_kind(index, superzip::BlockKind::Deflate));

    superzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 128 * 1024;
    verify.block_size = 64 * 1024;
    bool rejected = false;
    try {
        (void)superzip::verify_suzip(archive, verify);
    } catch (const superzip::GpuError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);

    const auto output = root / "out";
    verify.overwrite = true;
    rejected = false;
    try {
        (void)superzip::extract_suzip(archive, output, verify);
    } catch (const superzip::GpuError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(output / "cpu-deflate.txt"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
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

// Purpose: Verify failed extraction removes decoded temporary data before publishing a final file.
// Inputs: A raw-payload archive with one payload byte modified to force a CRC mismatch after decode.
// Outputs: Throws if extraction succeeds or leaves a partial target file behind.
TEST_CASE(suzip_extract_removes_partial_file_after_crc_failure) {
    const auto root = test_temp_dir("suzip-extract-crc-cleanup");
    const auto input = root / "file.bin";
    {
        std::vector<char> payload(64 * 1024);
        std::uint32_t state = 0x12345678U;
        for (std::size_t i = 0; i < payload.size(); ++i) {
            state = (state * 1664525U) + 1013904223U;
            payload[i] = static_cast<char>((state >> 24U) & 0xFFU);
        }
        std::ofstream(input, std::ios::binary).write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }
    const auto archive = root / "archive.suzip";
    superzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.force_cpu = true;
    compress.chunk_size = 64 * 1024;
    compress.block_size = 16 * 1024;
    (void)superzip::compress_suzip({input}, archive, compress);

    const auto index = read_test_archive_index(archive);
    REQUIRE_TRUE(archive_contains_block_kind(index, superzip::BlockKind::Raw));
    {
        std::fstream file(archive, std::ios::binary | std::ios::in | std::ios::out);
        char value = 0;
        file.seekg(0, std::ios::beg);
        file.read(&value, 1);
        value ^= 0x7F;
        file.seekp(0, std::ios::beg);
        file.write(&value, 1);
    }

    const auto output = root / "out";
    superzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.force_cpu = true;
    extract.overwrite = true;
    extract.chunk_size = 64 * 1024;
    extract.block_size = 16 * 1024;
    bool rejected = false;
    try {
        (void)superzip::extract_suzip(archive, output, extract);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(output / "file.bin"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));

    const auto overwrite_output = root / "overwrite-out";
    std::filesystem::create_directories(overwrite_output);
    const std::string preserved_text = "existing output should survive failed extraction";
    std::ofstream(overwrite_output / "file.bin", std::ios::binary) << preserved_text;
    rejected = false;
    try {
        (void)superzip::extract_suzip(archive, overwrite_output, extract);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(overwrite_output), static_cast<std::uint64_t>(1));
    REQUIRE_EQ(std::filesystem::file_size(overwrite_output / "file.bin"),
               static_cast<std::uintmax_t>(preserved_text.size()));
    std::ifstream preserved(overwrite_output / "file.bin", std::ios::binary);
    std::string actual(preserved_text.size(), '\0');
    preserved.read(actual.data(), static_cast<std::streamsize>(actual.size()));
    preserved.close();
    REQUIRE_EQ(actual, preserved_text);
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

// Purpose: Verify `.suzip` rejects Win32-disallowed control characters in entry paths before extraction.
// Inputs: A handcrafted archive with one raw entry whose name contains ASCII 0x1F.
// Outputs: Throws `SecurityError` before decoding or writing any file.
TEST_CASE(suzip_verify_rejects_control_character_entry_path) {
    const auto root = test_temp_dir("suzip-control-char-path");
    const auto archive = root / "archive.suzip";
    const std::string payload = "payload";
    {
        std::ofstream file(archive, std::ios::binary | std::ios::trunc);
        file.write(payload.data(), static_cast<std::streamsize>(payload.size()));

        superzip::ArchiveIndex index;
        superzip::ArchiveEntry entry;
        entry.path = std::string("dir/control") + static_cast<char>(0x1F) + ".txt";
        entry.uncompressed_size = payload.size();
        entry.payload_offset = 0;
        entry.payload_size = payload.size();
        entry.crc32 = superzip::crc32(std::as_bytes(std::span<const char>(payload.data(), payload.size())));
        entry.blocks.push_back(superzip::BlockDescriptor{
            .kind = superzip::BlockKind::Raw,
            .fill_value = 0,
            .uncompressed_len = static_cast<std::uint32_t>(payload.size()),
            .encoded_offset = 0,
            .encoded_len = static_cast<std::uint32_t>(payload.size()),
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
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify archive-wide validation rejects duplicate normalized entry paths before payload decode.
// Inputs: A handcrafted archive containing `dir/file.txt` and the equivalent `dir//file.txt`.
// Outputs: Throws `SecurityError` during verification instead of accepting ambiguous extraction metadata.
TEST_CASE(suzip_verify_rejects_duplicate_normalized_entry_paths) {
    const auto root = test_temp_dir("suzip-duplicate-paths");
    const auto archive = root / "archive.suzip";
    write_raw_test_archive(archive, {
                                        RawArchiveTestEntry{.path = "dir/file.txt", .payload = "first"},
                                        RawArchiveTestEntry{.path = "dir//file.txt", .payload = "second"},
                                    });

    superzip::ExtractOptions verify;
    verify.gpu_required = false;
    verify.force_cpu = true;
    bool rejected = false;
    try {
        (void)superzip::verify_suzip(archive, verify);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify archive-wide validation rejects a file entry that blocks a child entry path.
// Inputs: A handcrafted archive containing file `dir` and child file `dir/child.txt`.
// Outputs: Throws before extraction creates any output path or decodes payload bytes.
TEST_CASE(suzip_extract_rejects_file_entry_with_child_entry) {
    const auto root = test_temp_dir("suzip-file-child-conflict");
    const auto archive = root / "archive.suzip";
    write_raw_test_archive(archive, {
                                        RawArchiveTestEntry{.path = "dir", .payload = "parent"},
                                        RawArchiveTestEntry{.path = "dir/child.txt", .payload = "child"},
                                    });

    const auto output = root / "out";
    superzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.force_cpu = true;
    extract.overwrite = true;
    bool rejected = false;
    try {
        (void)superzip::extract_suzip(archive, output, extract);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
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
