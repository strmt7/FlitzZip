#include "core/archive_index.hpp"
#include "core/archive.hpp"
#include "core/checksum.hpp"
#include "core/compound_block.hpp"
#include "core/file_publish.hpp"
#include "core/windows_handle.hpp"
#include "core/host_memory_budget.hpp"
#include "core/result.hpp"
#include "core/resource_limits.hpp"
#include "core/sparse_pattern_block.hpp"
#include "gpu/gpu_codec.hpp"
#include "zstd/zstd_runtime.hpp"
#include "lz4.h"
#include "miniz.h"
#include "test_flzip_helpers.hpp"
#include "test_util.hpp"
#include "test_source_session.hpp"
#include "zip/zip_adapter.hpp"

#include <cmath>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <iostream>
#include <iterator>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>

namespace {

using namespace flitzzip_test;

struct RawArchiveTestEntry {
    std::string path;
    std::string payload;
};

// Purpose: Write a compact handcrafted FLZIP archive with raw file entries.
// Inputs: `path`, `entries`, and `version` supply archive contents and its declared native version.
// Outputs: Writes payloads, index, and footer for metadata validation without production compression.
void write_raw_test_archive(const std::filesystem::path& path, const std::vector<RawArchiveTestEntry>& entries,
                            std::uint32_t version = flitzzip::kFlitzZipVersion) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    flitzzip::ArchiveIndex index;
    index.version = version;
    for (const auto& source : entries) {
        flitzzip::ArchiveEntry entry;
        entry.path = source.path;
        entry.uncompressed_size = source.payload.size();
        entry.payload_offset = static_cast<std::uint64_t>(file.tellp());
        entry.payload_size = source.payload.size();
        entry.crc32 =
            flitzzip::crc32(std::as_bytes(std::span<const char>(source.payload.data(), source.payload.size())));
        file.write(source.payload.data(), static_cast<std::streamsize>(source.payload.size()));
        entry.blocks.push_back(flitzzip::BlockDescriptor{
            .kind = flitzzip::BlockKind::Raw,
            .fill_value = 0,
            .uncompressed_len = static_cast<std::uint32_t>(source.payload.size()),
            .encoded_offset = 0,
            .encoded_len = static_cast<std::uint32_t>(source.payload.size()),
        });
        index.entries.push_back(std::move(entry));
    }
    const auto index_offset = static_cast<std::uint64_t>(file.tellp());
    flitzzip::write_archive_index(file, index);
    const auto index_size = static_cast<std::uint64_t>(file.tellp()) - index_offset;
    write_test_footer(file, index_offset, index_size, version);
}

// Purpose: Wrap one pre-encoded block in a native archive with an explicit format version.
// Inputs: `path` is the output file, `payload` contains one block, and `decoded` supplies its expected CRC data.
// Outputs: Writes a complete one-file archive for public read-path validation.
void write_encoded_test_archive(const std::filesystem::path& path, std::span<const std::byte> payload,
                                std::string_view decoded, flitzzip::BlockKind kind, std::uint32_t version) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    flitzzip::ArchiveIndex index;
    index.version = version;
    flitzzip::ArchiveEntry entry;
    entry.path = "data.bin";
    entry.uncompressed_size = decoded.size();
    entry.payload_size = payload.size();
    entry.crc32 = flitzzip::crc32(std::as_bytes(std::span(decoded.data(), decoded.size())));
    entry.blocks.push_back(flitzzip::BlockDescriptor{
        .kind = kind,
        .uncompressed_len = static_cast<std::uint32_t>(decoded.size()),
        .encoded_len = static_cast<std::uint32_t>(payload.size()),
    });
    index.entries.push_back(std::move(entry));
    const auto index_offset = static_cast<std::uint64_t>(file.tellp());
    flitzzip::write_archive_index(file, index);
    const auto index_size = static_cast<std::uint64_t>(file.tellp()) - index_offset;
    write_test_footer(file, index_offset, index_size, index.version);
}

// Purpose: Wrap one pre-encoded dictionary payload in a version-four native archive.
// Inputs: `path` is the output file, `payload` includes its segment table, and `decoded` supplies expected CRC data.
// Outputs: Writes a complete one-file archive for public read-path validation.
void write_dictionary_test_archive(const std::filesystem::path& path, std::span<const std::byte> payload,
                                   std::string_view decoded) {
    write_encoded_test_archive(path, payload, decoded, flitzzip::BlockKind::GpuDictionary, 4U);
}

}  // namespace

// Purpose: Require the native footer and index to declare the same readable format version.
// Inputs: Raw archives at every readable version, followed by one archive with a changed footer version.
// Outputs: Valid archives verify and a version mismatch is rejected before payload decoding.
TEST_CASE(flzip_rejects_footer_index_version_mismatch) {
    const auto root = test_temp_dir("flzip-version-consistency");
    flitzzip::ExtractOptions options;
    options.force_cpu = true;
    options.gpu_required = false;
    for (std::uint32_t version = flitzzip::kFlitzZipMinReadableVersion;
         version <= flitzzip::kFlitzZipMaxReadableVersion; ++version) {
        const auto archive = root / ("version-" + std::to_string(version) + ".flzip");
        write_raw_test_archive(archive, {{"data.txt", "test payload"}}, version);
        REQUIRE_EQ(flitzzip::verify_flzip(archive, options).entries, 1U);
    }

    const auto archive = root / ("version-" + std::to_string(flitzzip::kFlitzZipVersion) + ".flzip");
    const auto footer_version_offset = static_cast<std::streamoff>(std::filesystem::file_size(archive) - 20U);
    for (const auto invalid_version :
         {flitzzip::kFlitzZipMinReadableVersion - 1U, flitzzip::kFlitzZipMaxReadableVersion + 1U}) {
        {
            std::fstream file(archive, std::ios::binary | std::ios::in | std::ios::out);
            file.seekp(footer_version_offset, std::ios::beg);
            flitzzip::write_u32(file, invalid_version);
            REQUIRE_TRUE(static_cast<bool>(file));
        }
        bool unsupported = false;
        try {
            static_cast<void>(flitzzip::verify_flzip(archive, options));
        } catch (const flitzzip::ArchiveError& error) {
            unsupported = std::string(error.what()).find("unsupported archive footer version") != std::string::npos;
        }
        REQUIRE_TRUE(unsupported);
    }
    {
        std::fstream file(archive, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(footer_version_offset, std::ios::beg);
        flitzzip::write_u32(file, flitzzip::kFlitzZipVersion - 1U);
        REQUIRE_TRUE(static_cast<bool>(file));
    }
    bool rejected = false;
    try {
        static_cast<void>(flitzzip::verify_flzip(archive, options));
    } catch (const flitzzip::ArchiveError& error) {
        rejected = std::string(error.what()).find("footer and index versions differ") != std::string::npos;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Prevent older native versions from claiming block encodings they do not define.
// Inputs: Prefix, dictionary, sparse, Zstandard, and long-sparse descriptors before their introduction.
// Outputs: Both the writer and parser reject unsupported version/block combinations.
TEST_CASE(flzip_rejects_block_kind_version_downgrade) {
    for (const auto [version, kind] :
         {std::pair{1U, flitzzip::BlockKind::GpuPrefix}, std::pair{1U, flitzzip::BlockKind::GpuAdaptivePrefix},
          std::pair{2U, flitzzip::BlockKind::GpuAdaptivePrefix}, std::pair{3U, flitzzip::BlockKind::GpuDictionary},
          std::pair{4U, flitzzip::BlockKind::GpuSparsePattern}, std::pair{5U, flitzzip::BlockKind::CpuZstd},
          std::pair{6U, flitzzip::BlockKind::GpuLongSparsePattern}, std::pair{7U, flitzzip::BlockKind::GpuHuffman}}) {
        flitzzip::ArchiveIndex index;
        index.version = version;
        flitzzip::ArchiveEntry entry;
        entry.path = "x";
        entry.blocks.push_back(flitzzip::BlockDescriptor{.kind = kind});
        index.entries.push_back(entry);

        bool writer_rejected = false;
        try {
            std::ostringstream output(std::ios::out | std::ios::binary);
            flitzzip::write_archive_index(output, index);
        } catch (const flitzzip::ArchiveError&) {
            writer_rejected = true;
        }
        REQUIRE_TRUE(writer_rejected);

        index.version = flitzzip::kFlitzZipMaxReadableVersion;
        std::ostringstream output(std::ios::out | std::ios::binary);
        flitzzip::write_archive_index(output, index);
        auto forged = output.str();
        forged[4] = static_cast<char>(version);
        std::istringstream input(forged, std::ios::in | std::ios::binary);
        bool reader_rejected = false;
        try {
            static_cast<void>(flitzzip::read_archive_index(input));
        } catch (const flitzzip::ArchiveError&) {
            reader_rejected = true;
        }
        REQUIRE_TRUE(reader_rejected);
    }
}

// Purpose: Exercise version-five sparse blocks through public CPU/HIP verification and extraction.
// Inputs: A four-byte motif, two sorted corrections, and a malformed redundant-correction variant.
// Outputs: Exact output roundtrips; noncanonical payload fails before publication.
TEST_CASE(flzip_sparse_pattern_cpu_reader_roundtrip) {
    std::string decoded(64U, '\0');
    for (std::size_t index = 0U; index < decoded.size(); ++index) {
        decoded[index] = "ABCD"[index % 4U];
    }
    decoded[5U] = 'X';
    decoded[11U] = 'Y';
    const std::vector<std::byte> payload{
        std::byte{4}, std::byte{0}, std::byte{0},   std::byte{0},   std::byte{2},   std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{'A'}, std::byte{'B'}, std::byte{'C'}, std::byte{'D'},
        std::byte{5}, std::byte{0}, std::byte{0},   std::byte{0},   std::byte{'X'}, std::byte{11},
        std::byte{0}, std::byte{0}, std::byte{0},   std::byte{'Y'},
    };
    const auto root = test_temp_dir("flzip-sparse-pattern-v5");
    const auto archive = root / "sparse.flzip";
    write_encoded_test_archive(archive, payload, decoded, flitzzip::BlockKind::GpuSparsePattern, 5U);
    REQUIRE_EQ(read_test_archive_index(archive).version, 5U);
    for (const bool hip : {false, true}) {
        if (hip && !flitzzip::query_gpu_info().available) {
            continue;
        }
        flitzzip::ExtractOptions options;
        options.force_cpu = !hip;
        options.gpu_required = hip;
        REQUIRE_EQ(flitzzip::verify_flzip(archive, options).gpu_used, hip);
        const auto destination = root / (hip ? "gpu" : "cpu");
        REQUIRE_EQ(flitzzip::extract_flzip(archive, destination, options).gpu_used, hip);
        std::ifstream restored(destination / "data.bin", std::ios::binary);
        REQUIRE_TRUE(restored.is_open());
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(restored), std::istreambuf_iterator<char>()), decoded);
    }

    auto invalid = payload;
    invalid[16U] = std::byte{'B'};
    const auto corrupt = root / "invalid.flzip";
    write_encoded_test_archive(corrupt, invalid, decoded, flitzzip::BlockKind::GpuSparsePattern, 5U);
    flitzzip::ExtractOptions options;
    options.force_cpu = true;
    options.gpu_required = false;
    bool rejected = false;
    try {
        (void)flitzzip::verify_flzip(corrupt, options);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    const auto invalid_destination = root / "invalid-output";
    rejected = false;
    try {
        (void)flitzzip::extract_flzip(corrupt, invalid_destination, options);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(invalid_destination / "data.bin"));
    std::filesystem::remove_all(root);
}

// Purpose: Prove v7 long-sparse archives remain independently readable without a GPU.
// Inputs: A 16 KiB-plus-one motif, one canonical patch, and explicit v7 metadata.
// Outputs: Public verify/extract preserve bytes on CPU and available HIP; corrupt patches fail before publication.
TEST_CASE(flzip_long_sparse_pattern_v7_reader_roundtrip) {
    constexpr std::uint32_t period = flitzzip::kMaxGpuPatternBytes + 1U;
    std::string decoded(2U * period, '\0');
    std::uint32_t state = 0xC67A349DU;
    for (std::size_t index = 0U; index < period; ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        decoded[index] = static_cast<char>(state >> 24U);
        decoded[index + period] = decoded[index];
    }
    decoded[period + 512U] = static_cast<char>(static_cast<unsigned char>(decoded[512U]) ^ 0xFFU);
    std::vector<std::byte> payload;
    const auto append_u32 = [&payload](std::uint32_t value) {
        for (std::uint32_t index = 0U; index < sizeof(value); ++index) {
            payload.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xFFU));
        }
    };
    append_u32(period);
    append_u32(1U);
    for (std::size_t index = 0U; index < period; ++index) {
        payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(decoded[index])));
    }
    append_u32(period + 512U);
    payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(decoded[period + 512U])));

    const auto root = test_temp_dir("flzip-long-sparse-v7");
    const auto archive = root / "long.flzip";
    write_encoded_test_archive(archive, payload, decoded, flitzzip::BlockKind::GpuLongSparsePattern, 7U);
    REQUIRE_EQ(read_test_archive_index(archive).version, 7U);
    for (const bool hip : {false, true}) {
        if (hip && !flitzzip::query_gpu_info().available) {
            continue;
        }
        flitzzip::ExtractOptions options;
        options.force_cpu = !hip;
        options.gpu_required = hip;
        REQUIRE_EQ(flitzzip::verify_flzip(archive, options).gpu_used, hip);
        const auto destination = root / (hip ? "gpu" : "cpu");
        REQUIRE_EQ(flitzzip::extract_flzip(archive, destination, options).gpu_used, hip);
        std::ifstream restored(destination / "data.bin", std::ios::binary);
        REQUIRE_TRUE(restored.is_open());
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(restored), std::istreambuf_iterator<char>()), decoded);
    }
    auto invalid = payload;
    invalid.back() = payload[flitzzip::kSparsePatternHeaderBytes + 512U];
    const auto corrupt = root / "invalid.flzip";
    write_encoded_test_archive(corrupt, invalid, decoded, flitzzip::BlockKind::GpuLongSparsePattern, 7U);
    flitzzip::ExtractOptions options;
    options.force_cpu = true;
    options.gpu_required = false;
    bool rejected = false;
    try {
        (void)flitzzip::verify_flzip(corrupt, options);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    const auto invalid_destination = root / "invalid-output";
    rejected = false;
    try {
        (void)flitzzip::extract_flzip(corrupt, invalid_destination, options);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(invalid_destination / "data.bin"));
    std::filesystem::remove_all(root);
}

