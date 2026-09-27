#include "core/archive_index.hpp"
#include "core/archive.hpp"
#include "core/checksum.hpp"
#include "core/file_publish.hpp"
#include "core/result.hpp"
#include "core/resource_limits.hpp"
#include "gpu/gpu_codec.hpp"
#include "lz4.h"
#include "miniz.h"
#include "test_suzip_helpers.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
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

using namespace superzip_test;

struct RawArchiveTestEntry {
    std::string path;
    std::string payload;
};

// Purpose: Write a compact handcrafted SUZIP archive with raw file entries.
// Inputs: `path`, `entries`, and `version` supply archive contents and its declared native version.
// Outputs: Writes payloads, index, and footer for metadata validation without production compression.
void write_raw_test_archive(const std::filesystem::path& path, const std::vector<RawArchiveTestEntry>& entries,
                            std::uint32_t version = superzip::kSuperZipVersion) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    superzip::ArchiveIndex index;
    index.version = version;
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
    write_test_footer(file, index_offset, index_size, version);
}

// Purpose: Wrap one pre-encoded block in a native archive with an explicit format version.
// Inputs: `path` is the output file, `payload` contains one block, and `decoded` supplies its expected CRC data.
// Outputs: Writes a complete one-file archive for public read-path validation.
void write_encoded_test_archive(const std::filesystem::path& path, std::span<const std::byte> payload,
                                std::string_view decoded, superzip::BlockKind kind, std::uint32_t version) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    superzip::ArchiveIndex index;
    index.version = version;
    superzip::ArchiveEntry entry;
    entry.path = "data.bin";
    entry.uncompressed_size = decoded.size();
    entry.payload_size = payload.size();
    entry.crc32 = superzip::crc32(std::as_bytes(std::span(decoded.data(), decoded.size())));
    entry.blocks.push_back(superzip::BlockDescriptor{
        .kind = kind,
        .uncompressed_len = static_cast<std::uint32_t>(decoded.size()),
        .encoded_len = static_cast<std::uint32_t>(payload.size()),
    });
    index.entries.push_back(std::move(entry));
    const auto index_offset = static_cast<std::uint64_t>(file.tellp());
    superzip::write_archive_index(file, index);
    const auto index_size = static_cast<std::uint64_t>(file.tellp()) - index_offset;
    write_test_footer(file, index_offset, index_size, index.version);
}

// Purpose: Wrap one pre-encoded dictionary payload in a version-four native archive.
// Inputs: `path` is the output file, `payload` includes its segment table, and `decoded` supplies expected CRC data.
// Outputs: Writes a complete one-file archive for public read-path validation.
void write_dictionary_test_archive(const std::filesystem::path& path, std::span<const std::byte> payload,
                                   std::string_view decoded) {
    write_encoded_test_archive(path, payload, decoded, superzip::BlockKind::GpuDictionary, 4U);
}

}  // namespace

