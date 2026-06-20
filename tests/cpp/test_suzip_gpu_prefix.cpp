#include "core/archive.hpp"
#include "core/result.hpp"
#include "core/resource_limits.hpp"
#include "gpu/gpu_codec.hpp"
#include "test_suzip_helpers.hpp"
#include "test_util.hpp"

#include <filesystem>
#include <fstream>

namespace {

using namespace superzip_test;

struct PrefixBlockLocation {
    std::uint64_t payload_offset;
    superzip::BlockDescriptor block;
};

// Purpose: Write deterministic low-entropy bytes that are not fill or periodic data.
// Inputs: `path` is the destination file and `byte_count` is the exact payload size.
// Outputs: Creates or replaces `path` with a biased byte distribution used by GPU-prefix tests.
void write_low_entropy_payload(const std::filesystem::path& path, std::size_t byte_count) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE_TRUE(out.is_open());
    std::uint32_t state = 0xC001D00DU;
    for (std::size_t i = 0; i < byte_count; ++i) {
        state = (state * 1664525U) + 1013904223U;
        const auto bucket = (state >> 16U) & 1023U;
        unsigned char value = 0;
        if (bucket < 180U) {
            value = 1;
        } else if (bucket < 330U) {
            value = 0;
        } else if (bucket < 450U) {
            value = 2;
        } else if (bucket < 520U) {
            value = 3;
        } else if (bucket < 720U) {
            value = static_cast<unsigned char>(4U + (bucket % 16U));
        } else if (bucket < 900U) {
            value = static_cast<unsigned char>(20U + (bucket % 64U));
        } else {
            value = static_cast<unsigned char>(84U + (bucket % 172U));
        }
        out.put(static_cast<char>(value));
    }
}

// Purpose: Write one fill block followed by one low-entropy raw block in the same chunk.
// Inputs: `path` is the destination file and `block_bytes` is the byte count for each half.
// Outputs: Creates a two-block file that must preserve fill encoding while prefix-compressing the raw half.
void write_fill_then_low_entropy_payload(const std::filesystem::path& path, std::size_t block_bytes) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE_TRUE(out.is_open());
    for (std::size_t i = 0; i < block_bytes; ++i) {
        out.put('\0');
    }
    std::uint32_t state = 0xB10C5A11U;
    for (std::size_t i = 0; i < block_bytes; ++i) {
        state = (state * 1664525U) + 1013904223U;
        const auto bucket = (state >> 16U) & 1023U;
        unsigned char value = 0;
        if (bucket < 180U) {
            value = 1;
        } else if (bucket < 330U) {
            value = 0;
        } else if (bucket < 450U) {
            value = 2;
        } else if (bucket < 520U) {
            value = 3;
        } else if (bucket < 720U) {
            value = static_cast<unsigned char>(4U + (bucket % 16U));
        } else if (bucket < 900U) {
            value = static_cast<unsigned char>(20U + (bucket % 64U));
        } else {
            value = static_cast<unsigned char>(84U + (bucket % 172U));
        }
        out.put(static_cast<char>(value));
    }
}

// Purpose: Write deterministic high-byte low-entropy data that requires adaptive GPU prefix coding to compress well.
// Inputs: `path` is the destination file and `byte_count` is the exact payload size.
// Outputs: Creates or replaces `path` with a biased distribution whose frequent bytes are not low numeric values.
void write_shifted_low_entropy_payload(const std::filesystem::path& path, std::size_t byte_count) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE_TRUE(out.is_open());
    std::uint32_t state = 0xA11CE5EEDU;
    for (std::size_t i = 0; i < byte_count; ++i) {
        state = (state * 1103515245U) + 12345U;
        const auto bucket = (state >> 16U) & 1023U;
        unsigned char value = 0;
        if (bucket < 220U) {
            value = 201U;
        } else if (bucket < 410U) {
            value = 233U;
        } else if (bucket < 560U) {
            value = 144U;
        } else if (bucket < 680U) {
            value = 177U;
        } else if (bucket < 860U) {
            value = static_cast<unsigned char>(96U + (bucket % 32U));
        } else {
            value = static_cast<unsigned char>(bucket % 256U);
        }
        out.put(static_cast<char>(value));
    }
}