// Purpose: Ensure the production writer promotes a long-sparse HIP archive to v7 and publishes verified bytes.
// Inputs: A 4 MiB file with a seeded 256 KiB motif and one mutation in each later record.
// Outputs: The archive has a v7 long-sparse block and CPU/HIP verification and CPU extraction preserve the file.
TEST_CASE(flzip_long_sparse_pattern_writer_selects_v7) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    constexpr std::size_t period = 256U * 1024U;
    constexpr std::size_t block_bytes = 4U * 1024U * 1024U;
    std::vector<std::byte> source(block_bytes);
    std::uint32_t state = 0xC67A349DU;
    for (std::size_t index = 0U; index < period; ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        source[index] = static_cast<std::byte>(state >> 24U);
    }
    for (std::size_t index = period; index < source.size(); ++index) {
        source[index] = source[index % period];
    }
    for (std::size_t record = 1U; record < source.size() / period; ++record) {
        source[record * period + 1024U] ^= std::byte{0xFF};
    }
    const auto root = test_temp_dir("flzip-long-sparse-writer-v7");
    const auto input = root / "source.bin";
    {
        std::ofstream file(input, std::ios::binary);
        file.write(reinterpret_cast<const char*>(source.data()), static_cast<std::streamsize>(source.size()));
        REQUIRE_TRUE(static_cast<bool>(file));
    }
    const auto archive = root / "archive.flzip";
    flitzzip::CompressOptions compress;
    compress.gpu_required = true;
    compress.chunk_size = static_cast<std::uint32_t>(block_bytes);
    compress.block_size = static_cast<std::uint32_t>(block_bytes);
    compress.verify_after_write = true;
    REQUIRE_TRUE(flitzzip::compress_flzip({input}, archive, compress).gpu_used);
    const auto index = read_test_archive_index(archive);
    REQUIRE_EQ(index.version, 7U);
    REQUIRE_TRUE(archive_contains_block_kind(index, flitzzip::BlockKind::GpuLongSparsePattern));
    for (const bool hip : {false, true}) {
        flitzzip::ExtractOptions verify;
        verify.force_cpu = !hip;
        verify.gpu_required = hip;
        REQUIRE_EQ(flitzzip::verify_flzip(archive, verify).gpu_used, hip);
    }
    flitzzip::ExtractOptions extract;
    extract.force_cpu = true;
    extract.gpu_required = false;
    const auto output = root / "output";
    REQUIRE_TRUE(!flitzzip::extract_flzip(archive, output, extract).gpu_used);
    std::ifstream restored(output / "source.bin", std::ios::binary);
    REQUIRE_TRUE(restored.is_open());
    const std::vector<char> actual{std::istreambuf_iterator<char>(restored), std::istreambuf_iterator<char>()};
    REQUIRE_EQ(actual.size(), source.size());
    REQUIRE_TRUE(std::equal(actual.begin(), actual.end(), source.begin(), [](char left, std::byte right) {
        return static_cast<unsigned char>(left) == static_cast<unsigned char>(right);
    }));
    restored.close();
    std::filesystem::remove_all(root);
}

// Purpose: Keep legacy pattern bounds intact while allowing bounded version-four motifs.
// Inputs: Pattern descriptors at the 256-byte, 257-byte, and 16 KiB boundaries.
// Outputs: The index writer and reader reject only version-incompatible or oversized descriptors.
TEST_CASE(flzip_pattern_length_version_boundaries) {
    for (const auto length : {256U, 257U, flitzzip::kMaxGpuPatternBytes, flitzzip::kMaxGpuPatternBytes + 1U}) {
        flitzzip::ArchiveIndex index;
        index.version = flitzzip::kFlitzZipMaxReadableVersion;
        flitzzip::ArchiveEntry entry;
        entry.path = "pattern.bin";
        entry.uncompressed_size = length * 2U;
        entry.blocks.push_back(flitzzip::BlockDescriptor{
            .kind = flitzzip::BlockKind::Pattern, .uncompressed_len = length * 2U, .encoded_len = length});
        index.entries.push_back(entry);
        const bool valid_v4 = length <= flitzzip::kMaxGpuPatternBytes;
        const bool valid_v3 = length <= flitzzip::kLegacyGpuPatternBytes;
        for (const auto [version, expected_valid] : {std::pair{3U, valid_v3}, std::pair{4U, valid_v4}}) {
            index.version = version;
            std::ostringstream output(std::ios::out | std::ios::binary);
            bool accepted = true;
            try {
                flitzzip::write_archive_index(output, index);
            } catch (const flitzzip::ArchiveError&) {
                accepted = false;
            }
            REQUIRE_EQ(accepted, expected_valid);
            if (expected_valid) {
                std::istringstream input(output.str(), std::ios::in | std::ios::binary);
                REQUIRE_EQ(flitzzip::read_archive_index(input).entries.front().blocks.front().encoded_len, length);
            }
        }
        if (valid_v4 && !valid_v3) {
            index.version = 4U;
            std::ostringstream output(std::ios::out | std::ios::binary);
            flitzzip::write_archive_index(output, index);
            auto forged = output.str();
            forged[4] = static_cast<char>(3);
            std::istringstream input(forged, std::ios::in | std::ios::binary);
            bool rejected = false;
            try {
                static_cast<void>(flitzzip::read_archive_index(input));
            } catch (const flitzzip::ArchiveError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
        }
    }
}

// Purpose: Validate version-four dictionary decoding and reject corrupt block framing or LZ4 matches.
// Inputs: An independently encoded 32-byte LZ4 block plus four table/payload mutations.
// Outputs: CPU and available HIP readers restore exact bytes; both reject every malformed mutation.
TEST_CASE(flzip_dictionary_reader_validates_segments) {
    constexpr std::array<unsigned char, 19> fixture{0U, 0U, 0U, 0U,    11U, 0U,  0U,  0U,  0x1FU, 'A',
                                                    1U, 0U, 7U, 0x50U, 'A', 'A', 'A', 'A', 'A'};
    const auto fixture_bytes = std::as_bytes(std::span(fixture));
    const std::vector<std::byte> valid(fixture_bytes.begin(), fixture_bytes.end());
    const std::string expected(32U, 'A');
    const auto root = test_temp_dir("flzip-dictionary-reader");
    const auto archive = root / "valid.flzip";
    write_dictionary_test_archive(archive, valid, expected);

    flitzzip::ExtractOptions cpu;
    cpu.force_cpu = true;
    cpu.gpu_required = false;
    REQUIRE_EQ(flitzzip::verify_flzip(archive, cpu).entries, 1U);
    const auto destination = root / "decoded";
    static_cast<void>(flitzzip::extract_flzip(archive, destination, cpu));
    std::ifstream extracted(destination / "data.bin", std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(extracted), {}), expected);
    extracted.close();

    flitzzip::ExtractOptions required_hip;
    required_hip.gpu_required = true;
    const bool hip_available = flitzzip::query_gpu_info().available;
    if (hip_available) {
        REQUIRE_TRUE(flitzzip::verify_flzip(archive, required_hip).gpu_used);
        const auto gpu_destination = root / "gpu-decoded";
        REQUIRE_TRUE(flitzzip::extract_flzip(archive, gpu_destination, required_hip).gpu_used);
        std::ifstream gpu_file(gpu_destination / "data.bin", std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(gpu_file), {}), expected);
        gpu_file.close();
    } else {
        bool rejected = false;
        try {
            static_cast<void>(flitzzip::verify_flzip(archive, required_hip));
        } catch (const flitzzip::GpuError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }

    for (int mutation = 0; mutation < 4; ++mutation) {
        auto corrupt = valid;
        switch (mutation) {
        case 0:
            corrupt[0] = std::byte{1};
            break;
        case 1:
            corrupt[4] = std::byte{12};
            break;
        case 2:
            corrupt[10] = std::byte{0};
            break;
        default:
            corrupt.push_back(std::byte{0x42});
            break;
        }
        const auto invalid_archive = root / ("corrupt-" + std::to_string(mutation) + ".flzip");
        write_dictionary_test_archive(invalid_archive, corrupt, expected);
        bool rejected = false;
        try {
            static_cast<void>(flitzzip::verify_flzip(invalid_archive, cpu));
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        if (hip_available) {
            bool gpu_rejected = false;
            try {
                static_cast<void>(flitzzip::verify_flzip(invalid_archive, required_hip));
            } catch (const flitzzip::ArchiveError&) {
                gpu_rejected = true;
            }
            REQUIRE_TRUE(gpu_rejected);
        }
    }
    if (hip_available) {
        REQUIRE_TRUE(flitzzip::verify_flzip(archive, required_hip).gpu_used);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Verify that dictionary block tables preserve exact output across a full segment and short tail.
// Inputs: Two independent LZ4 blocks for 64 KiB of one byte followed by 32 bytes of another.
// Outputs: The version-four reader verifies and extracts the exact concatenated bytes.
TEST_CASE(flzip_dictionary_reader_crosses_segment_boundary) {
    std::string expected(flitzzip::kGpuDictionarySegmentBytes, 'A');
    expected.append(32U, 'B');
    const std::array<std::string_view, 2> segments{
        std::string_view(expected).substr(0U, flitzzip::kGpuDictionarySegmentBytes),
        std::string_view(expected).substr(flitzzip::kGpuDictionarySegmentBytes)};
    std::ostringstream table(std::ios::out | std::ios::binary);
    flitzzip::write_u32(table, 0U);
    std::string compressed;
    for (const auto segment : segments) {
        std::string block(static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(segment.size()))), '\0');
        const auto size = LZ4_compress_default(segment.data(), block.data(), static_cast<int>(segment.size()),
                                               static_cast<int>(block.size()));
        REQUIRE_TRUE(size > 0);
        block.resize(static_cast<std::size_t>(size));
        compressed.append(block);
        flitzzip::write_u32(table, static_cast<std::uint32_t>(compressed.size()));
    }
    const auto encoded = table.str() + compressed;
    const auto payload = std::as_bytes(std::span(encoded.data(), encoded.size()));
    const auto root = test_temp_dir("flzip-dictionary-segments");
    const auto archive = root / "segments.flzip";
    write_dictionary_test_archive(archive, payload, expected);
    flitzzip::ExtractOptions options;
    options.force_cpu = true;
    options.gpu_required = false;
    REQUIRE_EQ(flitzzip::verify_flzip(archive, options).entries, 1U);
    const auto destination = root / "decoded";
    static_cast<void>(flitzzip::extract_flzip(archive, destination, options));
    std::ifstream file(destination / "data.bin", std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(file), {}), expected);
    file.close();
    if (flitzzip::query_gpu_info().available) {
        flitzzip::ExtractOptions required_hip;
        required_hip.gpu_required = true;
        REQUIRE_TRUE(flitzzip::verify_flzip(archive, required_hip).gpu_used);
        const auto gpu_destination = root / "gpu-decoded";
        REQUIRE_TRUE(flitzzip::extract_flzip(archive, gpu_destination, required_hip).gpu_used);
        std::ifstream gpu_file(gpu_destination / "data.bin", std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(gpu_file), {}), expected);
        gpu_file.close();
    }
    std::filesystem::remove_all(root);
}

// Purpose: Check native CPU/HIP readers against independently encoded LZ4 blocks across bounded data shapes.
// Inputs: Deterministic fill, periodic, repeated-record, and low-alphabet sources at short and full-segment lengths.
// Outputs: Every smaller LZ4 block decodes byte-exactly with matching CRC; at least twelve cases must be admitted.
TEST_CASE(flzip_dictionary_reader_matches_independent_lz4_corpus) {
    const bool hip_available = flitzzip::query_gpu_info().available;
    std::size_t admitted = 0U;
    for (const std::size_t size : {32U, 255U, 4096U, 16384U, 65536U}) {
        for (unsigned int profile = 0U; profile < 4U; ++profile) {
            std::vector<std::byte> expected(size);
            std::uint32_t random = 0x45C728D1U;
            for (std::size_t index = 0U; index < size; ++index) {
                random ^= random << 13U;
                random ^= random >> 17U;
                random ^= random << 5U;
                expected[index] = profile == 0U   ? std::byte{0xA5}
                                  : profile == 1U ? static_cast<std::byte>(index % 17U)
                                  : profile == 2U
                                      ? index < 1024U ? static_cast<std::byte>(random >> 24U) : expected[index % 1024U]
                                      : static_cast<std::byte>((random >> 16U) & 3U);
            }
            std::vector<std::byte> encoded(LZ4_compressBound(static_cast<int>(size)));
            const auto written = LZ4_compress_default(reinterpret_cast<const char*>(expected.data()),
                                                      reinterpret_cast<char*>(encoded.data()), static_cast<int>(size),
                                                      static_cast<int>(encoded.size()));
            REQUIRE_TRUE(written > 0);
            encoded.resize(static_cast<std::size_t>(written));
            if (encoded.size() + 8U >= size) {
                continue;
            }
            ++admitted;
            std::vector<std::byte> payload(8U + encoded.size());
            const auto length = static_cast<std::uint32_t>(encoded.size());
            for (std::size_t byte = 0U; byte < 4U; ++byte) {
                payload[4U + byte] = static_cast<std::byte>(length >> (byte * 8U));
            }
            std::copy(encoded.begin(), encoded.end(), payload.begin() + 8U);
            const std::array blocks{flitzzip::BlockDescriptor{
                .kind = flitzzip::BlockKind::GpuDictionary,
                .uncompressed_len = static_cast<std::uint32_t>(size),
                .encoded_len = static_cast<std::uint32_t>(payload.size()),
            }};
            for (const bool hip : {false, true}) {
                if (hip && !hip_available) {
                    continue;
                }
                flitzzip::GpuCodecOptions options;
                options.force_cpu = !hip;
                options.require_gpu = hip;
                std::vector<std::byte> decoded(size);
                REQUIRE_EQ(flitzzip::decode_chunk(payload, blocks, decoded, options), hip);
                REQUIRE_EQ(decoded, expected);
                REQUIRE_EQ(flitzzip::crc_decoded_chunk(payload, blocks, size, options).crc32,
                           flitzzip::crc32(expected));
            }
        }
    }
    REQUIRE_TRUE(admitted >= 12U);
}

// Purpose: Distinguish direct extraction from archive-wide validation before final publication.
// Inputs: A real two-entry FLZIP whose second payload is corrupted, plus existing first-file output.
// Outputs: Both modes reject the CRC error; staging retains the old first file and cleans all private output after one
// decode.
TEST_CASE(flzip_late_crc_failure_respects_publication_policy) {
    const auto root = test_temp_dir("flzip-publication-late-crc");
    const auto archive = root / "late-crc.flzip";
    write_raw_test_archive(archive, {{"a.txt", "valid first payload"}, {"b.txt", "invalid second payload"}});
    const auto index = read_test_archive_index(archive);
    REQUIRE_EQ(index.entries.size(), 2U);
    {
        std::fstream file(archive, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(static_cast<std::streamoff>(index.entries[1].payload_offset));
        file.put('X');
        REQUIRE_TRUE(static_cast<bool>(file));
    }
    for (const bool validate : {false, true}) {
        const auto output = root / (validate ? "private" : "direct");
        std::filesystem::create_directories(output);
        std::ofstream(output / "a.txt", std::ios::binary) << "existing bytes";
        int extractions = 0;
        bool rejected = false;
        try {
            flitzzip::extract_with_publication(
                {.destination = output, .overwrite = true, .validate_before_publish = validate},
                [&](const auto& destination, bool overwrite) {
                    ++extractions;
                    flitzzip::ExtractOptions options;
                    options.gpu_required = false;
                    options.force_cpu = true;
                    options.overwrite = overwrite;
                    static_cast<void>(flitzzip::extract_flzip(archive, destination, options));
                });
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(extractions, 1);
        std::ifstream first(output / "a.txt", std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(first), {}),
                   validate ? "existing bytes" : "valid first payload");
        REQUIRE_TRUE(!std::filesystem::exists(output / "b.txt"));
        REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(output), std::filesystem::directory_iterator{}),
                   1);
    }
}