// Purpose: Require the native footer and index to declare the same readable format version.
// Inputs: Raw archives at every readable version, followed by one archive with a changed footer version.
// Outputs: Valid archives verify and a version mismatch is rejected before payload decoding.
TEST_CASE(suzip_rejects_footer_index_version_mismatch) {
    const auto root = test_temp_dir("suzip-version-consistency");
    superzip::ExtractOptions options;
    options.force_cpu = true;
    options.gpu_required = false;
    for (std::uint32_t version = superzip::kSuperZipMinReadableVersion;
         version <= superzip::kSuperZipMaxReadableVersion; ++version) {
        const auto archive = root / ("version-" + std::to_string(version) + ".suzip");
        write_raw_test_archive(archive, {{"data.txt", "test payload"}}, version);
        REQUIRE_EQ(superzip::verify_suzip(archive, options).entries, 1U);
    }

    const auto archive = root / ("version-" + std::to_string(superzip::kSuperZipVersion) + ".suzip");
    const auto footer_version_offset = static_cast<std::streamoff>(std::filesystem::file_size(archive) - 20U);
    {
        std::fstream file(archive, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(footer_version_offset, std::ios::beg);
        superzip::write_u32(file, superzip::kSuperZipVersion - 1U);
        REQUIRE_TRUE(static_cast<bool>(file));
    }
    bool rejected = false;
    try {
        static_cast<void>(superzip::verify_suzip(archive, options));
    } catch (const superzip::ArchiveError& error) {
        rejected = std::string(error.what()).find("footer and index versions differ") != std::string::npos;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Prevent older native versions from claiming block encodings they do not define.
// Inputs: Prefix, dictionary, sparse, and Zstandard descriptors paired with pre-introduction versions.
// Outputs: Both the writer and parser reject unsupported version/block combinations.
TEST_CASE(suzip_rejects_block_kind_version_downgrade) {
    for (const auto [version, kind] :
         {std::pair{1U, superzip::BlockKind::GpuPrefix}, std::pair{1U, superzip::BlockKind::GpuAdaptivePrefix},
          std::pair{2U, superzip::BlockKind::GpuAdaptivePrefix}, std::pair{3U, superzip::BlockKind::GpuDictionary},
          std::pair{4U, superzip::BlockKind::GpuSparsePattern}, std::pair{5U, superzip::BlockKind::CpuZstd}}) {
        superzip::ArchiveIndex index;
        index.version = version;
        superzip::ArchiveEntry entry;
        entry.path = "x";
        entry.blocks.push_back(superzip::BlockDescriptor{.kind = kind});
        index.entries.push_back(entry);

        bool writer_rejected = false;
        try {
            std::ostringstream output(std::ios::out | std::ios::binary);
            superzip::write_archive_index(output, index);
        } catch (const superzip::ArchiveError&) {
            writer_rejected = true;
        }
        REQUIRE_TRUE(writer_rejected);

        index.version = superzip::kSuperZipMaxReadableVersion;
        std::ostringstream output(std::ios::out | std::ios::binary);
        superzip::write_archive_index(output, index);
        auto forged = output.str();
        forged[4] = static_cast<char>(version);
        std::istringstream input(forged, std::ios::in | std::ios::binary);
        bool reader_rejected = false;
        try {
            static_cast<void>(superzip::read_archive_index(input));
        } catch (const superzip::ArchiveError&) {
            reader_rejected = true;
        }
        REQUIRE_TRUE(reader_rejected);
    }
}

// Purpose: Exercise version-five sparse blocks through public CPU/HIP verification and extraction.
// Inputs: A four-byte motif, two sorted corrections, and a malformed redundant-correction variant.
// Outputs: Exact output roundtrips; noncanonical payload fails before publication.
TEST_CASE(suzip_sparse_pattern_cpu_reader_roundtrip) {
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
    const auto root = test_temp_dir("suzip-sparse-pattern-v5");
    const auto archive = root / "sparse.suzip";
    write_encoded_test_archive(archive, payload, decoded, superzip::BlockKind::GpuSparsePattern, 5U);
    REQUIRE_EQ(read_test_archive_index(archive).version, 5U);
    for (const bool hip : {false, true}) {
        if (hip && !superzip::query_gpu_info().available) {
            continue;
        }
        superzip::ExtractOptions options;
        options.force_cpu = !hip;
        options.gpu_required = hip;
        REQUIRE_EQ(superzip::verify_suzip(archive, options).gpu_used, hip);
        const auto destination = root / (hip ? "gpu" : "cpu");
        REQUIRE_EQ(superzip::extract_suzip(archive, destination, options).gpu_used, hip);
        std::ifstream restored(destination / "data.bin", std::ios::binary);
        REQUIRE_TRUE(restored.is_open());
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(restored), std::istreambuf_iterator<char>()), decoded);
    }

    auto invalid = payload;
    invalid[16U] = std::byte{'B'};
    const auto corrupt = root / "invalid.suzip";
    write_encoded_test_archive(corrupt, invalid, decoded, superzip::BlockKind::GpuSparsePattern, 5U);
    superzip::ExtractOptions options;
    options.force_cpu = true;
    options.gpu_required = false;
    bool rejected = false;
    try {
        (void)superzip::verify_suzip(corrupt, options);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    const auto invalid_destination = root / "invalid-output";
    rejected = false;
    try {
        (void)superzip::extract_suzip(corrupt, invalid_destination, options);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(invalid_destination / "data.bin"));
    std::filesystem::remove_all(root);
}

// Purpose: Keep legacy pattern bounds intact while allowing bounded version-four motifs.
// Inputs: Pattern descriptors at the 256-byte, 257-byte, and 16 KiB boundaries.
// Outputs: The index writer and reader reject only version-incompatible or oversized descriptors.
TEST_CASE(suzip_pattern_length_version_boundaries) {
    for (const auto length : {256U, 257U, superzip::kMaxGpuPatternBytes, superzip::kMaxGpuPatternBytes + 1U}) {
        superzip::ArchiveIndex index;
        index.version = superzip::kSuperZipMaxReadableVersion;
        superzip::ArchiveEntry entry;
        entry.path = "pattern.bin";
        entry.uncompressed_size = length * 2U;
        entry.blocks.push_back(superzip::BlockDescriptor{
            .kind = superzip::BlockKind::Pattern, .uncompressed_len = length * 2U, .encoded_len = length});
        index.entries.push_back(entry);
        const bool valid_v4 = length <= superzip::kMaxGpuPatternBytes;
        const bool valid_v3 = length <= superzip::kLegacyGpuPatternBytes;
        for (const auto [version, expected_valid] : {std::pair{3U, valid_v3}, std::pair{4U, valid_v4}}) {
            index.version = version;
            std::ostringstream output(std::ios::out | std::ios::binary);
            bool accepted = true;
            try {
                superzip::write_archive_index(output, index);
            } catch (const superzip::ArchiveError&) {
                accepted = false;
            }
            REQUIRE_EQ(accepted, expected_valid);
            if (expected_valid) {
                std::istringstream input(output.str(), std::ios::in | std::ios::binary);
                REQUIRE_EQ(superzip::read_archive_index(input).entries.front().blocks.front().encoded_len, length);
            }
        }
        if (valid_v4 && !valid_v3) {
            index.version = 4U;
            std::ostringstream output(std::ios::out | std::ios::binary);
            superzip::write_archive_index(output, index);
            auto forged = output.str();
            forged[4] = static_cast<char>(3);
            std::istringstream input(forged, std::ios::in | std::ios::binary);
            bool rejected = false;
            try {
                static_cast<void>(superzip::read_archive_index(input));
            } catch (const superzip::ArchiveError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
        }
    }
}

// Purpose: Validate version-four dictionary decoding and reject corrupt block framing or LZ4 matches.
// Inputs: An independently encoded 32-byte LZ4 block plus four table/payload mutations.
// Outputs: CPU and available HIP readers restore exact bytes; both reject every malformed mutation.
TEST_CASE(suzip_dictionary_reader_validates_segments) {
    constexpr std::array<unsigned char, 19> fixture{0U, 0U, 0U, 0U,    11U, 0U,  0U,  0U,  0x1FU, 'A',
                                                    1U, 0U, 7U, 0x50U, 'A', 'A', 'A', 'A', 'A'};
    const auto fixture_bytes = std::as_bytes(std::span(fixture));
    const std::vector<std::byte> valid(fixture_bytes.begin(), fixture_bytes.end());
    const std::string expected(32U, 'A');
    const auto root = test_temp_dir("suzip-dictionary-reader");
    const auto archive = root / "valid.suzip";
    write_dictionary_test_archive(archive, valid, expected);

    superzip::ExtractOptions cpu;
    cpu.force_cpu = true;
    cpu.gpu_required = false;
    REQUIRE_EQ(superzip::verify_suzip(archive, cpu).entries, 1U);
    const auto destination = root / "decoded";
    static_cast<void>(superzip::extract_suzip(archive, destination, cpu));
    std::ifstream extracted(destination / "data.bin", std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(extracted), {}), expected);
    extracted.close();

    superzip::ExtractOptions required_hip;
    required_hip.gpu_required = true;
    const bool hip_available = superzip::query_gpu_info().available;
    if (hip_available) {
        REQUIRE_TRUE(superzip::verify_suzip(archive, required_hip).gpu_used);
        const auto gpu_destination = root / "gpu-decoded";
        REQUIRE_TRUE(superzip::extract_suzip(archive, gpu_destination, required_hip).gpu_used);
        std::ifstream gpu_file(gpu_destination / "data.bin", std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(gpu_file), {}), expected);
        gpu_file.close();
    } else {
        bool rejected = false;
        try {
            static_cast<void>(superzip::verify_suzip(archive, required_hip));
        } catch (const superzip::GpuError&) {
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
        const auto invalid_archive = root / ("corrupt-" + std::to_string(mutation) + ".suzip");
        write_dictionary_test_archive(invalid_archive, corrupt, expected);
        bool rejected = false;
        try {
            static_cast<void>(superzip::verify_suzip(invalid_archive, cpu));
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        if (hip_available) {
            bool gpu_rejected = false;
            try {
                static_cast<void>(superzip::verify_suzip(invalid_archive, required_hip));
            } catch (const superzip::ArchiveError&) {
                gpu_rejected = true;
            }
            REQUIRE_TRUE(gpu_rejected);
        }
    }
    if (hip_available) {
        REQUIRE_TRUE(superzip::verify_suzip(archive, required_hip).gpu_used);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Verify that dictionary block tables preserve exact output across a full segment and short tail.
// Inputs: Two independent LZ4 blocks for 64 KiB of one byte followed by 32 bytes of another.
// Outputs: The version-four reader verifies and extracts the exact concatenated bytes.
TEST_CASE(suzip_dictionary_reader_crosses_segment_boundary) {
    std::string expected(superzip::kGpuDictionarySegmentBytes, 'A');
    expected.append(32U, 'B');
    const std::array<std::string_view, 2> segments{
        std::string_view(expected).substr(0U, superzip::kGpuDictionarySegmentBytes),
        std::string_view(expected).substr(superzip::kGpuDictionarySegmentBytes)};
    std::ostringstream table(std::ios::out | std::ios::binary);
    superzip::write_u32(table, 0U);
    std::string compressed;
    for (const auto segment : segments) {
        std::string block(static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(segment.size()))), '\0');
        const auto size = LZ4_compress_default(segment.data(), block.data(), static_cast<int>(segment.size()),
                                               static_cast<int>(block.size()));
        REQUIRE_TRUE(size > 0);
        block.resize(static_cast<std::size_t>(size));
        compressed.append(block);
        superzip::write_u32(table, static_cast<std::uint32_t>(compressed.size()));
    }
    const auto encoded = table.str() + compressed;
    const auto payload = std::as_bytes(std::span(encoded.data(), encoded.size()));
    const auto root = test_temp_dir("suzip-dictionary-segments");
    const auto archive = root / "segments.suzip";
    write_dictionary_test_archive(archive, payload, expected);
    superzip::ExtractOptions options;
    options.force_cpu = true;
    options.gpu_required = false;
    REQUIRE_EQ(superzip::verify_suzip(archive, options).entries, 1U);
    const auto destination = root / "decoded";
    static_cast<void>(superzip::extract_suzip(archive, destination, options));
    std::ifstream file(destination / "data.bin", std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(file), {}), expected);
    file.close();
    if (superzip::query_gpu_info().available) {
        superzip::ExtractOptions required_hip;
        required_hip.gpu_required = true;
        REQUIRE_TRUE(superzip::verify_suzip(archive, required_hip).gpu_used);
        const auto gpu_destination = root / "gpu-decoded";
        REQUIRE_TRUE(superzip::extract_suzip(archive, gpu_destination, required_hip).gpu_used);
        std::ifstream gpu_file(gpu_destination / "data.bin", std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(gpu_file), {}), expected);
        gpu_file.close();
    }
    std::filesystem::remove_all(root);
}

// Purpose: Check native CPU/HIP readers against independently encoded LZ4 blocks across bounded data shapes.
// Inputs: Deterministic fill, periodic, repeated-record, and low-alphabet sources at short and full-segment lengths.
// Outputs: Every smaller LZ4 block decodes byte-exactly with matching CRC; at least twelve cases must be admitted.
TEST_CASE(suzip_dictionary_reader_matches_independent_lz4_corpus) {
    const bool hip_available = superzip::query_gpu_info().available;
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
            const std::array blocks{superzip::BlockDescriptor{
                .kind = superzip::BlockKind::GpuDictionary,
                .uncompressed_len = static_cast<std::uint32_t>(size),
                .encoded_len = static_cast<std::uint32_t>(payload.size()),
            }};
            for (const bool hip : {false, true}) {
                if (hip && !hip_available) {
                    continue;
                }
                superzip::GpuCodecOptions options;
                options.force_cpu = !hip;
                options.require_gpu = hip;
                std::vector<std::byte> decoded(size);
                REQUIRE_EQ(superzip::decode_chunk(payload, blocks, decoded, options), hip);
                REQUIRE_EQ(decoded, expected);
                REQUIRE_EQ(superzip::crc_decoded_chunk(payload, blocks, size, options).crc32,
                           superzip::crc32(expected));
            }
        }
    }
    REQUIRE_TRUE(admitted >= 12U);
}