// Purpose: Return the serialized offset-table byte count for one GPU-prefix test block.
// Inputs: `decoded_len` is the block's uncompressed byte count.
// Outputs: Returns the number of bytes before the encoded bitstream starts.
std::uint64_t test_gpu_prefix_table_bytes(std::uint32_t decoded_len) {
    const auto segments = (static_cast<std::uint64_t>(decoded_len) + superzip::kGpuPrefixSegmentBytes - 1U) /
                          superzip::kGpuPrefixSegmentBytes;
    return (segments + 1U) * sizeof(std::uint32_t);
}

// Purpose: Locate the first GPU-prefix block inside a test archive index.
// Inputs: `index` is a parsed SUZIP test archive index.
// Outputs: Returns the entry payload offset and block descriptor; fails the test when no prefix block exists.
PrefixBlockLocation find_first_prefix_block(const superzip::ArchiveIndex& index) {
    for (const auto& entry : index.entries) {
        for (const auto& block : entry.blocks) {
            if (block.kind == superzip::BlockKind::GpuPrefix) {
                return PrefixBlockLocation{.payload_offset = entry.payload_offset, .block = block};
            }
        }
    }
    REQUIRE_TRUE(false);
    return PrefixBlockLocation{};
}

}  // namespace

// Purpose: Verify required-HIP compression can emit a real GPU prefix-compressed native block.
// Inputs: A low-byte payload that is not fill or periodic and is compressed with `gpu_required`.
// Outputs: Throws if the archive stays raw-sized, uses CPU deflate, omits prefix blocks, or fails required-HIP
// verification/extraction.
TEST_CASE(suzip_required_gpu_prefix_blocks_compress_low_entropy_payload) {
    if (!superzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("suzip-required-gpu-prefix");
    const auto input = root / "low-entropy.bin";
    write_low_entropy_payload(input, 2U * 1024U * 1024U);
    const auto source_size = std::filesystem::file_size(input);
    const auto archive = root / "archive.suzip";

    superzip::CompressOptions compress;
    compress.gpu_required = true;
    compress.force_cpu = false;
    compress.chunk_size = 2U * 1024U * 1024U;
    compress.block_size = 1024U * 1024U;
    compress.verify_after_write = true;
    const auto compressed = superzip::compress_suzip({input}, archive, compress);
    REQUIRE_TRUE(compressed.gpu_used);
    REQUIRE_TRUE(compressed.gpu_runtime.prefix_blocks > 0U);
    REQUIRE_TRUE(compressed.output_bytes < (source_size * 3U) / 4U);

    const auto index = read_test_archive_index(archive);
    REQUIRE_TRUE(archive_contains_block_kind(index, superzip::BlockKind::GpuPrefix));
    REQUIRE_TRUE(!archive_contains_block_kind(index, superzip::BlockKind::Deflate));

    superzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 2U * 1024U * 1024U;
    verify.block_size = 1024U * 1024U;
    const auto verified = superzip::verify_suzip(archive, verify);
    REQUIRE_TRUE(verified.gpu_used);

    const auto output = root / "out";
    verify.overwrite = true;
    const auto extracted = superzip::extract_suzip(archive, output, verify);
    REQUIRE_TRUE(extracted.gpu_used);
    REQUIRE_EQ(std::filesystem::file_size(output / "low-entropy.bin"), source_size);
    REQUIRE_TRUE(files_are_equal(output / "low-entropy.bin", input));
    std::filesystem::remove_all(root);
}

// Purpose: Verify raw blocks inside a mixed fill/raw chunk still receive GPU prefix compression.
// Inputs: A two-block payload where the first block is fill-compressed and the second is low-entropy raw data.
// Outputs: Throws if the raw block stays uncompressed only because the same chunk also contains a fill block.
TEST_CASE(suzip_required_gpu_prefix_blocks_compress_raw_blocks_inside_mixed_chunk) {
    if (!superzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("suzip-required-gpu-mixed-prefix");
    const auto input = root / "mixed-fill-low-entropy.bin";
    write_fill_then_low_entropy_payload(input, 1024U * 1024U);
    const auto source_size = std::filesystem::file_size(input);
    const auto archive = root / "archive.suzip";

    superzip::CompressOptions compress;
    compress.gpu_required = true;
    compress.force_cpu = false;
    compress.chunk_size = 2U * 1024U * 1024U;
    compress.block_size = 1024U * 1024U;
    compress.verify_after_write = true;
    const auto compressed = superzip::compress_suzip({input}, archive, compress);
    REQUIRE_TRUE(compressed.gpu_used);
    REQUIRE_TRUE(compressed.gpu_runtime.prefix_blocks > 0U);
    REQUIRE_TRUE(compressed.output_bytes < source_size);

    const auto index = read_test_archive_index(archive);
    REQUIRE_TRUE(archive_contains_block_kind(index, superzip::BlockKind::Fill));
    REQUIRE_TRUE(archive_contains_block_kind(index, superzip::BlockKind::GpuPrefix));
    REQUIRE_TRUE(!archive_contains_block_kind(index, superzip::BlockKind::Deflate));

    superzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 2U * 1024U * 1024U;
    verify.block_size = 1024U * 1024U;
    const auto verified = superzip::verify_suzip(archive, verify);
    REQUIRE_TRUE(verified.gpu_used);

    const auto output = root / "out";
    verify.overwrite = true;
    const auto extracted = superzip::extract_suzip(archive, output, verify);
    REQUIRE_TRUE(extracted.gpu_used);
    REQUIRE_EQ(std::filesystem::file_size(output / "mixed-fill-low-entropy.bin"), source_size);
    REQUIRE_TRUE(files_are_equal(output / "mixed-fill-low-entropy.bin", input));
    std::filesystem::remove_all(root);
}

// Purpose: Verify required-HIP higher compression levels can emit adaptive GPU-prefix blocks without CPU deflate.
// Inputs: A high-byte low-entropy payload where static low-value prefix coding is intentionally weak.
// Outputs: Throws if level 9 fails to beat level 1, omits adaptive blocks, emits deflate, or fails HIP roundtrip.
TEST_CASE(suzip_required_gpu_adaptive_prefix_blocks_honor_compression_level) {
    if (!superzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("suzip-required-gpu-adaptive-prefix");
    const auto input = root / "shifted-low-entropy.bin";
    write_shifted_low_entropy_payload(input, 2U * 1024U * 1024U);
    const auto source_size = std::filesystem::file_size(input);
    const auto fast_archive = root / "fast.suzip";
    const auto strong_archive = root / "strong.suzip";

    superzip::CompressOptions fast;
    fast.gpu_required = true;
    fast.force_cpu = false;
    fast.chunk_size = 2U * 1024U * 1024U;
    fast.block_size = 1024U * 1024U;
    fast.compression_level = 1;
    fast.verify_after_write = true;
    const auto fast_stats = superzip::compress_suzip({input}, fast_archive, fast);
    REQUIRE_TRUE(fast_stats.gpu_used);

    auto strong = fast;
    strong.compression_level = 9;
    const auto strong_stats = superzip::compress_suzip({input}, strong_archive, strong);
    REQUIRE_TRUE(strong_stats.gpu_used);
    REQUIRE_TRUE(strong_stats.gpu_runtime.prefix_blocks > 0U);
    REQUIRE_TRUE(strong_stats.output_bytes < fast_stats.output_bytes);
    REQUIRE_TRUE(strong_stats.output_bytes < (source_size * 3U) / 4U);

    const auto index = read_test_archive_index(strong_archive);
    REQUIRE_TRUE(archive_contains_block_kind(index, superzip::BlockKind::GpuAdaptivePrefix));
    REQUIRE_TRUE(!archive_contains_block_kind(index, superzip::BlockKind::Deflate));

    superzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 2U * 1024U * 1024U;
    verify.block_size = 1024U * 1024U;
    const auto verified = superzip::verify_suzip(strong_archive, verify);
    REQUIRE_TRUE(verified.gpu_used);

    const auto output = root / "out";
    verify.overwrite = true;
    const auto extracted = superzip::extract_suzip(strong_archive, output, verify);
    REQUIRE_TRUE(extracted.gpu_used);
    REQUIRE_EQ(std::filesystem::file_size(output / "shifted-low-entropy.bin"), source_size);
    REQUIRE_TRUE(files_are_equal(output / "shifted-low-entropy.bin", input));
    std::filesystem::remove_all(root);
}

// Purpose: Verify required-HIP verification rejects corrupted GPU-prefix payload bytes.
// Inputs: A valid required-HIP prefix archive with one encoded bitstream byte flipped after compression.
// Outputs: Throws if verification silently accepts the corrupted native GPU-prefix payload.
TEST_CASE(suzip_required_gpu_prefix_payload_corruption_is_rejected) {
    if (!superzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("suzip-required-gpu-prefix-corrupt");
    const auto input = root / "low-entropy.bin";
    write_low_entropy_payload(input, 1024U * 1024U);
    const auto archive = root / "archive.suzip";

    superzip::CompressOptions compress;
    compress.gpu_required = true;
    compress.force_cpu = false;
    compress.chunk_size = 1024U * 1024U;
    compress.block_size = 1024U * 1024U;
    const auto compressed = superzip::compress_suzip({input}, archive, compress);
    REQUIRE_TRUE(compressed.gpu_used);
    REQUIRE_TRUE(compressed.gpu_runtime.prefix_blocks > 0U);

    const auto prefix = find_first_prefix_block(read_test_archive_index(archive));
    const auto table_bytes = test_gpu_prefix_table_bytes(prefix.block.uncompressed_len);
    REQUIRE_TRUE(prefix.block.encoded_len > table_bytes);
    xor_archive_byte(archive, prefix.payload_offset + prefix.block.encoded_offset + table_bytes, 0x01U);

    superzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 1024U * 1024U;
    verify.block_size = 1024U * 1024U;
    bool rejected = false;
    try {
        (void)superzip::verify_suzip(archive, verify);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify required-HIP verification rejects corrupted GPU-prefix table offsets before GPU materialization.
// Inputs: A valid required-HIP prefix archive with its final prefix table offset corrupted after compression.
// Outputs: Throws if verification reaches a GPU read using malformed table-controlled offsets.
TEST_CASE(suzip_required_gpu_prefix_table_corruption_is_rejected) {
    if (!superzip::query_gpu_info().available) {
        return;
    }

    const auto root = test_temp_dir("suzip-required-gpu-prefix-table-corrupt");
    const auto input = root / "low-entropy.bin";
    write_low_entropy_payload(input, 1024U * 1024U);
    const auto archive = root / "archive.suzip";

    superzip::CompressOptions compress;
    compress.gpu_required = true;
    compress.force_cpu = false;
    compress.chunk_size = 1024U * 1024U;
    compress.block_size = 1024U * 1024U;
    const auto compressed = superzip::compress_suzip({input}, archive, compress);
    REQUIRE_TRUE(compressed.gpu_used);
    REQUIRE_TRUE(compressed.gpu_runtime.prefix_blocks > 0U);

    const auto prefix = find_first_prefix_block(read_test_archive_index(archive));
    const auto table_bytes = test_gpu_prefix_table_bytes(prefix.block.uncompressed_len);
    REQUIRE_TRUE(prefix.block.encoded_len > table_bytes);
    xor_archive_byte(archive, prefix.payload_offset + prefix.block.encoded_offset + table_bytes - 1U, 0x80U);

    superzip::ExtractOptions verify;
    verify.gpu_required = true;
    verify.force_cpu = false;
    verify.chunk_size = 1024U * 1024U;
    verify.block_size = 1024U * 1024U;
    bool rejected = false;
    try {
        (void)superzip::verify_suzip(archive, verify);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}