// Purpose: Compare native CPU blocks with actual codec sizes instead of a size heuristic.
// Inputs: Non-uniform periodic and random payloads around the framing and old 512-byte cutoffs at every effort.
// Outputs: Requires the smallest admitted Raw/Deflate representation, stable ties, and lossless CPU decoding.
TEST_CASE(flzip_cpu_short_blocks_use_smaller_deflate) {
    for (const auto size :
         {2U, 3U, 7U, 8U, 9U, 10U, 11U, 12U, 16U, 17U, 31U, 32U, 63U, 64U, 127U, 255U, 511U, 512U, 513U, 1024U}) {
        for (const bool periodic : {false, true}) {
            std::vector<std::byte> input(size);
            std::uint32_t state = 0xE0DA746BU;
            for (std::size_t i = 0; i < input.size(); ++i) {
                state ^= state << 13U;
                state ^= state >> 17U;
                state ^= state << 5U;
                input[i] = static_cast<std::byte>(periodic ? 'a' + i % 3U : state & 255U);
            }
            for (int level = 1; level <= 9; ++level) {
                flitzzip::GpuCodecOptions options;
                options.force_cpu = true;
                options.require_gpu = false;
                options.compression_level = level;
                std::vector<std::byte> reference = input;
                std::vector<std::byte> trial(mz_compressBound(static_cast<mz_ulong>(input.size())));
                for (int admitted = 1; admitted <= level; ++admitted) {
                    auto written = static_cast<mz_ulong>(trial.size());
                    REQUIRE_EQ(mz_compress2(reinterpret_cast<unsigned char*>(trial.data()), &written,
                                            reinterpret_cast<const unsigned char*>(input.data()),
                                            static_cast<mz_ulong>(input.size()), admitted),
                               MZ_OK);
                    if (written < reference.size()) {
                        reference.assign(trial.begin(), trial.begin() + static_cast<std::ptrdiff_t>(written));
                    }
                }
                const bool smaller = reference.size() < input.size();
                const auto encoded = flitzzip::encode_chunk(input, options);
                REQUIRE_EQ(encoded.blocks.size(), 1U);
                REQUIRE_EQ(encoded.blocks.front().kind,
                           smaller ? flitzzip::BlockKind::Deflate : flitzzip::BlockKind::Raw);
                REQUIRE_EQ(encoded.payload, smaller ? reference : input);
                REQUIRE_TRUE(!encoded.gpu_used);
                std::vector<std::byte> decoded(input.size());
                flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options);
                REQUIRE_EQ(decoded, input);
            }
        }
    }
}

// Purpose: Require a bounded native CPU Zstandard frame for compressible full blocks at every effort.
// Inputs: A deterministic periodic block and product levels one through nine.
// Outputs: Requires version-six block kind, smaller payload, and byte-exact CPU decode.
TEST_CASE(flzip_cpu_zstd_blocks_all_levels_roundtrip) {
    std::vector<std::byte> input(256U * 1024U);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<std::byte>('a' + ((i / 31U) % 17U));
    }
    for (int level = 1; level <= 9; ++level) {
        flitzzip::GpuCodecOptions options;
        options.force_cpu = true;
        options.require_gpu = false;
        options.compression_level = level;
        options.block_size = static_cast<std::uint32_t>(input.size());
        const auto encoded = flitzzip::encode_chunk(input, options);
        REQUIRE_EQ(encoded.blocks.size(), 1U);
        REQUIRE_EQ(encoded.blocks[0].kind, flitzzip::BlockKind::CpuZstd);
        REQUIRE_TRUE(encoded.payload.size() < input.size());
        std::vector<std::byte> decoded(input.size());
        REQUIRE_TRUE(!flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
        REQUIRE_EQ(decoded, input);
    }
}