// Purpose: Distinguish direct extraction from archive-wide validation before final publication.
// Inputs: A real two-entry SUZIP whose second payload is corrupted, plus existing first-file output.
// Outputs: Both modes reject the CRC error; staging retains the old first file and cleans all private output after one
// decode.
TEST_CASE(suzip_late_crc_failure_respects_publication_policy) {
    const auto root = test_temp_dir("suzip-publication-late-crc");
    const auto archive = root / "late-crc.suzip";
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
            superzip::extract_with_publication(
                {.destination = output, .overwrite = true, .validate_before_publish = validate},
                [&](const auto& destination, bool overwrite) {
                    ++extractions;
                    superzip::ExtractOptions options;
                    options.gpu_required = false;
                    options.force_cpu = true;
                    options.overwrite = overwrite;
                    static_cast<void>(superzip::extract_suzip(archive, destination, options));
                });
        } catch (const superzip::ArchiveError&) {
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
// Outputs: Requires the smaller Raw/Deflate representation, exact codec bytes, and lossless CPU decoding.
TEST_CASE(suzip_cpu_short_blocks_use_smaller_deflate) {
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
                superzip::GpuCodecOptions options;
                options.force_cpu = true;
                options.require_gpu = false;
                options.compression_level = level;
                auto capacity = mz_compressBound(static_cast<mz_ulong>(input.size()));
                std::vector<std::byte> reference(capacity);
                REQUIRE_EQ(mz_compress2(reinterpret_cast<unsigned char*>(reference.data()), &capacity,
                                        reinterpret_cast<const unsigned char*>(input.data()),
                                        static_cast<mz_ulong>(input.size()), level),
                           MZ_OK);
                reference.resize(capacity);
                const bool smaller = reference.size() < input.size();
                const auto encoded = superzip::encode_chunk(input, options);
                REQUIRE_EQ(encoded.blocks.size(), 1U);
                REQUIRE_EQ(encoded.blocks.front().kind,
                           smaller ? superzip::BlockKind::Deflate : superzip::BlockKind::Raw);
                REQUIRE_EQ(encoded.payload, smaller ? reference : input);
                REQUIRE_TRUE(!encoded.gpu_used);
                std::vector<std::byte> decoded(input.size());
                superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options);
                REQUIRE_EQ(decoded, input);
            }
        }
    }
}

// Purpose: Require a bounded native CPU Zstandard frame for compressible full blocks at every effort.
// Inputs: A deterministic periodic block and product levels one through nine.
// Outputs: Requires version-six block kind, smaller payload, and byte-exact CPU decode.
TEST_CASE(suzip_cpu_zstd_blocks_all_levels_roundtrip) {
    std::vector<std::byte> input(256U * 1024U);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<std::byte>('a' + ((i / 31U) % 17U));
    }
    for (int level = 1; level <= 9; ++level) {
        superzip::GpuCodecOptions options;
        options.force_cpu = true;
        options.require_gpu = false;
        options.compression_level = level;
        options.block_size = static_cast<std::uint32_t>(input.size());
        const auto encoded = superzip::encode_chunk(input, options);
        REQUIRE_EQ(encoded.blocks.size(), 1U);
        REQUIRE_EQ(encoded.blocks[0].kind, superzip::BlockKind::CpuZstd);
        REQUIRE_TRUE(encoded.payload.size() < input.size());
        std::vector<std::byte> decoded(input.size());
        REQUIRE_TRUE(!superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
        REQUIRE_EQ(decoded, input);
    }
}

// Purpose: Preserve dense payload assembly when a CPU chunk mixes all native CPU block representations.
// Inputs: One Zstandard, random raw, fill, and short Deflate block with single/multiple worker budgets.
// Outputs: Requires stable kinds, contiguous encoded offsets, and byte-exact decode.
TEST_CASE(suzip_cpu_zstd_mixed_block_layout) {
    constexpr std::size_t block_size = superzip::kMinArchiveBlockBytes;
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
        superzip::GpuCodecOptions options;
        options.force_cpu = true;
        options.require_gpu = false;
        options.block_size = static_cast<std::uint32_t>(block_size);
        options.worker_count = workers;
        const auto encoded = superzip::encode_chunk(input, options);
        REQUIRE_EQ(encoded.blocks.size(), 4U);
        REQUIRE_EQ(encoded.blocks[0].kind, superzip::BlockKind::CpuZstd);
        REQUIRE_EQ(encoded.blocks[1].kind, superzip::BlockKind::Raw);
        REQUIRE_EQ(encoded.blocks[2].kind, superzip::BlockKind::Fill);
        REQUIRE_EQ(encoded.blocks[3].kind, superzip::BlockKind::Deflate);
        REQUIRE_EQ(encoded.blocks[0].encoded_offset, 0U);
        REQUIRE_EQ(encoded.blocks[1].encoded_offset, encoded.blocks[0].encoded_len);
        REQUIRE_EQ(encoded.blocks[3].encoded_offset, encoded.blocks[1].encoded_offset + encoded.blocks[1].encoded_len);
        std::vector<std::byte> decoded(input.size());
        REQUIRE_TRUE(!superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
        REQUIRE_EQ(decoded, input);
    }
}