// Purpose: Preserve actual lower-effort winners when codec search policies are not size-monotone.
// Inputs: Independent low-alphabet, biased, repeated-record, random, and long-distance RAM fixtures at all efforts.
// Outputs: Requires the smallest complete codec frame or raw bytes, stable tie handling, and exact read-back.
TEST_CASE(flzip_cpu_efforts_preserve_complete_frame_winners) {
    const auto& zstd = flitzzip::zstd_runtime();
    for (const auto family : {0U, 1U, 2U, 3U, 4U}) {
        std::vector<std::byte> input(family == 4U ? 2U * 1024U * 1024U : 256U * 1024U);
        std::uint32_t state = 0x917DD82FU;
        for (std::size_t index = 0U; index < input.size(); ++index) {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            const auto value = state & 255U;
            input[index] = static_cast<std::byte>(family == 0U                   ? value & 15U
                                                  : family == 1U && value < 160U ? value & 3U
                                                                                 : value);
            if (family == 2U && index >= 16384U) {
                input[index] = input[index % 16384U];
            }
            if (family == 4U && index >= input.size() / 2U) {
                input[index] = input[index - input.size() / 2U];
            }
        }
        std::vector<std::byte> best = input;
        std::vector<std::byte> trial(zstd.block_compress_bound(input.size()));
        std::size_t first_bytes = 0U;
        for (int effort = 1; effort <= 9; ++effort) {
            const auto written = zstd.compress_block(trial.data(), trial.size(), input.data(), input.size(), effort);
            REQUIRE_TRUE(!zstd.is_error(written));
            if (written < best.size()) {
                best.assign(trial.begin(), trial.begin() + static_cast<std::ptrdiff_t>(written));
            }
            flitzzip::GpuCodecOptions options;
            options.force_cpu = true;
            options.require_gpu = false;
            options.block_size = static_cast<std::uint32_t>(input.size());
            options.compression_level = effort;
            const auto encoded = flitzzip::encode_chunk(input, options);
            REQUIRE_EQ(encoded.payload, best);
            REQUIRE_EQ(encoded.blocks.size(), 1U);
            REQUIRE_EQ(encoded.blocks.front().kind,
                       best.size() < input.size() ? flitzzip::BlockKind::CpuZstd : flitzzip::BlockKind::Raw);
            std::vector<std::byte> decoded(input.size());
            REQUIRE_TRUE(!flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
            REQUIRE_EQ(decoded, input);
            if (effort == 1) {
                first_bytes = encoded.payload.size();
            }
        }
        if (family == 4U) {
            REQUIRE_TRUE(best.size() < first_bytes * 3U / 4U);
        }
    }
}

// Purpose: Preserve dense payload assembly when a CPU chunk mixes all native CPU block representations.
// Inputs: One Zstandard, random raw, fill, and short Deflate block with single/multiple worker budgets.
// Outputs: Requires stable kinds, contiguous encoded offsets, and byte-exact decode.
TEST_CASE(flzip_cpu_zstd_mixed_block_layout) {
    constexpr std::size_t block_size = flitzzip::kMinArchiveBlockBytes;
    std::vector<std::byte> input(block_size * 3U + 127U);
    for (std::size_t i = 0; i < block_size; ++i) {
        input[i] = static_cast<std::byte>('a' + (i % 7U));
    }
    std::uint32_t state = 0xB179E34DU;
    for (std::size_t i = block_size; i < block_size * 2U; ++i) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        input[i] = static_cast<std::byte>(state & 255U);
    }
    std::fill(input.begin() + static_cast<std::ptrdiff_t>(block_size * 2U),
              input.begin() + static_cast<std::ptrdiff_t>(block_size * 3U), std::byte{'F'});
    for (std::size_t i = block_size * 3U; i < input.size(); ++i) {
        input[i] = static_cast<std::byte>('a' + (i % 3U));
    }
    for (const auto workers : {1U, 4U}) {
        flitzzip::GpuCodecOptions options;
        options.force_cpu = true;
        options.require_gpu = false;
        options.block_size = static_cast<std::uint32_t>(block_size);
        options.worker_count = workers;
        const auto encoded = flitzzip::encode_chunk(input, options);
        REQUIRE_EQ(encoded.blocks.size(), 4U);
        REQUIRE_EQ(encoded.blocks[0].kind, flitzzip::BlockKind::CpuZstd);
        REQUIRE_EQ(encoded.blocks[1].kind, flitzzip::BlockKind::Raw);
        REQUIRE_EQ(encoded.blocks[2].kind, flitzzip::BlockKind::Fill);
        REQUIRE_EQ(encoded.blocks[3].kind, flitzzip::BlockKind::Deflate);
        REQUIRE_EQ(encoded.blocks[0].encoded_offset, 0U);
        REQUIRE_EQ(encoded.blocks[1].encoded_offset, encoded.blocks[0].encoded_len);
        REQUIRE_EQ(encoded.blocks[3].encoded_offset, encoded.blocks[1].encoded_offset + encoded.blocks[1].encoded_len);
        std::vector<std::byte> decoded(input.size());
        REQUIRE_TRUE(!flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
        REQUIRE_EQ(decoded, input);
    }
}

// Purpose: Detect decoder state leaking between independent frames in a reused CPU worker context.
// Inputs: Independently encoded frames with changing alphabets, lengths and efforts, plus malformed input.
// Outputs: Requires exact whole-chunk and CPU-only readback at multiple worker counts and clean recovery.
TEST_CASE(flzip_cpu_zstd_reused_decoder_independent_frames) {
    const auto& zstd = flitzzip::zstd_runtime();
    std::vector<std::byte> expected;
    std::vector<std::byte> payload;
    std::vector<flitzzip::BlockDescriptor> blocks;
    for (std::uint32_t frame = 1U; frame <= 9U; ++frame) {
        std::vector<std::byte> source(4096U * frame + frame * 17U);
        for (std::size_t i = 0U; i < source.size(); ++i) {
            source[i] = static_cast<std::byte>((i / frame + frame * 19U) % (frame * 7U));
        }
        std::vector<std::byte> encoded(zstd.block_compress_bound(source.size()));
        const auto written =
            zstd.compress_block(encoded.data(), encoded.size(), source.data(), source.size(), static_cast<int>(frame));
        REQUIRE_TRUE(!zstd.is_error(written));
        REQUIRE_TRUE(written < source.size());
        blocks.push_back({.kind = flitzzip::BlockKind::CpuZstd,
                          .uncompressed_len = static_cast<std::uint32_t>(source.size()),
                          .encoded_offset = payload.size(),
                          .encoded_len = static_cast<std::uint32_t>(written)});
        payload.insert(payload.end(), encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(written));
        expected.insert(expected.end(), source.begin(), source.end());
    }
    for (const auto workers : {1U, 4U}) {
        for (const bool cpu_only : {false, true}) {
            std::vector<std::byte> decoded(expected.size());
            const flitzzip::ArchiveCodecOptions options{.worker_count = workers};
            // Purpose: Exercise both production CPU consumers over exactly the same independent frame sequence.
            // Inputs: Borrowed payload/table and an exclusively owned, completely sized output buffer.
            // Outputs: Decodes all frames or propagates a malformed-frame rejection.
            const auto decode = [&](std::span<const std::byte> bytes) {
                if (cpu_only) {
                    flitzzip::decode_cpu_only_blocks_cpu(bytes, blocks, decoded, options);
                } else {
                    flitzzip::decode_chunk_cpu(bytes, blocks, decoded, options);
                }
            };
            decode(payload);
            REQUIRE_EQ(decoded, expected);
            auto invalid = payload;
            invalid[static_cast<std::size_t>(blocks[4].encoded_offset)] ^= std::byte{0xFF};
            bool rejected = false;
            try {
                decode(invalid);
            } catch (const flitzzip::ArchiveError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
            std::ranges::fill(decoded, std::byte{0});
            decode(payload);
            REQUIRE_EQ(decoded, expected);
        }
    }
}

// Purpose: Reject invalid native CPU frame boundaries and protect version-six publication.
// Inputs: One valid frame, truncated and concatenated frames, trailing bytes, and a lying decoded-size declaration.
// Outputs: Valid CPU verification/extraction succeeds; invalid archives fail before output publication.
TEST_CASE(flzip_cpu_zstd_archive_frame_validation) {
    const auto root = test_temp_dir("flzip-cpu-zstd-frame");
    std::string decoded(64U * 1024U, 'Z');
    for (std::size_t i = 0; i < decoded.size(); i += 53U) {
        decoded[i] = 'Q';
    }
    const auto source = std::as_bytes(std::span(decoded.data(), decoded.size()));
    flitzzip::GpuCodecOptions codec;
    codec.force_cpu = true;
    codec.require_gpu = false;
    codec.block_size = static_cast<std::uint32_t>(decoded.size());
    const auto encoded = flitzzip::encode_chunk(source, codec);
    REQUIRE_EQ(encoded.blocks.size(), 1U);
    REQUIRE_EQ(encoded.blocks[0].kind, flitzzip::BlockKind::CpuZstd);

    flitzzip::ExtractOptions options;
    options.force_cpu = true;
    options.gpu_required = false;
    const auto valid = root / "valid.flzip";
    write_encoded_test_archive(valid, encoded.payload, decoded, flitzzip::BlockKind::CpuZstd, 6U);
    REQUIRE_EQ(read_test_archive_index(valid).version, 6U);
    REQUIRE_EQ(flitzzip::verify_flzip(valid, options).entries, 1U);
    const auto destination = root / "valid-output";
    REQUIRE_EQ(flitzzip::extract_flzip(valid, destination, options).entries, 1U);
    std::ifstream restored(destination / "data.bin", std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(restored), {}), decoded);
    restored.close();

    auto truncated = encoded.payload;
    truncated.pop_back();
    auto trailing = encoded.payload;
    trailing.push_back(std::byte{0});
    auto concatenated = encoded.payload;
    concatenated.insert(concatenated.end(), encoded.payload.begin(), encoded.payload.end());
    for (const auto& [name, bytes] : {std::pair{"truncated", truncated}, std::pair{"trailing", trailing},
                                      std::pair{"concatenated", concatenated}}) {
        const auto archive = root / (std::string(name) + ".flzip");
        write_encoded_test_archive(archive, bytes, decoded, flitzzip::BlockKind::CpuZstd, 6U);
        bool rejected = false;
        try {
            (void)flitzzip::verify_flzip(archive, options);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        const auto invalid_destination = root / (std::string(name) + "-output");
        rejected = false;
        try {
            (void)flitzzip::extract_flzip(archive, invalid_destination, options);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_TRUE(!std::filesystem::exists(invalid_destination / "data.bin"));
    }
    const auto wrong_size = root / "wrong-size.flzip";
    write_encoded_test_archive(wrong_size, encoded.payload, decoded + "Q", flitzzip::BlockKind::CpuZstd, 6U);
    bool rejected = false;
    try {
        (void)flitzzip::verify_flzip(wrong_size, options);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Preserve public reading of short legacy Deflate blocks after adding native Zstandard blocks.
// Inputs: A version-three archive containing a product-generated short Deflate payload.
// Outputs: Requires exact verification and extraction through the unchanged legacy CPU decoder.
TEST_CASE(flzip_legacy_deflate_archive_remains_readable) {
    const auto root = test_temp_dir("flzip-legacy-deflate");
    std::string decoded(127U, 'a');
    for (std::size_t i = 0; i < decoded.size(); i += 3U) {
        decoded[i] = 'b';
    }
    flitzzip::GpuCodecOptions codec;
    codec.force_cpu = true;
    codec.require_gpu = false;
    const auto encoded = flitzzip::encode_chunk(std::as_bytes(std::span(decoded.data(), decoded.size())), codec);
    REQUIRE_EQ(encoded.blocks.size(), 1U);
    REQUIRE_EQ(encoded.blocks[0].kind, flitzzip::BlockKind::Deflate);
    const auto archive = root / "legacy.flzip";
    write_encoded_test_archive(archive, encoded.payload, decoded, flitzzip::BlockKind::Deflate, 3U);
    flitzzip::ExtractOptions options;
    options.force_cpu = true;
    options.gpu_required = false;
    REQUIRE_EQ(flitzzip::verify_flzip(archive, options).entries, 1U);
    const auto destination = root / "output";
    REQUIRE_EQ(flitzzip::extract_flzip(archive, destination, options).entries, 1U);
    std::ifstream restored(destination / "data.bin", std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(restored), {}), decoded);
    restored.close();
    std::filesystem::remove_all(root);
}

// Purpose: Retain compression of a short final block after full-sized raw data in parallel CPU work.
// Inputs: One deterministic random block followed by a 127-byte repeated sequence, three efforts and worker budgets.
// Outputs: Requires raw then Deflate descriptors, contiguous payload, and byte-exact decoding.
TEST_CASE(flzip_cpu_short_tail_after_raw_block) {
    std::vector<std::byte> input(flitzzip::kMinArchiveBlockBytes + 127U);
    std::uint32_t state = 0x43A81D27U;
    for (std::size_t i = 0; i < flitzzip::kMinArchiveBlockBytes; ++i) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        input[i] = static_cast<std::byte>(state & 255U);
    }
    for (std::size_t i = flitzzip::kMinArchiveBlockBytes; i < input.size(); ++i) {
        input[i] = static_cast<std::byte>(i % 3U);
    }
    for (const int level : {1, 5, 9}) {
        for (const auto workers : {1U, 2U, 4U}) {
            flitzzip::GpuCodecOptions options;
            options.force_cpu = true;
            options.require_gpu = false;
            options.compression_level = level;
            options.block_size = flitzzip::kMinArchiveBlockBytes;
            options.worker_count = workers;
            const auto encoded = flitzzip::encode_chunk(input, options);
            REQUIRE_EQ(encoded.blocks.size(), 2U);
            REQUIRE_EQ(encoded.blocks[0].kind, flitzzip::BlockKind::Raw);
            REQUIRE_EQ(encoded.blocks[1].kind, flitzzip::BlockKind::Deflate);
            REQUIRE_EQ(encoded.blocks[1].encoded_offset, flitzzip::kMinArchiveBlockBytes);
            REQUIRE_TRUE(encoded.payload.size() < input.size());
            std::vector<std::byte> decoded(input.size());
            flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options);
            REQUIRE_EQ(decoded, input);
        }
    }
}

// Purpose: Restore native UTF-8 filenames without depending on the host's ANSI code page.
// Inputs: Unicode source, archive, destination and entry paths in CPU and available required-HIP modes.
// Outputs: Requires verified archive roundtrips, identical filenames/payloads, and honest CPU/HIP telemetry.
TEST_CASE(flzip_unicode_paths_roundtrip) {
    const auto root = test_temp_dir("flzip-unicode");
    const auto source = root / std::filesystem::path(u8"\u65e5\u672c");
    const auto relative = std::filesystem::path(u8"caf\u00e9") / std::filesystem::path(u8"\U0001f4c1.txt");
    std::filesystem::create_directories((source / relative).parent_path());
    const std::string payload(8192U, 'U');
    {
        std::ofstream input(source / relative, std::ios::binary);
        REQUIRE_TRUE(input.is_open());
        input << payload;
        input.close();
        REQUIRE_TRUE(!input.fail());
    }
    const bool hip_available = flitzzip::query_gpu_info().available;
    for (const bool hip : {false, true}) {
        if (hip && !hip_available) {
            continue;
        }
        flitzzip::CompressOptions compression;
        compression.force_cpu = !hip;
        compression.gpu_required = hip;
        compression.verify_after_write = true;
        const auto archive = root / std::filesystem::path(u8"\u03a9.flzip");
        const auto compressed = flitzzip::compress_flzip({source}, archive, compression);
        REQUIRE_EQ(compressed.gpu_used, hip);
        flitzzip::ExtractOptions extraction;
        extraction.force_cpu = !hip;
        extraction.gpu_required = hip;
        const auto destination = root / (hip ? "hip" : "cpu") / std::filesystem::path(u8"\u00e9");
        const auto extracted = flitzzip::extract_flzip(archive, destination, extraction);
        REQUIRE_EQ(extracted.gpu_used, hip);
        REQUIRE_EQ(extracted.output_bytes, payload.size());
        std::ifstream restored(destination / source.filename() / relative, std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(restored), std::istreambuf_iterator<char>()), payload);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Verify every product block-size option is a real compression setting.
// Inputs: A deterministic 3 MiB file compressed with each offered block size.
// Outputs: Extracted payload matches the source and archive metadata never exceeds the selected block size.
TEST_CASE(flzip_supported_block_sizes_roundtrip_and_bound_metadata) {
    const auto root = test_temp_dir("flzip-block-sizes");
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
        256U * 1024U,
        512U * 1024U,
        1024U * 1024U,
        2U * 1024U * 1024U,
        4U * 1024U * 1024U,
        8U * 1024U * 1024U,
        flitzzip::kMaxArchiveBlockBytes,
    };
    for (const auto block_size : block_sizes) {
        const auto archive = root / ("archive-" + std::to_string(block_size) + ".flzip");
        const auto output = root / ("out-" + std::to_string(block_size));
        flitzzip::CompressOptions compress;
        compress.force_cpu = true;
        compress.gpu_required = false;
        compress.block_size = block_size;
        const auto compressed = flitzzip::compress_flzip({source}, archive, compress);
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

        flitzzip::ExtractOptions extract;
        extract.force_cpu = true;
        extract.gpu_required = false;
        const auto extracted = flitzzip::extract_flzip(archive, output, extract);
        REQUIRE_TRUE(!extracted.gpu_used);
        const auto restored = output / "source" / "payload.bin";
        REQUIRE_TRUE(std::filesystem::exists(restored));
        REQUIRE_EQ(std::filesystem::file_size(restored), std::filesystem::file_size(input));
    }
    std::filesystem::remove_all(root);
}

// Purpose: Keep the new default block size observable in archive metadata without changing saved legacy choices.
// Inputs: A one-byte tail after a full default-sized block, with explicit CPU mode and default block size.
// Outputs: The archive contains 8 MiB and one-byte blocks and extracts the original bytes exactly.
TEST_CASE(flzip_default_block_size_roundtrip) {
    REQUIRE_EQ(flitzzip::kDefaultArchiveBlockBytes, 8U * 1024U * 1024U);
    const auto root = test_temp_dir("flzip-default-block-size");
    const auto source = root / "payload.bin";
    const auto archive = root / "payload.flzip";
    const auto output = root / "out";
    const std::string expected(static_cast<std::size_t>(flitzzip::kDefaultArchiveBlockBytes) + 1U, 'x');
    {
        std::ofstream file(source, std::ios::binary);
        file.write(expected.data(), static_cast<std::streamsize>(expected.size()));
    }
    flitzzip::CompressOptions options;
    options.force_cpu = true;
    options.gpu_required = false;
    REQUIRE_EQ(options.block_size, flitzzip::kDefaultArchiveBlockBytes);
    (void)flitzzip::compress_flzip({source}, archive, options);
    const auto index = read_test_archive_index(archive);
    REQUIRE_EQ(index.entries.size(), 1U);
    REQUIRE_EQ(index.entries.front().blocks.size(), 2U);
    REQUIRE_EQ(index.entries.front().blocks[0].uncompressed_len, flitzzip::kDefaultArchiveBlockBytes);
    REQUIRE_EQ(index.entries.front().blocks[1].uncompressed_len, 1U);
    flitzzip::ExtractOptions extract;
    extract.force_cpu = true;
    extract.gpu_required = false;
    (void)flitzzip::extract_flzip(archive, output, extract);
    {
        std::ifstream restored(output / "payload.bin", std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(restored), {}), expected);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Verify the production default compression level is balanced, not fastest-only.
// Inputs: A repetitive temporary payload compressed with explicit level 1 and default options.
// Outputs: Default compression must not produce a larger archive than explicit fastest mode.
TEST_CASE(flzip_default_compression_level_is_balanced) {
    const auto root = test_temp_dir("flzip-default-level");
    const auto input = root / "repetitive.txt";
    {
        std::ofstream out(input, std::ios::binary);
        for (int i = 0; i < 120000; ++i) {
            out << "FlitzZip compression level regression payload " << (i % 17) << "\n";
        }
    }

    flitzzip::CompressOptions fastest;
    fastest.force_cpu = true;
    fastest.gpu_required = false;
    fastest.compression_level = flitzzip::kMinCompressionLevel;
    const auto fastest_archive = root / "fastest.flzip";
    const auto fastest_stats = flitzzip::compress_flzip({input}, fastest_archive, fastest);

    flitzzip::CompressOptions balanced;
    balanced.force_cpu = true;
    balanced.gpu_required = false;
    const auto balanced_archive = root / "balanced.flzip";
    const auto balanced_stats = flitzzip::compress_flzip({input}, balanced_archive, balanced);

    REQUIRE_TRUE(balanced.compression_level == flitzzip::kDefaultCompressionLevel);
    REQUIRE_TRUE(balanced_stats.output_bytes <= fastest_stats.output_bytes);
    std::filesystem::remove_all(root);
}

// Purpose: Build deterministic near-identical records for dictionary codec regressions.
// Inputs: `state` is the caller-owned PRNG seed and receives the final state.
// Outputs: Returns four 16 KiB records with distinct first bytes.
std::vector<std::byte> make_dictionary_writer_input(std::uint32_t& state) {
    std::vector<std::byte> input(65536U);
    for (std::size_t index = 0; index < input.size(); ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        input[index] = index < 16384U ? static_cast<std::byte>(state >> 24U) : input[index % 16384U];
    }
    for (std::size_t record = 1U; record < 4U; ++record) {
        input[record * 16384U] = static_cast<std::byte>(record);
    }
    return input;
}

// Purpose: Exercise production HIP dictionary selection across every effort level.
// Inputs: Four seeded 16 KiB records with changing first bytes.
// Outputs: Every level produces a smaller distinct payload, honest telemetry, and an exact CPU decode.
TEST_CASE(flzip_gpu_dictionary_writer_levels_and_roundtrip) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    std::uint32_t state = 0x31674325U;
    const auto input = make_dictionary_writer_input(state);

    std::size_t previous = input.size();
    for (int level = 1; level <= 9; ++level) {
        flitzzip::GpuCodecOptions options;
        options.require_gpu = true;
        options.compression_level = level;
        options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
        const auto encoded = flitzzip::encode_chunk(input, options);
        REQUIRE_TRUE(encoded.gpu_used);
        REQUIRE_EQ(encoded.blocks.size(), 1U);
        REQUIRE_EQ(encoded.blocks.front().kind, flitzzip::BlockKind::GpuDictionary);
        REQUIRE_TRUE(encoded.payload.size() < previous);
        const auto telemetry = flitzzip::snapshot_gpu_telemetry(*options.telemetry);
        REQUIRE_TRUE(telemetry.kernel_launches >= 5U);
        REQUIRE_TRUE(telemetry.h2d_bytes >= input.size());
        REQUIRE_EQ(telemetry.dictionary_blocks, 1U);
        REQUIRE_EQ(telemetry.prefix_blocks, 0U);
        std::vector<std::byte> decoded(input.size());
        flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, {.require_gpu = false, .force_cpu = true});
        REQUIRE_EQ(decoded, input);
        previous = encoded.payload.size();
    }
}

// Purpose: Verify mixed GPU block offsets and a production dictionary archive on both readers.
// Inputs: Seeded dictionary, random, and fill records plus one required-HIP version-four archive.
// Outputs: Requires exact mixed decode/CRC and byte-exact CPU/HIP archive verification and extraction.
TEST_CASE(flzip_gpu_dictionary_mixed_and_archive_roundtrip) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    std::uint32_t state = 0x31674325U;
    const auto input = make_dictionary_writer_input(state);
    std::vector<std::byte> mixed = input;
    for (std::size_t index = 0; index < input.size(); ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        mixed.push_back(static_cast<std::byte>(state >> 24U));
    }
    mixed.insert(mixed.end(), input.size(), std::byte{0xA7});
    flitzzip::GpuCodecOptions mixed_options;
    mixed_options.block_size = static_cast<std::uint32_t>(input.size());
    mixed_options.compression_level = 9;
    const auto mixed_encoded = flitzzip::encode_chunk(mixed, mixed_options);
    REQUIRE_EQ(mixed_encoded.blocks.size(), 3U);
    REQUIRE_EQ(mixed_encoded.blocks[0].kind, flitzzip::BlockKind::GpuDictionary);
    REQUIRE_EQ(mixed_encoded.blocks[1].kind, flitzzip::BlockKind::Raw);
    REQUIRE_EQ(mixed_encoded.blocks[2].kind, flitzzip::BlockKind::Fill);
    REQUIRE_EQ(mixed_encoded.blocks[1].encoded_offset, mixed_encoded.blocks[0].encoded_len);
    REQUIRE_EQ(mixed_encoded.payload.size(), mixed_encoded.blocks[0].encoded_len + mixed_encoded.blocks[1].encoded_len);
    for (const bool hip : {false, true}) {
        auto decode_options = mixed_options;
        decode_options.force_cpu = !hip;
        decode_options.require_gpu = hip;
        std::vector<std::byte> decoded(mixed.size());
        REQUIRE_EQ(flitzzip::decode_chunk(mixed_encoded.payload, mixed_encoded.blocks, decoded, decode_options), hip);
        REQUIRE_EQ(decoded, mixed);
        const auto crc =
            flitzzip::crc_decoded_chunk(mixed_encoded.payload, mixed_encoded.blocks, mixed.size(), decode_options);
        REQUIRE_EQ(crc.crc32, flitzzip::crc32(mixed));
    }

    const auto root = test_temp_dir("flzip-dictionary-writer");
    const auto source = root / "record.bin";
    {
        std::ofstream file(source, std::ios::binary);
        file.write(reinterpret_cast<const char*>(input.data()), static_cast<std::streamsize>(input.size()));
    }
    const auto archive = root / "record.flzip";
    flitzzip::CompressOptions compression;
    compression.compression_level = 9;
    compression.block_size = static_cast<std::uint32_t>(input.size());
    compression.verify_after_write = true;
    const auto compressed = flitzzip::compress_flzip({source}, archive, compression);
    REQUIRE_TRUE(compressed.gpu_used);
    const auto index = read_test_archive_index(archive);
    REQUIRE_EQ(index.version, 4U);
    REQUIRE_TRUE(archive_contains_block_kind(index, flitzzip::BlockKind::GpuDictionary));
    for (const bool hip : {false, true}) {
        flitzzip::ExtractOptions options;
        options.force_cpu = !hip;
        options.gpu_required = hip;
        REQUIRE_EQ(flitzzip::verify_flzip(archive, options).gpu_used, hip);
        const auto destination = root / (hip ? "hip" : "cpu");
        REQUIRE_EQ(flitzzip::extract_flzip(archive, destination, options).gpu_used, hip);
        std::ifstream restored(destination / "record.bin", std::ios::binary);
        REQUIRE_TRUE(restored.is_open());
        std::vector<std::byte> bytes(input.size());
        restored.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE_EQ(restored.gcount(), static_cast<std::streamsize>(bytes.size()));
        REQUIRE_EQ(restored.peek(), std::char_traits<char>::eof());
        REQUIRE_EQ(bytes, input);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Verify long periodic blocks are selected only after full HIP validation and use native version four.
// Inputs: Nontrivial motifs at multiple lengths plus one near-periodic block with a late mismatch.
// Outputs: CPU/HIP decoders reproduce exact bytes, a real archive roundtrips, and mismatches are never mislabeled.
TEST_CASE(flzip_gpu_long_pattern_version_four_roundtrip) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> archive_input;
    for (const auto period : {257U, 4096U, 8192U, flitzzip::kMaxGpuPatternBytes}) {
        std::vector<std::byte> motif(period);
        std::uint32_t state = 0x6A09E667U ^ period;
        for (auto& value : motif) {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            value = static_cast<std::byte>(state >> 24U);
        }
        std::vector<std::byte> input(period * 4U);
        for (std::size_t index = 0; index < input.size(); ++index) {
            input[index] = motif[index % period];
        }
        flitzzip::GpuCodecOptions options;
        options.require_gpu = true;
        options.compression_level = 5;
        const auto encoded = flitzzip::encode_chunk(input, options);
        REQUIRE_TRUE(encoded.gpu_used);
        REQUIRE_EQ(encoded.blocks.size(), 1U);
        REQUIRE_EQ(encoded.blocks.front().kind, flitzzip::BlockKind::Pattern);
        REQUIRE_EQ(encoded.blocks.front().encoded_len, period);
        REQUIRE_EQ(encoded.payload.size(), period);
        for (const bool hip : {false, true}) {
            auto decode_options = options;
            decode_options.force_cpu = !hip;
            decode_options.require_gpu = hip;
            std::vector<std::byte> decoded(input.size());
            REQUIRE_EQ(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, decode_options), hip);
            REQUIRE_EQ(decoded, input);
        }
        if (period == flitzzip::kMaxGpuPatternBytes) {
            archive_input = input;
            input.back() ^= std::byte{0x01};
            const auto near_pattern = flitzzip::encode_chunk(input, options);
            REQUIRE_TRUE(near_pattern.gpu_used);
            REQUIRE_EQ(near_pattern.blocks.size(), 1U);
            REQUIRE_TRUE(near_pattern.blocks.front().kind != flitzzip::BlockKind::Pattern);
            std::vector<std::byte> decoded(input.size());
            flitzzip::decode_chunk(near_pattern.payload, near_pattern.blocks, decoded,
                                   {.require_gpu = false, .force_cpu = true});
            REQUIRE_EQ(decoded, input);
        }
    }

    const auto root = test_temp_dir("flzip-long-pattern-v4");
    const auto source = root / "pattern.bin";
    {
        std::ofstream file(source, std::ios::binary);
        file.write(reinterpret_cast<const char*>(archive_input.data()),
                   static_cast<std::streamsize>(archive_input.size()));
    }
    const auto archive = root / "pattern.flzip";
    flitzzip::CompressOptions compression;
    compression.gpu_required = true;
    compression.verify_after_write = true;
    REQUIRE_TRUE(flitzzip::compress_flzip({source}, archive, compression).gpu_used);
    const auto index = read_test_archive_index(archive);
    REQUIRE_EQ(index.version, 4U);
    REQUIRE_EQ(index.entries.front().blocks.front().kind, flitzzip::BlockKind::Pattern);
    REQUIRE_EQ(index.entries.front().blocks.front().encoded_len, flitzzip::kMaxGpuPatternBytes);
    for (const bool hip : {false, true}) {
        flitzzip::ExtractOptions options;
        options.force_cpu = !hip;
        options.gpu_required = hip;
        REQUIRE_EQ(flitzzip::verify_flzip(archive, options).gpu_used, hip);
        const auto destination = root / (hip ? "hip" : "cpu");
        REQUIRE_EQ(flitzzip::extract_flzip(archive, destination, options).gpu_used, hip);
        std::ifstream restored(destination / "pattern.bin", std::ios::binary);
        std::vector<std::byte> bytes(archive_input.size());
        restored.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE_EQ(restored.gcount(), static_cast<std::streamsize>(bytes.size()));
        REQUIRE_EQ(restored.peek(), std::char_traits<char>::eof());
        REQUIRE_EQ(bytes, archive_input);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Exercise CPU pattern expansion at the smallest valid tail and repeated-copy boundaries.
// Inputs: Motifs spanning legacy and version-four limits with non-multiple decoded lengths.
// Outputs: Every decoded byte matches the independent modulo reference, including the final partial copy.
TEST_CASE(flzip_cpu_pattern_expansion_boundaries) {
    for (const auto period : {2U, 256U, 257U, flitzzip::kMaxGpuPatternBytes}) {
        std::vector<std::byte> motif(period);
        for (std::size_t index = 0; index < motif.size(); ++index) {
            motif[index] = static_cast<std::byte>((index * 131U + 17U) & 0xFFU);
        }
        for (const auto length : {period + 1U, period * 2U - 1U, period * 3U + 13U}) {
            const std::array blocks{flitzzip::BlockDescriptor{
                .kind = flitzzip::BlockKind::Pattern,
                .uncompressed_len = length,
                .encoded_len = period,
            }};
            std::vector<std::byte> decoded(length);
            REQUIRE_TRUE(!flitzzip::decode_chunk(motif, blocks, decoded, {.require_gpu = false, .force_cpu = true}));
            for (std::size_t index = 0; index < decoded.size(); ++index) {
                REQUIRE_EQ(decoded[index], motif[index % period]);
            }
        }
    }
}

// Purpose: Measure public CPU decoding of bounded version-four pattern blocks without filesystem writes.
// Inputs: An explicit environment opt-in and eight deterministic 16 MiB pattern blocks.
// Outputs: Prints a median decode time only after byte-exact validation; never asserts a timing threshold.
TEST_CASE(flzip_cpu_pattern_decode_benchmark_opt_in) {
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"FLITZZIP_PATTERN_CPU_BENCHMARK", enabled, 2U) != 1U || enabled[0] != L'1') {
        return;
    }
    constexpr std::size_t block_bytes = flitzzip::kMaxArchiveBlockBytes;
    constexpr std::size_t block_count = 8U;
    constexpr std::size_t pattern_bytes = flitzzip::kMaxGpuPatternBytes;
    std::vector<std::byte> payload(block_count * pattern_bytes);
    std::uint32_t state = 0x243F6A88U;
    for (std::size_t index = 0; index < pattern_bytes; ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        const auto value = static_cast<std::byte>(state >> 24U);
        for (std::size_t block = 0; block < block_count; ++block) {
            payload[block * pattern_bytes + index] = value;
        }
    }
    std::vector<flitzzip::BlockDescriptor> blocks;
    for (std::size_t block = 0; block < block_count; ++block) {
        blocks.push_back(flitzzip::BlockDescriptor{
            .kind = flitzzip::BlockKind::Pattern,
            .uncompressed_len = static_cast<std::uint32_t>(block_bytes),
            .encoded_offset = block * pattern_bytes,
            .encoded_len = static_cast<std::uint32_t>(pattern_bytes),
        });
    }
    std::vector<std::byte> decoded(block_count * block_bytes);
    const flitzzip::GpuCodecOptions options{.require_gpu = false, .force_cpu = true};
    std::vector<double> milliseconds;
    for (int iteration = 0; iteration < 5; ++iteration) {
        const auto started = std::chrono::steady_clock::now();
        REQUIRE_TRUE(!flitzzip::decode_chunk(payload, blocks, decoded, options));
        milliseconds.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        for (std::size_t index = 0; index < decoded.size(); ++index) {
            REQUIRE_EQ(decoded[index], payload[index % pattern_bytes]);
        }
    }
    std::sort(milliseconds.begin(), milliseconds.end());
    std::cout << "pattern_cpu_decode input_bytes=" << decoded.size() << " payload_bytes=" << payload.size()
              << " median_ms=" << milliseconds[milliseconds.size() / 2U] << " memory_only=true disk_write_bytes=0\n";
}

// Purpose: Verify `.flzip` roundtrip behavior for compressible and mixed byte patterns.
// Inputs: Temporary files with repetitive and randomish content.
// Outputs: Throws on failed compression/extraction or mismatched restored bytes.
TEST_CASE(flzip_roundtrip_repetitive_and_randomish_files) {
    const auto root = test_temp_dir("flzip-roundtrip");
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
    const auto archive = root / "archive.flzip";
    flitzzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.chunk_size = 128 * 1024;
    compress.block_size = 64 * 1024;
    compress.verify_after_write = true;
    const auto compressed = flitzzip::compress_flzip({input_dir}, archive, compress);
    REQUIRE_TRUE(compressed.output_bytes > 0);
    const auto output = root / "out";
    flitzzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.overwrite = true;
    extract.chunk_size = 64 * 1024;
    extract.block_size = 64 * 1024;
    (void)flitzzip::extract_flzip(archive, output, extract);
    REQUIRE_TRUE(std::filesystem::exists(output / "input" / "zeros.bin"));
    REQUIRE_TRUE(std::filesystem::exists(output / "input" / "nested" / "pattern.bin"));
    REQUIRE_EQ(std::filesystem::file_size(output / "input" / "zeros.bin"), static_cast<std::uintmax_t>(256 * 1024));
    REQUIRE_EQ(std::filesystem::file_size(output / "input" / "nested" / "pattern.bin"),
               static_cast<std::uintmax_t>(200000));
    std::filesystem::remove_all(root);
}

// Purpose: Verify `.flzip` preserves empty files and empty directories.
// Inputs: A temporary source tree containing one empty file and one empty directory.
// Outputs: Throws when extracted filesystem shape is incomplete.
TEST_CASE(flzip_roundtrip_empty_file_and_empty_directory) {
    const auto root = test_temp_dir("flzip-empty");
    const auto input_dir = root / "input";
    const auto empty_dir = input_dir / "empty";
    std::filesystem::create_directories(empty_dir);
    std::ofstream(input_dir / "empty.txt", std::ios::binary);
    const auto archive = root / "archive.flzip";
    flitzzip::CompressOptions compress;
    compress.gpu_required = false;
    (void)flitzzip::compress_flzip({input_dir}, archive, compress);
    const auto output = root / "out";
    flitzzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.overwrite = true;
    (void)flitzzip::extract_flzip(archive, output, extract);
    REQUIRE_TRUE(std::filesystem::is_directory(output / "input" / "empty"));
    REQUIRE_TRUE(std::filesystem::exists(output / "input" / "empty.txt"));
    REQUIRE_EQ(std::filesystem::file_size(output / "input" / "empty.txt"), static_cast<std::uintmax_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify forced-CPU diagnostics bypass AMD HIP without changing archive correctness.
// Inputs: A temporary source file compressed, verified, and extracted with `force_cpu` enabled.
// Outputs: Throws if any operation reports GPU usage or if extracted content differs.
TEST_CASE(flzip_force_cpu_roundtrip_reports_no_gpu_usage) {
    const auto root = test_temp_dir("flzip-force-cpu");
    const auto input = root / "input.bin";
    std::ofstream(input, std::ios::binary) << "force cpu benchmark path";
    const auto archive = root / "archive.flzip";

    flitzzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.force_cpu = true;
    const auto compressed = flitzzip::compress_flzip({input}, archive, compress);
    REQUIRE_TRUE(!compressed.gpu_used);

    flitzzip::ExtractOptions verify;
    verify.gpu_required = false;
    verify.force_cpu = true;
    const auto verified = flitzzip::verify_flzip(archive, verify);
    REQUIRE_TRUE(!verified.gpu_used);

    const auto output = root / "out";
    flitzzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.force_cpu = true;
    extract.overwrite = true;
    const auto extracted = flitzzip::extract_flzip(archive, output, extract);
    REQUIRE_TRUE(!extracted.gpu_used);
    REQUIRE_TRUE(std::filesystem::exists(output / "input.bin"));
    REQUIRE_EQ(std::filesystem::file_size(output / "input.bin"), static_cast<std::uintmax_t>(24));
    std::filesystem::remove_all(root);
}

// Purpose: Exercise resource-aware pinned-output admission through real required-HIP archive extraction.
// Inputs: A bounded native archive, 32 requested workers, and the normal automatic decode queue policy.
// Outputs: Requires exact CPU/HIP extraction, true GPU telemetry, and at most four large output windows in flight.
TEST_CASE(flzip_required_gpu_extract_uses_owned_output_admission) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const auto root = test_temp_dir("flzip-pinned-output-admission");
    const auto input = root / "input.bin";
    std::vector<char> payload(1024U * 1024U);
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<char>(index & 15U);
    }
    std::ofstream(input, std::ios::binary).write(payload.data(), static_cast<std::streamsize>(payload.size()));
    const auto archive = root / "archive.flzip";
    flitzzip::CompressOptions compress;
    compress.gpu_required = true;
    compress.block_size = static_cast<std::uint32_t>(payload.size());
    const auto compressed = flitzzip::compress_flzip({input}, archive, compress);
    REQUIRE_TRUE(compressed.gpu_used);
    for (const bool gpu : {false, true}) {
        flitzzip::ExtractOptions extract;
        extract.gpu_required = gpu;
        extract.force_cpu = !gpu;
        extract.worker_count = 32U;
        const auto destination = root / (gpu ? "gpu" : "cpu");
        const auto stats = flitzzip::extract_flzip(archive, destination, extract);
        REQUIRE_EQ(stats.gpu_used, gpu);
        if (gpu) {
            REQUIRE_TRUE(stats.inflight_chunks >= 1U && stats.inflight_chunks <= 4U);
            REQUIRE_TRUE(stats.gpu_runtime.decode_chunks > 0U);
            REQUIRE_TRUE(stats.gpu_runtime.host_pinned_allocation_bytes <= payload.size());
        } else {
            REQUIRE_EQ(stats.gpu_runtime.host_pinned_allocation_bytes, 0U);
        }
        std::ifstream restored(destination / "input.bin", std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>(restored), std::istreambuf_iterator<char>()};
        REQUIRE_TRUE(std::ranges::equal(contents, payload));
    }
    std::filesystem::remove_all(root);
}