// Purpose: Reject invalid native CPU frame boundaries and protect version-six publication.
// Inputs: One valid frame, truncated and concatenated frames, trailing bytes, and a lying decoded-size declaration.
// Outputs: Valid CPU verification/extraction succeeds; invalid archives fail before output publication.
TEST_CASE(suzip_cpu_zstd_archive_frame_validation) {
    const auto root = test_temp_dir("suzip-cpu-zstd-frame");
    std::string decoded(64U * 1024U, 'Z');
    for (std::size_t i = 0; i < decoded.size(); i += 53U) {
        decoded[i] = 'Q';
    }
    const auto source = std::as_bytes(std::span(decoded.data(), decoded.size()));
    superzip::GpuCodecOptions codec;
    codec.force_cpu = true;
    codec.require_gpu = false;
    codec.block_size = static_cast<std::uint32_t>(decoded.size());
    const auto encoded = superzip::encode_chunk(source, codec);
    REQUIRE_EQ(encoded.blocks.size(), 1U);
    REQUIRE_EQ(encoded.blocks[0].kind, superzip::BlockKind::CpuZstd);

    superzip::ExtractOptions options;
    options.force_cpu = true;
    options.gpu_required = false;
    const auto valid = root / "valid.suzip";
    write_encoded_test_archive(valid, encoded.payload, decoded, superzip::BlockKind::CpuZstd, 6U);
    REQUIRE_EQ(read_test_archive_index(valid).version, 6U);
    REQUIRE_EQ(superzip::verify_suzip(valid, options).entries, 1U);
    const auto destination = root / "valid-output";
    REQUIRE_EQ(superzip::extract_suzip(valid, destination, options).entries, 1U);
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
        const auto archive = root / (std::string(name) + ".suzip");
        write_encoded_test_archive(archive, bytes, decoded, superzip::BlockKind::CpuZstd, 6U);
        bool rejected = false;
        try {
            (void)superzip::verify_suzip(archive, options);
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        const auto invalid_destination = root / (std::string(name) + "-output");
        rejected = false;
        try {
            (void)superzip::extract_suzip(archive, invalid_destination, options);
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_TRUE(!std::filesystem::exists(invalid_destination / "data.bin"));
    }
    const auto wrong_size = root / "wrong-size.suzip";
    write_encoded_test_archive(wrong_size, encoded.payload, decoded + "Q", superzip::BlockKind::CpuZstd, 6U);
    bool rejected = false;
    try {
        (void)superzip::verify_suzip(wrong_size, options);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Preserve public reading of short legacy Deflate blocks after adding native Zstandard blocks.
// Inputs: A version-three archive containing a product-generated short Deflate payload.
// Outputs: Requires exact verification and extraction through the unchanged legacy CPU decoder.
TEST_CASE(suzip_legacy_deflate_archive_remains_readable) {
    const auto root = test_temp_dir("suzip-legacy-deflate");
    std::string decoded(127U, 'a');
    for (std::size_t i = 0; i < decoded.size(); i += 3U) {
        decoded[i] = 'b';
    }
    superzip::GpuCodecOptions codec;
    codec.force_cpu = true;
    codec.require_gpu = false;
    const auto encoded = superzip::encode_chunk(std::as_bytes(std::span(decoded.data(), decoded.size())), codec);
    REQUIRE_EQ(encoded.blocks.size(), 1U);
    REQUIRE_EQ(encoded.blocks[0].kind, superzip::BlockKind::Deflate);
    const auto archive = root / "legacy.suzip";
    write_encoded_test_archive(archive, encoded.payload, decoded, superzip::BlockKind::Deflate, 3U);
    superzip::ExtractOptions options;
    options.force_cpu = true;
    options.gpu_required = false;
    REQUIRE_EQ(superzip::verify_suzip(archive, options).entries, 1U);
    const auto destination = root / "output";
    REQUIRE_EQ(superzip::extract_suzip(archive, destination, options).entries, 1U);
    std::ifstream restored(destination / "data.bin", std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(restored), {}), decoded);
    restored.close();
    std::filesystem::remove_all(root);
}

// Purpose: Retain compression of a short final block after full-sized raw data in parallel CPU work.
// Inputs: One deterministic random block followed by a 127-byte repeated sequence, three efforts and worker budgets.
// Outputs: Requires raw then Deflate descriptors, contiguous payload, and byte-exact decoding.
TEST_CASE(suzip_cpu_short_tail_after_raw_block) {
    std::vector<std::byte> input(superzip::kMinArchiveBlockBytes + 127U);
    std::uint32_t state = 0x43A81D27U;
    for (std::size_t i = 0; i < superzip::kMinArchiveBlockBytes; ++i) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        input[i] = static_cast<std::byte>(state & 255U);
    }
    for (std::size_t i = superzip::kMinArchiveBlockBytes; i < input.size(); ++i) {
        input[i] = static_cast<std::byte>(i % 3U);
    }
    for (const int level : {1, 5, 9}) {
        for (const auto workers : {1U, 2U, 4U}) {
            superzip::GpuCodecOptions options;
            options.force_cpu = true;
            options.require_gpu = false;
            options.compression_level = level;
            options.block_size = superzip::kMinArchiveBlockBytes;
            options.worker_count = workers;
            const auto encoded = superzip::encode_chunk(input, options);
            REQUIRE_EQ(encoded.blocks.size(), 2U);
            REQUIRE_EQ(encoded.blocks[0].kind, superzip::BlockKind::Raw);
            REQUIRE_EQ(encoded.blocks[1].kind, superzip::BlockKind::Deflate);
            REQUIRE_EQ(encoded.blocks[1].encoded_offset, superzip::kMinArchiveBlockBytes);
            REQUIRE_TRUE(encoded.payload.size() < input.size());
            std::vector<std::byte> decoded(input.size());
            superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options);
            REQUIRE_EQ(decoded, input);
        }
    }
}