// Purpose: Verify extraction handles archives whose encoded block size is larger than the runtime chunk preference.
// Inputs: A force-CPU archive written with 64 KiB blocks and extracted with 4 KiB runtime chunks.
// Outputs: Throws if decode memory budgeting rejects the archive or restores different bytes.
TEST_CASE(flzip_extract_supports_smaller_runtime_chunk_than_archive_block) {
    const auto root = test_temp_dir("flzip-small-runtime-chunk");
    const auto input = root / "input.bin";
    std::vector<char> payload(96 * 1024);
    std::uint32_t state = 0x31415926U;
    for (auto& byte : payload) {
        state = (state * 1664525U) + 1013904223U;
        byte = static_cast<char>((state >> 24U) & 0xFFU);
    }
    std::ofstream(input, std::ios::binary).write(payload.data(), static_cast<std::streamsize>(payload.size()));

    const auto archive = root / "archive.flzip";
    flitzzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.force_cpu = true;
    compress.chunk_size = 128 * 1024;
    compress.block_size = 64 * 1024;
    (void)flitzzip::compress_flzip({input}, archive, compress);

    flitzzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.force_cpu = true;
    extract.overwrite = true;
    extract.chunk_size = flitzzip::kMinArchiveBlockBytes;
    extract.block_size = flitzzip::kMinArchiveBlockBytes;
    const auto verified = flitzzip::verify_flzip(archive, extract);
    REQUIRE_EQ(verified.output_bytes, static_cast<std::uint64_t>(payload.size()));

    const auto output = root / "out";
    (void)flitzzip::extract_flzip(archive, output, extract);
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
TEST_CASE(flzip_optional_gpu_fallback_reports_zero_gpu_telemetry_without_device) {
    if (flitzzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("flzip-optional-gpu-fallback-telemetry");
    const auto input = root / "input.bin";
    {
        std::ofstream out(input, std::ios::binary);
        for (int i = 0; i < 8192; ++i) {
            out << "optional gpu fallback telemetry should stay CPU-visible only\n";
        }
    }
    const auto archive = root / "archive.flzip";

    flitzzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.force_cpu = false;
    compress.verify_after_write = true;
    const auto compressed = flitzzip::compress_flzip({input}, archive, compress);
    REQUIRE_TRUE(!compressed.gpu_used);
    REQUIRE_EQ(compressed.gpu_runtime.encode_chunks, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(compressed.gpu_runtime.decode_chunks, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(compressed.gpu_runtime.kernel_launches, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(compressed.gpu_runtime.h2d_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(compressed.gpu_runtime.d2h_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(compressed.gpu_runtime.device_allocation_bytes, static_cast<std::uint64_t>(0));

    flitzzip::ExtractOptions verify;
    verify.gpu_required = false;
    verify.force_cpu = false;
    const auto verified = flitzzip::verify_flzip(archive, verify);
    REQUIRE_TRUE(!verified.gpu_used);
    REQUIRE_EQ(verified.gpu_runtime.encode_chunks, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(verified.gpu_runtime.decode_chunks, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(verified.gpu_runtime.kernel_launches, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(verified.gpu_runtime.h2d_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(verified.gpu_runtime.d2h_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(verified.gpu_runtime.device_allocation_bytes, static_cast<std::uint64_t>(0));

    const auto output = root / "out";
    flitzzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.force_cpu = false;
    extract.overwrite = true;
    const auto extracted = flitzzip::extract_flzip(archive, output, extract);
    REQUIRE_TRUE(!extracted.gpu_used);
    REQUIRE_EQ(extracted.gpu_runtime.encode_chunks, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(extracted.gpu_runtime.decode_chunks, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(extracted.gpu_runtime.kernel_launches, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(extracted.gpu_runtime.h2d_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(extracted.gpu_runtime.d2h_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_EQ(extracted.gpu_runtime.device_allocation_bytes, static_cast<std::uint64_t>(0));

    std::filesystem::remove_all(root);
}

// Purpose: Verify native FLZIP uses deflated blocks for text-heavy data instead of storing everything raw.
// Inputs: A repetitive log-like file large enough to amortize archive metadata.
// Outputs: Throws if the archive is not smaller than the source data or if verification fails.
TEST_CASE(flzip_compresses_text_heavy_payload) {
    const auto root = test_temp_dir("flzip-text-compression");
    const auto input = root / "log.txt";
    {
        std::ofstream out(input, std::ios::binary);
        for (int i = 0; i < 32768; ++i) {
            out << "FlitzZip structured benchmark line with repeated fields and timestamps 2026-06-14\n";
        }
    }
    const auto source_size = std::filesystem::file_size(input);
    const auto archive = root / "archive.flzip";

    flitzzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.force_cpu = true;
    const auto compressed = flitzzip::compress_flzip({input}, archive, compress);
    REQUIRE_TRUE(compressed.output_bytes < source_size / 4U);

    flitzzip::ExtractOptions verify;
    verify.gpu_required = false;
    verify.force_cpu = true;
    (void)flitzzip::verify_flzip(archive, verify);
    std::filesystem::remove_all(root);
}

// Purpose: Verify the required-HIP encoder produces GPU-supported FLZIP blocks without CPU deflate.
// Inputs: A repetitive payload compressed with `gpu_required` on an AMD HIP host.
// Outputs: Requires preserved encode-stage statistics after read-back verification and HIP-only roundtrips.
TEST_CASE(flzip_required_gpu_encoder_emits_no_cpu_deflate_blocks) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("flzip-required-gpu-no-deflate");
    const auto input = root / "gpu.txt";
    {
        std::ofstream out(input, std::ios::binary);
        for (int i = 0; i < 32768; ++i) {
            out << "FlitzZip required HIP codec should stay separate from CPU deflate.\n";
        }
    }
    const auto archive = root / "archive.flzip";

    flitzzip::CompressOptions compress;
    compress.gpu_required = true;
    compress.force_cpu = false;
    compress.chunk_size = 128 * 1024;
    compress.block_size = 64 * 1024;
    compress.verify_after_write = true;
    const auto compressed = flitzzip::compress_flzip({input}, archive, compress);
    REQUIRE_TRUE(compressed.gpu_used);
    REQUIRE_TRUE(compressed.gpu_runtime.encode_chunks > 0);
    REQUIRE_TRUE(compressed.gpu_runtime.kernel_launches > 0);

    REQUIRE_TRUE(compressed.gpu_runtime.encode_stage_worker_seconds[static_cast<std::size_t>(
                     flitzzip::GpuEncodeStage::DeviceClassification)] > 0.0);
    double classification_total = 0.0;
    for (const auto value : compressed.gpu_runtime.classification_stage_worker_seconds) {
        REQUIRE_TRUE(std::isfinite(value) && value >= 0.0);
        classification_total += value;
    }
    REQUIRE_TRUE(classification_total > 0.0);
    const auto classification_parent =
        compressed.gpu_runtime
            .encode_stage_worker_seconds[static_cast<std::size_t>(flitzzip::GpuEncodeStage::DeviceClassification)];
    // Four converted microsecond counters may round above their independently converted parent by a few ULPs.
    const auto rounding_allowance = 8.0 * std::numeric_limits<double>::epsilon() * classification_parent;
    REQUIRE_TRUE(classification_total <= classification_parent ||
                 classification_total - classification_parent <= rounding_allowance);

    const auto index = read_test_archive_index(archive);
    REQUIRE_TRUE(!archive_contains_block_kind(index, flitzzip::BlockKind::Deflate));

    flitzzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 128 * 1024;
    verify.block_size = 64 * 1024;
    const auto verified = flitzzip::verify_flzip(archive, verify);
    REQUIRE_TRUE(verified.gpu_used);

    const auto output = root / "out";
    verify.overwrite = true;
    const auto extracted = flitzzip::extract_flzip(archive, output, verify);
    REQUIRE_TRUE(extracted.gpu_used);
    REQUIRE_EQ(std::filesystem::file_size(output / "gpu.txt"), std::filesystem::file_size(input));
    std::filesystem::remove_all(root);
}

// Purpose: Verify required-HIP verification refuses archives that need CPU Zstandard.
// Inputs: A force-CPU text archive that intentionally contains Zstandard blocks.
// Outputs: Throws if `gpu_required` silently invokes the CPU Zstandard codec.
TEST_CASE(flzip_required_gpu_rejects_cpu_zstd_archive) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("flzip-required-gpu-rejects-zstd");
    const auto input = root / "cpu-zstd.txt";
    {
        std::ofstream out(input, std::ios::binary);
        for (int i = 0; i < 32768; ++i) {
            out << "CPU Zstandard archive block for required HIP rejection.\n";
        }
    }
    const auto archive = root / "archive.flzip";

    flitzzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.force_cpu = true;
    compress.chunk_size = 128 * 1024;
    compress.block_size = 64 * 1024;
    const auto compressed = flitzzip::compress_flzip({input}, archive, compress);
    REQUIRE_TRUE(!compressed.gpu_used);
    REQUIRE_TRUE(compressed.output_bytes < std::filesystem::file_size(input) / 4U);

    const auto index = read_test_archive_index(archive);
    REQUIRE_EQ(index.version, 6U);
    REQUIRE_TRUE(archive_contains_block_kind(index, flitzzip::BlockKind::CpuZstd));

    flitzzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 128 * 1024;
    verify.block_size = 64 * 1024;
    bool rejected = false;
    try {
        (void)flitzzip::verify_flzip(archive, verify);
    } catch (const flitzzip::GpuError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);

    const auto output = root / "out";
    verify.overwrite = true;
    rejected = false;
    try {
        (void)flitzzip::extract_flzip(archive, output, verify);
    } catch (const flitzzip::GpuError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(output / "cpu-zstd.txt"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify secure extraction refuses accidental overwrite by default.
// Inputs: A `.flzip` archive and preexisting extraction target file.
// Outputs: Throws if overwrite is not rejected with `SecurityError`.
TEST_CASE(flzip_rejects_overwrite_by_default) {
    const auto root = test_temp_dir("flzip-overwrite");
    const auto input = root / "file.txt";
    std::ofstream(input) << "first";
    const auto archive = root / "archive.flzip";
    flitzzip::CompressOptions compress;
    compress.gpu_required = false;
    (void)flitzzip::compress_flzip({input}, archive, compress);
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    std::ofstream(output / "file.txt") << "existing";
    flitzzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.overwrite = false;
    bool rejected = false;
    try {
        (void)flitzzip::extract_flzip(archive, output, extract);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify `.flzip` payload corruption is detected during verification.
// Inputs: A valid archive with one payload byte flipped.
// Outputs: Throws if verification does not reject the corrupted archive.
TEST_CASE(flzip_verify_rejects_corrupt_payload) {
    const auto root = test_temp_dir("flzip-corrupt");
    const auto input = root / "file.bin";
    std::ofstream(input, std::ios::binary) << "abcdefabcdefabcdef";
    const auto archive = root / "archive.flzip";
    flitzzip::CompressOptions compress;
    compress.gpu_required = false;
    (void)flitzzip::compress_flzip({input}, archive, compress);
    {
        std::fstream file(archive, std::ios::binary | std::ios::in | std::ios::out);
        char value = 0;
        file.seekg(0, std::ios::beg);
        file.read(&value, 1);
        value ^= 0x7F;
        file.seekp(0, std::ios::beg);
        file.write(&value, 1);
    }
    flitzzip::ExtractOptions extract;
    extract.gpu_required = false;
    bool rejected = false;
    try {
        (void)flitzzip::verify_flzip(archive, extract);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify failed extraction removes decoded temporary data before publishing a final file.
// Inputs: A raw-payload archive with one payload byte modified to force a CRC mismatch after decode.
// Outputs: Throws if extraction succeeds or leaves a partial target file behind.
TEST_CASE(flzip_extract_removes_partial_file_after_crc_failure) {
    const auto root = test_temp_dir("flzip-extract-crc-cleanup");
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
    const auto archive = root / "archive.flzip";
    flitzzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.force_cpu = true;
    compress.chunk_size = 64 * 1024;
    compress.block_size = 16 * 1024;
    (void)flitzzip::compress_flzip({input}, archive, compress);

    const auto index = read_test_archive_index(archive);
    REQUIRE_TRUE(archive_contains_block_kind(index, flitzzip::BlockKind::Raw));
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
    flitzzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.force_cpu = true;
    extract.overwrite = true;
    extract.chunk_size = 64 * 1024;
    extract.block_size = 16 * 1024;
    bool rejected = false;
    try {
        (void)flitzzip::extract_flzip(archive, output, extract);
    } catch (const flitzzip::ArchiveError&) {
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
        (void)flitzzip::extract_flzip(archive, overwrite_output, extract);
    } catch (const flitzzip::ArchiveError&) {
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

// Purpose: Verify `.flzip` rejects archives that are too small to contain a footer.
// Inputs: A temporary file shorter than the fixed FlitzZip footer.
// Outputs: Throws if verification does not reject the truncated archive.
TEST_CASE(flzip_verify_rejects_truncated_footer) {
    const auto root = test_temp_dir("flzip-truncated-footer");
    const auto archive = root / "short.flzip";
    std::ofstream(archive, std::ios::binary) << "not a valid archive";
    flitzzip::ExtractOptions extract;
    bool rejected = false;
    try {
        (void)flitzzip::verify_flzip(archive, extract);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify `.flzip` rejects a footer whose index offset points outside the archive file.
// Inputs: A valid archive with the footer index offset overwritten beyond EOF.
// Outputs: Throws if verification trusts the corrupt footer.
TEST_CASE(flzip_verify_rejects_index_offset_outside_file) {
    const auto root = test_temp_dir("flzip-index-outside");
    const auto input = root / "file.bin";
    std::ofstream(input, std::ios::binary) << "payload";
    const auto archive = root / "archive.flzip";
    flitzzip::CompressOptions compress;
    compress.gpu_required = false;
    (void)flitzzip::compress_flzip({input}, archive, compress);
    const auto size = std::filesystem::file_size(archive);
    write_u64_at(archive, static_cast<std::streamoff>(size - 16), size + 1024);
    flitzzip::ExtractOptions extract;
    bool rejected = false;
    try {
        (void)flitzzip::verify_flzip(archive, extract);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify `.flzip` rejects an archive footer with a corrupt magic value.
// Inputs: A valid archive whose footer magic byte is modified.
// Outputs: Throws if verification accepts the corrupt footer.
TEST_CASE(flzip_verify_rejects_corrupt_footer_magic) {
    const auto root = test_temp_dir("flzip-footer-magic");
    const auto input = root / "file.bin";
    const std::string payload(4096, 'x');
    std::ofstream(input, std::ios::binary).write(payload.data(), static_cast<std::streamsize>(payload.size()));
    const auto archive = root / "archive.flzip";
    flitzzip::CompressOptions compress;
    compress.gpu_required = false;
    (void)flitzzip::compress_flzip({input}, archive, compress);
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
    flitzzip::ExtractOptions extract;
    bool rejected = false;
    try {
        (void)flitzzip::verify_flzip(archive, extract);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify FLZIP metadata validation enforces the archive-wide decoded-output policy before decode.
// Inputs: A compact fill-only index declaring one byte more than the 64 GiB aggregate output limit.
// Outputs: Throws `ArchiveError` without allocating or decoding the declared payload.
TEST_CASE(flzip_verify_rejects_aggregate_output_over_resource_limit) {
    const auto root = test_temp_dir("flzip-aggregate-output-limit");
    const auto archive = root / "oversized.flzip";
    {
        std::ofstream file(archive, std::ios::binary | std::ios::trunc);
        flitzzip::ArchiveIndex index;
        flitzzip::ArchiveEntry entry;
        entry.path = "oversized.bin";
        entry.uncompressed_size = flitzzip::kMaxExtractedOutputBytes + 1U;
        entry.payload_offset = 0;
        entry.payload_size = 0;
        entry.crc32 = 0;
        const auto full_blocks = flitzzip::kMaxExtractedOutputBytes / flitzzip::kMaxArchiveBlockBytes;
        entry.blocks.reserve(static_cast<std::size_t>(full_blocks + 1U));
        for (std::uint64_t i = 0; i < full_blocks; ++i) {
            entry.blocks.push_back(flitzzip::BlockDescriptor{
                .kind = flitzzip::BlockKind::Fill,
                .fill_value = 0,
                .uncompressed_len = flitzzip::kMaxArchiveBlockBytes,
                .encoded_offset = 0,
                .encoded_len = 0,
            });
        }
        entry.blocks.push_back(flitzzip::BlockDescriptor{
            .kind = flitzzip::BlockKind::Fill,
            .fill_value = 0,
            .uncompressed_len = 1,
            .encoded_offset = 0,
            .encoded_len = 0,
        });
        index.entries.push_back(std::move(entry));
        const auto index_offset = static_cast<std::uint64_t>(file.tellp());
        flitzzip::write_archive_index(file, index);
        const auto index_size = static_cast<std::uint64_t>(file.tellp()) - index_offset;
        write_test_footer(file, index_offset, index_size);
    }

    flitzzip::ExtractOptions verify;
    verify.gpu_required = false;
    verify.force_cpu = true;
    bool rejected = false;
    try {
        static_cast<void>(flitzzip::verify_flzip(archive, verify));
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify `.flzip` rejects Win32-disallowed control characters in entry paths before extraction.
// Inputs: A handcrafted archive with one raw entry whose name contains ASCII 0x1F.
// Outputs: Throws `SecurityError` before decoding or writing any file.
TEST_CASE(flzip_verify_rejects_control_character_entry_path) {
    const auto root = test_temp_dir("flzip-control-char-path");
    const auto archive = root / "archive.flzip";
    const std::string payload = "payload";
    {
        std::ofstream file(archive, std::ios::binary | std::ios::trunc);
        file.write(payload.data(), static_cast<std::streamsize>(payload.size()));

        flitzzip::ArchiveIndex index;
        flitzzip::ArchiveEntry entry;
        entry.path = std::string("dir/control") + static_cast<char>(0x1F) + ".txt";
        entry.uncompressed_size = payload.size();
        entry.payload_offset = 0;
        entry.payload_size = payload.size();
        entry.crc32 = flitzzip::crc32(std::as_bytes(std::span<const char>(payload.data(), payload.size())));
        entry.blocks.push_back(flitzzip::BlockDescriptor{
            .kind = flitzzip::BlockKind::Raw,
            .fill_value = 0,
            .uncompressed_len = static_cast<std::uint32_t>(payload.size()),
            .encoded_offset = 0,
            .encoded_len = static_cast<std::uint32_t>(payload.size()),
        });
        index.entries.push_back(std::move(entry));
        const auto index_offset = static_cast<std::uint64_t>(file.tellp());
        flitzzip::write_archive_index(file, index);
        const auto index_size = static_cast<std::uint64_t>(file.tellp()) - index_offset;
        write_test_footer(file, index_offset, index_size);
    }

    flitzzip::ExtractOptions verify;
    verify.gpu_required = false;
    bool rejected = false;
    try {
        (void)flitzzip::verify_flzip(archive, verify);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify archive-wide validation rejects duplicate normalized entry paths before payload decode.
// Inputs: A handcrafted archive containing `dir/file.txt` and the equivalent `dir//file.txt`.
// Outputs: Throws `SecurityError` during verification instead of accepting ambiguous extraction metadata.
TEST_CASE(flzip_verify_rejects_duplicate_normalized_entry_paths) {
    const auto root = test_temp_dir("flzip-duplicate-paths");
    const auto archive = root / "archive.flzip";
    write_raw_test_archive(archive, {
                                        RawArchiveTestEntry{.path = "dir/file.txt", .payload = "first"},
                                        RawArchiveTestEntry{.path = "dir//file.txt", .payload = "second"},
                                    });

    flitzzip::ExtractOptions verify;
    verify.gpu_required = false;
    verify.force_cpu = true;
    bool rejected = false;
    try {
        (void)flitzzip::verify_flzip(archive, verify);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify archive-wide validation rejects a file entry that blocks a child entry path.
// Inputs: A handcrafted archive containing file `dir` and child file `dir/child.txt`.
// Outputs: Throws before extraction creates any output path or decodes payload bytes.
TEST_CASE(flzip_extract_rejects_file_entry_with_child_entry) {
    const auto root = test_temp_dir("flzip-file-child-conflict");
    const auto archive = root / "archive.flzip";
    write_raw_test_archive(archive, {
                                        RawArchiveTestEntry{.path = "dir", .payload = "parent"},
                                        RawArchiveTestEntry{.path = "dir/child.txt", .payload = "child"},
                                    });

    const auto output = root / "out";
    flitzzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.force_cpu = true;
    extract.overwrite = true;
    bool rejected = false;
    try {
        (void)flitzzip::extract_flzip(archive, output, extract);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Exhaustively check the serialized byte kind against every readable native version.
// Inputs: One small in-memory descriptor, every kind byte and every readable version.
// Outputs: Requires exact kind preservation for supported values and ArchiveError for all other values.
TEST_CASE(flzip_index_block_kind_version_matrix) {
    flitzzip::ArchiveIndex index;
    index.version = flitzzip::kFlitzZipMaxReadableVersion;
    index.entries.push_back(flitzzip::ArchiveEntry{
        .path = "a",
        .blocks = {flitzzip::BlockDescriptor{.uncompressed_len = 4U, .encoded_len = 2U}},
    });
    std::ostringstream encoded(std::ios::binary);
    flitzzip::write_archive_index(encoded, index);
    const auto original = encoded.str();
    constexpr std::array<unsigned int, 14> first_versions{1U, 1U, 1U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U};
    REQUIRE_EQ(first_versions.back(), flitzzip::kFlitzZipMaxReadableVersion);
    constexpr std::size_t kind_offset = 12U + 2U + 1U + 1U + 24U + 4U + 4U;
    REQUIRE_EQ(original.size(), kind_offset + 18U);
    for (std::uint32_t version = 1U; version <= flitzzip::kFlitzZipMaxReadableVersion; ++version) {
        for (unsigned int byte = 0U; byte <= 255U; ++byte) {
            auto bytes = original;
            bytes[4] = static_cast<char>(version);
            bytes[kind_offset] = static_cast<char>(byte);
            std::istringstream input(bytes, std::ios::binary);
            bool rejected = false;
            try {
                const auto parsed = flitzzip::read_archive_index(input);
                REQUIRE_EQ(static_cast<unsigned int>(parsed.entries.front().blocks.front().kind), byte);
            } catch (const flitzzip::ArchiveError&) {
                rejected = true;
            }
            const bool supported = byte < first_versions.size() && version >= first_versions[byte];
            REQUIRE_EQ(rejected, !supported);
        }
    }
    for (std::size_t bytes = 0U; bytes < original.size(); ++bytes) {
        std::istringstream input(original.substr(0U, bytes), std::ios::binary);
        bool rejected = false;
        try {
            (void)flitzzip::read_archive_index(input);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Enforce per-path allocation limits at both native index boundaries.
// Inputs: A caller-owned oversized path and an untrusted length field without its claimed path payload.
// Outputs: Requires writer rejection before emitting bytes and reader rejection before consuming path bytes.
TEST_CASE(flzip_index_rejects_oversized_path_before_io) {
    flitzzip::ArchiveIndex index;
    index.entries.push_back(flitzzip::ArchiveEntry{.path = std::string(flitzzip::kMaxArchivePathBytes + 1U, 'a')});
    std::stringstream output(std::ios::in | std::ios::out | std::ios::binary);
    bool rejected = false;
    try {
        flitzzip::write_archive_index(output, index);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(output.str().empty());
    flitzzip::write_u32(output, flitzzip::kFlitzZipMagic);
    flitzzip::write_u32(output, flitzzip::kFlitzZipVersion);
    flitzzip::write_u32(output, 1U);
    flitzzip::write_u16(output, static_cast<std::uint16_t>(flitzzip::kMaxArchivePathBytes + 1U));
    output << std::string(35U, '\0');
    output.seekg(0);
    rejected = false;
    try {
        (void)flitzzip::read_archive_index(output);
    } catch (const flitzzip::ArchiveError& error) {
        REQUIRE_EQ(std::string(error.what()), "archive path exceeds FlitzZip resource limit");
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(output.tellg(), std::streampos(14));
}

// Purpose: Admit the shared path budget exactly and reject its first extra retained byte.
// Inputs: A RAM-only index containing 64 MiB of path metadata and one extra one-byte entry.
// Outputs: Requires boundary roundtrip, pre-output writer rejection and pre-allocation reader rejection.
TEST_CASE(flzip_index_path_metadata_budget_boundary) {
    flitzzip::ArchiveIndex index;
    const auto count =
        static_cast<std::uint32_t>(flitzzip::kMaxArchivePathMetadataBytes / flitzzip::kMaxArchivePathBytes);
    for (std::uint32_t entry = 0U; entry < count; ++entry) {
        index.entries.push_back(flitzzip::ArchiveEntry{
            .path = std::string(flitzzip::kMaxArchivePathBytes, 'a'),
            .directory = true,
        });
    }
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    flitzzip::write_archive_index(stream, index);
    stream.seekg(0);
    REQUIRE_EQ(flitzzip::read_archive_index(stream).entries.size(), count);
    index.entries.push_back(flitzzip::ArchiveEntry{.path = "b", .directory = true});
    std::ostringstream refused(std::ios::binary);
    bool rejected = false;
    try {
        flitzzip::write_archive_index(refused, index);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(refused.str().empty());
    stream.seekp(8);
    flitzzip::write_u32(stream, count + 1U);
    stream.seekp(0, std::ios::end);
    const auto extra_entry_offset = stream.tellp();
    flitzzip::write_u16(stream, 1U);
    stream << 'b' << '\1' << std::string(32U, '\0');
    stream.seekg(0);
    rejected = false;
    try {
        (void)flitzzip::read_archive_index(stream);
    } catch (const flitzzip::ArchiveError& error) {
        REQUIRE_EQ(std::string(error.what()), "archive index exceeds FlitzZip path metadata limit");
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(stream.tellg(), extra_entry_offset + std::streamoff(2));
}

// Purpose: Verify impossible archive-index entry counts are rejected before large allocations.
// Inputs: A seekable in-memory index header whose declared entry count cannot fit in the remaining bytes.
// Outputs: Throws `ArchiveError` instead of reserving attacker-controlled entry capacity.
TEST_CASE(flzip_index_parser_rejects_entry_count_exceeding_buffer) {
    std::stringstream input(std::ios::in | std::ios::out | std::ios::binary);
    flitzzip::write_u32(input, flitzzip::kFlitzZipMagic);
    flitzzip::write_u32(input, flitzzip::kFlitzZipVersion);
    flitzzip::write_u32(input, 1'000'000U);
    input.seekg(0, std::ios::beg);

    bool rejected = false;
    try {
        (void)flitzzip::read_archive_index(input);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify impossible per-entry block counts are rejected before large allocations.
// Inputs: A seekable in-memory index with one file entry declaring more blocks than the bytes can encode.
// Outputs: Throws `ArchiveError` before reserving attacker-controlled block capacity.
TEST_CASE(flzip_index_parser_rejects_block_count_exceeding_buffer) {
    std::stringstream input(std::ios::in | std::ios::out | std::ios::binary);
    flitzzip::write_u32(input, flitzzip::kFlitzZipMagic);
    flitzzip::write_u32(input, flitzzip::kFlitzZipVersion);
    flitzzip::write_u32(input, 1U);
    flitzzip::write_u16(input, 1U);
    input.put('a');
    input.put('\0');
    flitzzip::write_u64(input, 0U);
    flitzzip::write_u64(input, 0U);
    flitzzip::write_u64(input, 0U);
    flitzzip::write_u32(input, 0U);
    flitzzip::write_u32(input, 4'000'000U);
    input.seekg(0, std::ios::beg);

    bool rejected = false;
    try {
        (void)flitzzip::read_archive_index(input);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify compression rejects invalid resource options before filesystem work begins.
// Inputs: A small source file and a zero chunk size that would otherwise risk an infinite streaming loop.
// Outputs: Throws if invalid chunking options are not rejected.
TEST_CASE(flzip_rejects_zero_chunk_size) {
    const auto root = test_temp_dir("flzip-zero-chunk");
    const auto input = root / "input.bin";
    std::ofstream(input, std::ios::binary) << "bounded resources";
    flitzzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.chunk_size = 0;

    bool rejected = false;
    try {
        (void)flitzzip::compress_flzip({input}, root / "archive.flzip", compress);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify compression rejects invalid deflate levels before archive output is created.
// Inputs: A small source file and a compression level outside the supported miniz range.
// Outputs: Throws if invalid compression effort is accepted.
TEST_CASE(flzip_rejects_invalid_compression_level) {
    const auto root = test_temp_dir("flzip-invalid-compression-level");
    const auto input = root / "input.bin";
    std::ofstream(input, std::ios::binary) << "bounded compression level";
    flitzzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.compression_level = 10;

    bool rejected = false;
    try {
        (void)flitzzip::compress_flzip({input}, root / "archive.flzip", compress);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify explicit in-flight chunk settings cannot bypass the archive memory guard.
// Inputs: A small source file and an in-flight chunk count above the supported ceiling.
// Outputs: Throws if compression accepts an unbounded buffering request.
TEST_CASE(flzip_rejects_excessive_inflight_chunks) {
    const auto root = test_temp_dir("flzip-excessive-inflight");
    const auto input = root / "input.bin";
    std::ofstream(input, std::ios::binary) << "bounded in-flight chunks";
    flitzzip::CompressOptions compress;
    compress.gpu_required = false;
    compress.max_inflight_chunks = flitzzip::kMaxInflightArchiveChunks + 1U;

    bool rejected = false;
    try {
        (void)flitzzip::compress_flzip({input}, root / "archive.flzip", compress);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify sparse raw payload metadata is rejected before allocating a large payload window.
// Inputs: A handcrafted archive whose second raw block starts far after the first raw block.
// Outputs: Throws if verification accepts sparse or overlapping raw block layout.
TEST_CASE(flzip_verify_rejects_sparse_raw_payload_layout) {
    const auto root = test_temp_dir("flzip-sparse-payload");
    const auto archive = root / "archive.flzip";
    {
        std::ofstream file(archive, std::ios::binary | std::ios::trunc);
        constexpr std::uint32_t block_size = flitzzip::kDefaultArchiveBlockBytes;
        const std::string payload((static_cast<std::size_t>(block_size) * 2U) + 1U, 'x');
        file.write(payload.data(), static_cast<std::streamsize>(payload.size()));

        flitzzip::ArchiveIndex index;
        flitzzip::ArchiveEntry entry;
        entry.path = "file.bin";
        entry.uncompressed_size = static_cast<std::uint64_t>(block_size) * 2U;
        entry.payload_offset = 0;
        entry.payload_size = payload.size();
        entry.blocks.push_back(flitzzip::BlockDescriptor{
            .kind = flitzzip::BlockKind::Raw,
            .fill_value = 0,
            .uncompressed_len = block_size,
            .encoded_offset = 0,
            .encoded_len = block_size,
        });
        entry.blocks.push_back(flitzzip::BlockDescriptor{
            .kind = flitzzip::BlockKind::Raw,
            .fill_value = 0,
            .uncompressed_len = block_size,
            .encoded_offset = static_cast<std::uint64_t>(block_size) + 1U,
            .encoded_len = block_size,
        });
        index.entries.push_back(std::move(entry));
        const auto index_offset = static_cast<std::uint64_t>(file.tellp());
        flitzzip::write_archive_index(file, index);
        const auto index_size = static_cast<std::uint64_t>(file.tellp()) - index_offset;
        write_test_footer(file, index_offset, index_size);
    }

    flitzzip::ExtractOptions verify;
    verify.gpu_required = false;
    bool rejected = false;
    try {
        (void)flitzzip::verify_flzip(archive, verify);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify CPU extraction can read compact pattern blocks emitted by the AMD HIP encoder.
// Inputs: A handcrafted `.flzip` archive with one repeated four-byte pattern block.
// Outputs: Throws if forced-CPU extraction fails or expands incorrect content.
TEST_CASE(flzip_cpu_extracts_gpu_pattern_block) {
    const auto root = test_temp_dir("flzip-pattern-block");
    const auto archive = root / "archive.flzip";
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

        flitzzip::ArchiveIndex index;
        flitzzip::ArchiveEntry entry;
        entry.path = "pattern.bin";
        entry.uncompressed_size = expanded.size();
        entry.payload_offset = 0;
        entry.payload_size = pattern.size();
        entry.crc32 = flitzzip::crc32(std::span<const std::byte>(expanded.data(), expanded.size()));
        entry.blocks.push_back(flitzzip::BlockDescriptor{
            .kind = flitzzip::BlockKind::Pattern,
            .fill_value = 0,
            .uncompressed_len = static_cast<std::uint32_t>(expanded.size()),
            .encoded_offset = 0,
            .encoded_len = static_cast<std::uint32_t>(pattern.size()),
        });
        index.entries.push_back(std::move(entry));
        const auto index_offset = static_cast<std::uint64_t>(file.tellp());
        flitzzip::write_archive_index(file, index);
        const auto index_size = static_cast<std::uint64_t>(file.tellp()) - index_offset;
        write_test_footer(file, index_offset, index_size);
    }

    const auto output = root / "out";
    flitzzip::ExtractOptions extract;
    extract.gpu_required = false;
    extract.force_cpu = true;
    extract.overwrite = true;
    (void)flitzzip::extract_flzip(archive, output, extract);
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
TEST_CASE(flzip_verify_rejects_invalid_gpu_pattern_metadata) {
    const auto root = test_temp_dir("flzip-invalid-pattern-block");
    const auto archive = root / "archive.flzip";
    {
        std::ofstream file(archive, std::ios::binary | std::ios::trunc);
        const std::array<char, 2> pattern{'x', 'y'};
        file.write(pattern.data(), static_cast<std::streamsize>(pattern.size()));

        flitzzip::ArchiveIndex index;
        flitzzip::ArchiveEntry entry;
        entry.path = "pattern.bin";
        entry.uncompressed_size = 4096;
        entry.payload_offset = 0;
        entry.payload_size = pattern.size();
        entry.blocks.push_back(flitzzip::BlockDescriptor{
            .kind = flitzzip::BlockKind::Pattern,
            .fill_value = 0,
            .uncompressed_len = 4096,
            .encoded_offset = 0,
            .encoded_len = static_cast<std::uint32_t>(pattern.size()),
        });
        index.entries.push_back(std::move(entry));
        const auto index_offset = static_cast<std::uint64_t>(file.tellp());
        flitzzip::write_archive_index(file, index);
        const auto index_size = static_cast<std::uint64_t>(file.tellp()) - index_offset;
        file.seekp(-static_cast<std::streamoff>(sizeof(std::uint32_t)), std::ios::cur);
        flitzzip::write_u32(file, 1U);
        write_test_footer(file, index_offset, index_size);
    }

    flitzzip::ExtractOptions verify;
    verify.gpu_required = false;
    verify.force_cpu = true;
    bool rejected = false;
    try {
        (void)flitzzip::verify_flzip(archive, verify);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Check pure memory admission, then exercise the public Neutron boundary only with available HIP.
// Inputs: Maximal policy counters; available HIP also uses tiny caller files and 64 retained 128 MiB windows.
// Outputs: Always proves the pure cap; available HIP rejects before archive output/progress/encoding and recovers.
TEST_CASE(flzip_neutron_inflight_memory_admission_preserves_destination_and_recovers) {
    constexpr std::uint64_t chunk_bytes = 128ULL * 1024ULL * 1024ULL;
    // Independent policy arithmetic: 3 stream extents plus 6 Neutron intermediates = 72 GiB total.
    REQUIRE_TRUE(64ULL * 9ULL * chunk_bytes > flitzzip::kMaxPipelineMemoryBytes);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto unlimited_limit = flitzzip::resolve_host_pipeline_inflight_limit({maximum, maximum}, chunk_bytes, 0U,
                                                                                {.per_window_bytes = 6U * chunk_bytes});
    REQUIRE_EQ(unlimited_limit, 56U);
    // Production capability policy queries HIP before the host-memory budget; never treat its refusal as memory.
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const auto root = test_temp_dir("flzip-neutron-inflight-admission");
    const auto source = root / "input.bin";
    const auto archive = root / "archive.flzip";
    const std::string payload = "Independent admission recovery bytes";
    std::ofstream(source, std::ios::binary) << payload;
    std::ofstream(archive, std::ios::binary) << "caller-owned destination";
    flitzzip::CompressOptions refused;
    refused.compression_mode = flitzzip::NativeCompressionMode::NeutronStar;
    refused.compression_level = 9;
    refused.gpu_required = true;
    refused.chunk_size = chunk_bytes;
    refused.worker_count = 64U;
    refused.max_inflight_chunks = 64U;
    int progress_calls = 0;
    bool rejected = false;
    try {
        (void)flitzzip::compress_flzip({source}, archive, refused,
                                       [&](const flitzzip::ProgressSnapshot&) { ++progress_calls; });
    } catch (const flitzzip::ArchiveError& error) {
        const std::string_view message(error.what());
        rejected = message == "requested in-flight chunks exceed FlitzZip memory budget" ||
                   message == "insufficient physical memory for one archive window within the 80% host RAM target; "
                              "retry when more memory is available";
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(progress_calls, 0);
    {
        std::ifstream input(archive, std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "caller-owned destination");
    }
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}), 2);
    flitzzip::CompressOptions admitted;
    admitted.gpu_required = false;
    admitted.force_cpu = true;
    admitted.chunk_size = 4096U;
    admitted.block_size = 4096U;
    admitted.worker_count = 1U;
    admitted.max_inflight_chunks = 1U;
    const auto encoded = flitzzip::compress_flzip({source}, archive, admitted);
    REQUIRE_EQ(encoded.input_bytes, payload.size());
    flitzzip::ExtractOptions decoded;
    decoded.gpu_required = false;
    decoded.force_cpu = true;
    decoded.chunk_size = 4096U;
    decoded.block_size = 4096U;
    const auto destination = root / "restored";
    REQUIRE_EQ(flitzzip::extract_flzip(archive, destination, decoded).output_bytes, payload.size());
    std::ifstream restored(destination / "input.bin", std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(restored), {}), payload);
    std::ifstream unchanged(source, std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(unchanged), {}), payload);
}

namespace {

// Independent version-six raw native frame and Python standard-library stored ZIP, each with one self-named member.
constexpr std::array<unsigned char, 136> kNativeSelfCollisionWire{
    0x69, 0x6E, 0x64, 0x65, 0x70, 0x65, 0x6E, 0x64, 0x65, 0x6E, 0x74, 0x20, 0x73, 0x65, 0x6C, 0x66, 0x2D,
    0x63, 0x6F, 0x6C, 0x6C, 0x69, 0x73, 0x69, 0x6F, 0x6E, 0x20, 0x70, 0x61, 0x79, 0x6C, 0x6F, 0x61, 0x64,
    0x0A, 0x53, 0x55, 0x5A, 0x50, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x73, 0x6F,
    0x75, 0x72, 0x63, 0x65, 0x2E, 0x66, 0x6C, 0x7A, 0x69, 0x70, 0x00, 0x23, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x34, 0xCA, 0x05, 0xC7, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0x00, 0x53, 0x55, 0x5A, 0x46, 0x06, 0x00, 0x00,
    0x00, 0x23, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

constexpr std::array<unsigned char, 153> kZipSelfCollisionWire{
    0x50, 0x4B, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x83, 0x18, 0x22, 0x50, 0x34, 0xCA, 0x05,
    0xC7, 0x23, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x73, 0x6F, 0x75, 0x72,
    0x63, 0x65, 0x2E, 0x7A, 0x69, 0x70, 0x69, 0x6E, 0x64, 0x65, 0x70, 0x65, 0x6E, 0x64, 0x65, 0x6E, 0x74,
    0x20, 0x73, 0x65, 0x6C, 0x66, 0x2D, 0x63, 0x6F, 0x6C, 0x6C, 0x69, 0x73, 0x69, 0x6F, 0x6E, 0x20, 0x70,
    0x61, 0x79, 0x6C, 0x6F, 0x61, 0x64, 0x0A, 0x50, 0x4B, 0x01, 0x02, 0x14, 0x00, 0x14, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x83, 0x18, 0x22, 0x50, 0x34, 0xCA, 0x05, 0xC7, 0x23, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00,
    0x00, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x73, 0x6F, 0x75, 0x72, 0x63, 0x65, 0x2E, 0x7A, 0x69, 0x70, 0x50, 0x4B, 0x05, 0x06, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x38, 0x00, 0x00, 0x00, 0x4B, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// Purpose: Observe DELETE access without deleting, renaming or mutating an owned archive.
// Inputs: Existing test-owned archive; error receives the immediate native refusal. Outputs: Admission, no live handle.
bool archive_delete_access_available(const std::filesystem::path& path, DWORD& error) {
    const auto raw = CreateFileW(path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    error = raw == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    flitzzip::windows_detail::HandleOwner owner(raw);
    return owner != nullptr && owner.get() != INVALID_HANDLE_VALUE;
}

// Purpose: Materialize independently prepared tiny archive bytes without invoking any product writer.
// Inputs: Owned path and exact wire bytes. Outputs: Complete closed fixture or a stream exception.
void write_self_collision_wire(const std::filesystem::path& path, std::span<const unsigned char> wire) {
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(reinterpret_cast<const char*>(wire.data()), static_cast<std::streamsize>(wire.size()));
    output.close();
}

}  // namespace

// Purpose: Protect an archive when one member would replace that same source, then publish into a clean destination.
// Inputs: Independent tiny native/ZIP bytes, CPU native decoding and each real public extraction operation.
// Outputs: Valid controls and recovery restore literal bytes; collision preserves archive/caller data and cleans
// staging.
TEST_CASE(archive_source_self_collision_preserves_native_and_zip_inputs_then_recovers) {
    struct Fixture {
        const char* filename;
        std::span<const unsigned char> wire;
        bool native;
    };
    const std::array fixtures{Fixture{"source.flzip", kNativeSelfCollisionWire, true},
                              Fixture{"source.zip", kZipSelfCollisionWire, false}};
    const auto root = test_temp_dir("archive-source-self-collision");
    const std::string payload = "independent self-collision payload\n";
    unsigned ordinal = 0;
    for (const auto& fixture : fixtures) {
        const auto directory = root / std::to_string(ordinal++);
        std::filesystem::create_directory(directory);
        const auto archive = directory / fixture.filename;
        write_self_collision_wire(archive, fixture.wire);
        std::ofstream(directory / "caller.txt", std::ios::binary) << "unrelated caller data";
        const std::string original(reinterpret_cast<const char*>(fixture.wire.data()), fixture.wire.size());
        REQUIRE_EQ(flitzzip_test::source_session_fixture_bytes(archive), original);
        flitzzip::ExtractOptions options;
        options.gpu_required = false;
        options.force_cpu = true;
        options.overwrite = true;
        options.chunk_size = options.block_size = 4096U;
        options.worker_count = options.max_inflight_chunks = 1U;
        // Purpose: Use the real decoder. Inputs: Owned destination/observer. Outputs: Actual extraction statistics.
        const auto extract = [&](const auto& destination, const flitzzip::ProgressCallback& callback) {
            return fixture.native ? flitzzip::extract_flzip(archive, destination, options, callback)
                                  : flitzzip::extract_zip(archive, destination, true, callback);
        };
        const auto control = root / ("control-" + std::to_string(ordinal));
        REQUIRE_EQ(extract(control, {}).output_bytes, payload.size());
        REQUIRE_EQ(flitzzip_test::source_session_fixture_bytes(control / fixture.filename), payload);
        DWORD access_error = ERROR_SUCCESS;
        REQUIRE_TRUE(archive_delete_access_available(archive, access_error));
        unsigned callbacks = 0;
        bool refused = false;
        try {
            (void)extract(directory, [&](const flitzzip::ProgressSnapshot&) {
                ++callbacks;
                REQUIRE_TRUE(!archive_delete_access_available(archive, access_error));
                REQUIRE_EQ(access_error, static_cast<DWORD>(ERROR_SHARING_VIOLATION));
            });
        } catch (const flitzzip::ArchiveError& error) {
            const std::string_view text(error.what());
            refused = text.starts_with("failed to publish verified output:") &&
                      (text.find("(Windows error 5)") != text.npos || text.find("(Windows error 32)") != text.npos);
        }
        REQUIRE_TRUE(refused && callbacks > 0U);
        REQUIRE_EQ(flitzzip_test::source_session_fixture_bytes(archive), original);
        REQUIRE_EQ(flitzzip_test::source_session_fixture_bytes(directory / "caller.txt"), "unrelated caller data");
        REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}),
                   2);
        REQUIRE_TRUE(archive_delete_access_available(archive, access_error));
        const auto recovery = root / ("recovery-" + std::to_string(ordinal));
        REQUIRE_EQ(extract(recovery, {}).output_bytes, payload.size());
        REQUIRE_EQ(flitzzip_test::source_session_fixture_bytes(recovery / fixture.filename), payload);
        REQUIRE_EQ(flitzzip_test::source_session_fixture_bytes(archive), original);
        REQUIRE_TRUE(archive_delete_access_available(archive, access_error));
    }
}