// Purpose: Restore native UTF-8 filenames without depending on the host's ANSI code page.
// Inputs: Unicode source, archive, destination and entry paths in CPU and available required-HIP modes.
// Outputs: Requires verified archive roundtrips, identical filenames/payloads, and honest CPU/HIP telemetry.
TEST_CASE(suzip_unicode_paths_roundtrip) {
    const auto root = test_temp_dir("suzip-unicode");
    const auto source = root / std::filesystem::path(u8"\u65e5\u672c");
    const auto relative = std::filesystem::path(u8"caf\u00e9/\U0001f4c1.txt");
    std::filesystem::create_directories((source / relative).parent_path());
    const std::string payload(8192U, 'U');
    std::ofstream(source / relative, std::ios::binary) << payload;
    const bool hip_available = superzip::query_gpu_info().available;
    for (const bool hip : {false, true}) {
        if (hip && !hip_available) {
            continue;
        }
        superzip::CompressOptions compression;
        compression.force_cpu = !hip;
        compression.gpu_required = hip;
        compression.verify_after_write = true;
        const auto archive = root / std::filesystem::path(u8"\u03a9.suzip");
        const auto compressed = superzip::compress_suzip({source}, archive, compression);
        REQUIRE_EQ(compressed.gpu_used, hip);
        superzip::ExtractOptions extraction;
        extraction.force_cpu = !hip;
        extraction.gpu_required = hip;
        const auto destination = root / (hip ? "hip" : "cpu") / std::filesystem::path(u8"\u00e9");
        const auto extracted = superzip::extract_suzip(archive, destination, extraction);
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
        256U * 1024U,
        512U * 1024U,
        1024U * 1024U,
        2U * 1024U * 1024U,
        4U * 1024U * 1024U,
        8U * 1024U * 1024U,
        superzip::kMaxArchiveBlockBytes,
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

// Purpose: Keep the new default block size observable in archive metadata without changing saved legacy choices.
// Inputs: A one-byte tail after a full default-sized block, with explicit CPU mode and default block size.
// Outputs: The archive contains 8 MiB and one-byte blocks and extracts the original bytes exactly.
TEST_CASE(suzip_default_block_size_roundtrip) {
    REQUIRE_EQ(superzip::kDefaultArchiveBlockBytes, 8U * 1024U * 1024U);
    const auto root = test_temp_dir("suzip-default-block-size");
    const auto source = root / "payload.bin";
    const auto archive = root / "payload.suzip";
    const auto output = root / "out";
    const std::string expected(static_cast<std::size_t>(superzip::kDefaultArchiveBlockBytes) + 1U, 'x');
    {
        std::ofstream file(source, std::ios::binary);
        file.write(expected.data(), static_cast<std::streamsize>(expected.size()));
    }
    superzip::CompressOptions options;
    options.force_cpu = true;
    options.gpu_required = false;
    REQUIRE_EQ(options.block_size, superzip::kDefaultArchiveBlockBytes);
    (void)superzip::compress_suzip({source}, archive, options);
    const auto index = read_test_archive_index(archive);
    REQUIRE_EQ(index.entries.size(), 1U);
    REQUIRE_EQ(index.entries.front().blocks.size(), 2U);
    REQUIRE_EQ(index.entries.front().blocks[0].uncompressed_len, superzip::kDefaultArchiveBlockBytes);
    REQUIRE_EQ(index.entries.front().blocks[1].uncompressed_len, 1U);
    superzip::ExtractOptions extract;
    extract.force_cpu = true;
    extract.gpu_required = false;
    (void)superzip::extract_suzip(archive, output, extract);
    {
        std::ifstream restored(output / "payload.bin", std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(restored), {}), expected);
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

// Purpose: Exercise production HIP dictionary selection across efforts and both native readers.
// Inputs: Four seeded 16 KiB records with changing first bytes, plus one required-HIP archive at level nine.
// Outputs: All efforts produce smaller distinct payloads; the emitted version-four archive roundtrips on CPU/HIP.
TEST_CASE(suzip_gpu_dictionary_writer_levels_and_roundtrip) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> input(65536U);
    std::uint32_t state = 0x31674325U;
    for (std::size_t index = 0; index < input.size(); ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        input[index] = index < 16384U ? static_cast<std::byte>(state >> 24U) : input[index % 16384U];
    }
    for (std::size_t record = 1U; record < 4U; ++record) {
        input[record * 16384U] = static_cast<std::byte>(record);
    }

    std::size_t previous = input.size();
    for (int level = 1; level <= 9; ++level) {
        superzip::GpuCodecOptions options;
        options.require_gpu = true;
        options.compression_level = level;
        options.telemetry = std::make_shared<superzip::GpuTelemetry>();
        const auto encoded = superzip::encode_chunk(input, options);
        REQUIRE_TRUE(encoded.gpu_used);
        REQUIRE_EQ(encoded.blocks.size(), 1U);
        REQUIRE_EQ(encoded.blocks.front().kind, superzip::BlockKind::GpuDictionary);
        REQUIRE_TRUE(encoded.payload.size() < previous);
        const auto telemetry = superzip::snapshot_gpu_telemetry(*options.telemetry);
        REQUIRE_TRUE(telemetry.kernel_launches >= 5U);
        REQUIRE_TRUE(telemetry.h2d_bytes >= input.size());
        REQUIRE_EQ(telemetry.dictionary_blocks, 1U);
        REQUIRE_EQ(telemetry.prefix_blocks, 0U);
        std::vector<std::byte> decoded(input.size());
        superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, {.require_gpu = false, .force_cpu = true});
        REQUIRE_EQ(decoded, input);
        previous = encoded.payload.size();
    }

    std::vector<std::byte> mixed = input;
    for (std::size_t index = 0; index < input.size(); ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        mixed.push_back(static_cast<std::byte>(state >> 24U));
    }
    mixed.insert(mixed.end(), input.size(), std::byte{0xA7});
    superzip::GpuCodecOptions mixed_options;
    mixed_options.block_size = static_cast<std::uint32_t>(input.size());
    mixed_options.compression_level = 9;
    const auto mixed_encoded = superzip::encode_chunk(mixed, mixed_options);
    REQUIRE_EQ(mixed_encoded.blocks.size(), 3U);
    REQUIRE_EQ(mixed_encoded.blocks[0].kind, superzip::BlockKind::GpuDictionary);
    REQUIRE_EQ(mixed_encoded.blocks[1].kind, superzip::BlockKind::Raw);
    REQUIRE_EQ(mixed_encoded.blocks[2].kind, superzip::BlockKind::Fill);
    REQUIRE_EQ(mixed_encoded.blocks[1].encoded_offset, mixed_encoded.blocks[0].encoded_len);
    REQUIRE_EQ(mixed_encoded.payload.size(), mixed_encoded.blocks[0].encoded_len + mixed_encoded.blocks[1].encoded_len);
    for (const bool hip : {false, true}) {
        auto decode_options = mixed_options;
        decode_options.force_cpu = !hip;
        decode_options.require_gpu = hip;
        std::vector<std::byte> decoded(mixed.size());
        REQUIRE_EQ(superzip::decode_chunk(mixed_encoded.payload, mixed_encoded.blocks, decoded, decode_options), hip);
        REQUIRE_EQ(decoded, mixed);
        const auto crc =
            superzip::crc_decoded_chunk(mixed_encoded.payload, mixed_encoded.blocks, mixed.size(), decode_options);
        REQUIRE_EQ(crc.crc32, superzip::crc32(mixed));
    }

    const auto root = test_temp_dir("suzip-dictionary-writer");
    const auto source = root / "record.bin";
    {
        std::ofstream file(source, std::ios::binary);
        file.write(reinterpret_cast<const char*>(input.data()), static_cast<std::streamsize>(input.size()));
    }
    const auto archive = root / "record.suzip";
    superzip::CompressOptions compression;
    compression.compression_level = 9;
    compression.block_size = static_cast<std::uint32_t>(input.size());
    compression.verify_after_write = true;
    const auto compressed = superzip::compress_suzip({source}, archive, compression);
    REQUIRE_TRUE(compressed.gpu_used);
    const auto index = read_test_archive_index(archive);
    REQUIRE_EQ(index.version, 4U);
    REQUIRE_TRUE(archive_contains_block_kind(index, superzip::BlockKind::GpuDictionary));
    for (const bool hip : {false, true}) {
        superzip::ExtractOptions options;
        options.force_cpu = !hip;
        options.gpu_required = hip;
        REQUIRE_EQ(superzip::verify_suzip(archive, options).gpu_used, hip);
        const auto destination = root / (hip ? "hip" : "cpu");
        REQUIRE_EQ(superzip::extract_suzip(archive, destination, options).gpu_used, hip);
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
TEST_CASE(suzip_gpu_long_pattern_version_four_roundtrip) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> archive_input;
    for (const auto period : {257U, 4096U, 8192U, superzip::kMaxGpuPatternBytes}) {
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
        superzip::GpuCodecOptions options;
        options.require_gpu = true;
        options.compression_level = 5;
        const auto encoded = superzip::encode_chunk(input, options);
        REQUIRE_TRUE(encoded.gpu_used);
        REQUIRE_EQ(encoded.blocks.size(), 1U);
        REQUIRE_EQ(encoded.blocks.front().kind, superzip::BlockKind::Pattern);
        REQUIRE_EQ(encoded.blocks.front().encoded_len, period);
        REQUIRE_EQ(encoded.payload.size(), period);
        for (const bool hip : {false, true}) {
            auto decode_options = options;
            decode_options.force_cpu = !hip;
            decode_options.require_gpu = hip;
            std::vector<std::byte> decoded(input.size());
            REQUIRE_EQ(superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, decode_options), hip);
            REQUIRE_EQ(decoded, input);
        }
        if (period == superzip::kMaxGpuPatternBytes) {
            archive_input = input;
            input.back() ^= std::byte{0x01};
            const auto near_pattern = superzip::encode_chunk(input, options);
            REQUIRE_TRUE(near_pattern.gpu_used);
            REQUIRE_EQ(near_pattern.blocks.size(), 1U);
            REQUIRE_TRUE(near_pattern.blocks.front().kind != superzip::BlockKind::Pattern);
            std::vector<std::byte> decoded(input.size());
            superzip::decode_chunk(near_pattern.payload, near_pattern.blocks, decoded,
                                   {.require_gpu = false, .force_cpu = true});
            REQUIRE_EQ(decoded, input);
        }
    }

    const auto root = test_temp_dir("suzip-long-pattern-v4");
    const auto source = root / "pattern.bin";
    {
        std::ofstream file(source, std::ios::binary);
        file.write(reinterpret_cast<const char*>(archive_input.data()),
                   static_cast<std::streamsize>(archive_input.size()));
    }
    const auto archive = root / "pattern.suzip";
    superzip::CompressOptions compression;
    compression.gpu_required = true;
    compression.verify_after_write = true;
    REQUIRE_TRUE(superzip::compress_suzip({source}, archive, compression).gpu_used);
    const auto index = read_test_archive_index(archive);
    REQUIRE_EQ(index.version, 4U);
    REQUIRE_EQ(index.entries.front().blocks.front().kind, superzip::BlockKind::Pattern);
    REQUIRE_EQ(index.entries.front().blocks.front().encoded_len, superzip::kMaxGpuPatternBytes);
    for (const bool hip : {false, true}) {
        superzip::ExtractOptions options;
        options.force_cpu = !hip;
        options.gpu_required = hip;
        REQUIRE_EQ(superzip::verify_suzip(archive, options).gpu_used, hip);
        const auto destination = root / (hip ? "hip" : "cpu");
        REQUIRE_EQ(superzip::extract_suzip(archive, destination, options).gpu_used, hip);
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
TEST_CASE(suzip_cpu_pattern_expansion_boundaries) {
    for (const auto period : {2U, 256U, 257U, superzip::kMaxGpuPatternBytes}) {
        std::vector<std::byte> motif(period);
        for (std::size_t index = 0; index < motif.size(); ++index) {
            motif[index] = static_cast<std::byte>((index * 131U + 17U) & 0xFFU);
        }
        for (const auto length : {period + 1U, period * 2U - 1U, period * 3U + 13U}) {
            const std::array blocks{superzip::BlockDescriptor{
                .kind = superzip::BlockKind::Pattern,
                .uncompressed_len = length,
                .encoded_len = period,
            }};
            std::vector<std::byte> decoded(length);
            REQUIRE_TRUE(!superzip::decode_chunk(motif, blocks, decoded, {.require_gpu = false, .force_cpu = true}));
            for (std::size_t index = 0; index < decoded.size(); ++index) {
                REQUIRE_EQ(decoded[index], motif[index % period]);
            }
        }
    }
}

// Purpose: Measure public CPU decoding of bounded version-four pattern blocks without filesystem writes.
// Inputs: An explicit environment opt-in and eight deterministic 16 MiB pattern blocks.
// Outputs: Prints a median decode time only after byte-exact validation; never asserts a timing threshold.
TEST_CASE(suzip_cpu_pattern_decode_benchmark_opt_in) {
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"SUPERZIP_PATTERN_CPU_BENCHMARK", enabled, 2U) != 1U || enabled[0] != L'1') {
        return;
    }
    constexpr std::size_t block_bytes = superzip::kMaxArchiveBlockBytes;
    constexpr std::size_t block_count = 8U;
    constexpr std::size_t pattern_bytes = superzip::kMaxGpuPatternBytes;
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
    std::vector<superzip::BlockDescriptor> blocks;
    for (std::size_t block = 0; block < block_count; ++block) {
        blocks.push_back(superzip::BlockDescriptor{
            .kind = superzip::BlockKind::Pattern,
            .uncompressed_len = static_cast<std::uint32_t>(block_bytes),
            .encoded_offset = block * pattern_bytes,
            .encoded_len = static_cast<std::uint32_t>(pattern_bytes),
        });
    }
    std::vector<std::byte> decoded(block_count * block_bytes);
    const superzip::GpuCodecOptions options{.require_gpu = false, .force_cpu = true};
    std::vector<double> milliseconds;
    for (int iteration = 0; iteration < 5; ++iteration) {
        const auto started = std::chrono::steady_clock::now();
        REQUIRE_TRUE(!superzip::decode_chunk(payload, blocks, decoded, options));
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

// Purpose: Verify required-HIP verification refuses archives that need CPU Zstandard.
// Inputs: A force-CPU text archive that intentionally contains Zstandard blocks.
// Outputs: Throws if `gpu_required` silently invokes the CPU Zstandard codec.
TEST_CASE(suzip_required_gpu_rejects_cpu_zstd_archive) {
    if (!superzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("suzip-required-gpu-rejects-zstd");
    const auto input = root / "cpu-zstd.txt";
    {
        std::ofstream out(input, std::ios::binary);
        for (int i = 0; i < 32768; ++i) {
            out << "CPU Zstandard archive block for required HIP rejection.\n";
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
    REQUIRE_EQ(index.version, 6U);
    REQUIRE_TRUE(archive_contains_block_kind(index, superzip::BlockKind::CpuZstd));

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
    REQUIRE_TRUE(!std::filesystem::exists(output / "cpu-zstd.txt"));
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

// Purpose: Verify SUZIP metadata validation enforces the archive-wide decoded-output policy before decode.
// Inputs: A compact fill-only index declaring one byte more than the 64 GiB aggregate output limit.
// Outputs: Throws `ArchiveError` without allocating or decoding the declared payload.
TEST_CASE(suzip_verify_rejects_aggregate_output_over_resource_limit) {
    const auto root = test_temp_dir("suzip-aggregate-output-limit");
    const auto archive = root / "oversized.suzip";
    {
        std::ofstream file(archive, std::ios::binary | std::ios::trunc);
        superzip::ArchiveIndex index;
        superzip::ArchiveEntry entry;
        entry.path = "oversized.bin";
        entry.uncompressed_size = superzip::kMaxExtractedOutputBytes + 1U;
        entry.payload_offset = 0;
        entry.payload_size = 0;
        entry.crc32 = 0;
        const auto full_blocks = superzip::kMaxExtractedOutputBytes / superzip::kMaxArchiveBlockBytes;
        entry.blocks.reserve(static_cast<std::size_t>(full_blocks + 1U));
        for (std::uint64_t i = 0; i < full_blocks; ++i) {
            entry.blocks.push_back(superzip::BlockDescriptor{
                .kind = superzip::BlockKind::Fill,
                .fill_value = 0,
                .uncompressed_len = superzip::kMaxArchiveBlockBytes,
                .encoded_offset = 0,
                .encoded_len = 0,
            });
        }
        entry.blocks.push_back(superzip::BlockDescriptor{
            .kind = superzip::BlockKind::Fill,
            .fill_value = 0,
            .uncompressed_len = 1,
            .encoded_offset = 0,
            .encoded_len = 0,
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
        static_cast<void>(superzip::verify_suzip(archive, verify));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
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
        const std::array<char, 2> pattern{'x', 'y'};
        file.write(pattern.data(), static_cast<std::streamsize>(pattern.size()));

        superzip::ArchiveIndex index;
        superzip::ArchiveEntry entry;
        entry.path = "pattern.bin";
        entry.uncompressed_size = 4096;
        entry.payload_offset = 0;
        entry.payload_size = pattern.size();
        entry.blocks.push_back(superzip::BlockDescriptor{
            .kind = superzip::BlockKind::Pattern,
            .fill_value = 0,
            .uncompressed_len = 4096,
            .encoded_offset = 0,
            .encoded_len = static_cast<std::uint32_t>(pattern.size()),
        });
        index.entries.push_back(std::move(entry));
        const auto index_offset = static_cast<std::uint64_t>(file.tellp());
        superzip::write_archive_index(file, index);
        const auto index_size = static_cast<std::uint64_t>(file.tellp()) - index_offset;
        file.seekp(-static_cast<std::streamoff>(sizeof(std::uint32_t)), std::ios::cur);
        superzip::write_u32(file, 1U);
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
