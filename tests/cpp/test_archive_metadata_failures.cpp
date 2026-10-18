#include "test_util.hpp"

#include "core/archive.hpp"
#include "core/archive_blocks.hpp"
#include "core/archive_index.hpp"
#include "core/result.hpp"
#include "test_flzip_helpers.hpp"

#if FLITZZIP_ENABLE_HIP
#include "gpu/hip_codec_support.hpp"
#endif

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>

namespace {

using Path = std::filesystem::path;
using Kind = flitzzip::BlockKind;

// Independent Python zlib output for 1024 zero bytes, with CRC-32 4021661486.
constexpr std::array<unsigned char, 17> kDeflateZeroFrame{120, 156, 99, 96, 24, 5, 163, 96, 20,
                                                          140, 84,  0,  0,  4,  0, 0,   1};

// Purpose: Emit small hostile wire records without asking the production writer to admit invalid descriptors.
// Inputs: One entry, actual bounded payload and declared version. Outputs: A closed archive with independent framing.
void write_metadata_fixture(const Path& path, const flitzzip::ArchiveEntry& entry, std::string_view payload = "foo",
                            std::uint32_t version = 11) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    const auto offset = static_cast<std::uint64_t>(output.tellp());
    flitzzip::write_u32(output, flitzzip::kFlitzZipMagic);
    flitzzip::write_u32(output, version);
    flitzzip::write_u32(output, 1);
    flitzzip::write_u16(output, static_cast<std::uint16_t>(entry.path.size()));
    output.write(entry.path.data(), static_cast<std::streamsize>(entry.path.size()));
    output.put(entry.directory ? '\1' : '\0');
    flitzzip::write_u64(output, entry.uncompressed_size);
    flitzzip::write_u64(output, entry.payload_offset);
    flitzzip::write_u64(output, entry.payload_size);
    flitzzip::write_u32(output, entry.crc32);
    flitzzip::write_u32(output, static_cast<std::uint32_t>(entry.blocks.size()));
    for (const auto& block : entry.blocks) {
        output.put(static_cast<char>(block.kind));
        output.put(static_cast<char>(block.fill_value));
        flitzzip::write_u32(output, block.uncompressed_len);
        flitzzip::write_u64(output, block.encoded_offset);
        flitzzip::write_u32(output, block.encoded_len);
    }
    const auto index_bytes = static_cast<std::uint64_t>(output.tellp()) - offset;
    flitzzip_test::write_test_footer(output, offset, index_bytes, version);
    output.close();
}

// Purpose: Provide a positive native wire control independent of FlitzZip compression.
// Inputs: None. Outputs: One raw foo entry with an independently known CRC-32.
flitzzip::ArchiveEntry raw_control() {
    return {.path = "payload.bin",
            .uncompressed_size = 3,
            .payload_size = 3,
            .crc32 = 0x8C736521U,
            .blocks = {{.kind = Kind::Raw, .uncompressed_len = 3, .encoded_len = 3}}};
}

// Purpose: Require a particular validation boundary to reject before output publication.
// Inputs: Small fixture and expected diagnostic fragment. Outputs: Both verify and extract reject, preserving bytes.
void require_metadata_rejection(const Path& archive, const Path& destination, std::string_view diagnostic) {
    const flitzzip::ExtractOptions options{.gpu_required = false, .force_cpu = true, .overwrite = true};
    for (const bool verify : {true, false}) {
        bool rejected = false;
        try {
            if (verify) {
                (void)flitzzip::verify_flzip(archive, options);
            } else {
                (void)flitzzip::extract_flzip(archive, destination, options);
            }
        } catch (const flitzzip::ArchiveError& error) {
            REQUIRE_TRUE(std::string_view(error.what()).find(diagnostic) != std::string_view::npos);
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        std::ifstream original(destination / "payload.bin", std::ios::binary);
        REQUIRE_TRUE(original.is_open());
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(original), {}), "preserve destination");
        REQUIRE_EQ(
            std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator{}), 1);
    }
}

// Purpose: Own bounded hostile metadata fixtures and the destination that must survive them.
// Inputs: Test label. Outputs: Isolated files and no leftover directory on scope exit.
struct MetadataFixture {
    Path root;
    Path archive;
    Path destination;
    // Purpose: Establish a sentinel target. Inputs: Stable label. Outputs: An existing destination with known bytes.
    explicit MetadataFixture(const char* label)
        : root(test_temp_dir(label)), archive(root / "test.flzip"), destination(root / "out") {
        std::filesystem::create_directory(destination);
        std::ofstream output(destination / "payload.bin", std::ios::binary);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << "preserve destination";
        output.close();
    }
    // Purpose: Remove closed owned fixtures. Inputs: Owned root. Outputs: Best-effort cleanup.
    ~MetadataFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

}  // namespace

// Purpose: Validate a positive raw fixture through every readable native format version.
// Inputs: Independently framed foo bytes and its CRC-32, with versions one through eleven.
// Outputs: Verify and extraction both agree on bytes/counts without requiring GPU hardware.
TEST_CASE(flzip_metadata_positive_controls_cover_readable_versions) {
    MetadataFixture fixture("native-metadata-positive");
    const flitzzip::ExtractOptions options{.gpu_required = false, .force_cpu = true, .overwrite = true};
    for (std::uint32_t version = 1; version <= 11; ++version) {
        write_metadata_fixture(fixture.archive, raw_control(), "foo", version);
        REQUIRE_EQ(flitzzip::verify_flzip(fixture.archive, options).entries, 1U);
        REQUIRE_EQ(flitzzip::extract_flzip(fixture.archive, fixture.destination, options).output_bytes, 3U);
        std::ifstream restored(fixture.destination / "payload.bin", std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(restored), {}), "foo");
    }
}

// Purpose: Reject incomplete or overlong Deflate streams without replacing existing destination bytes.
// Inputs: An independent zlib frame, every byte truncation, suffixes, and wrong decoded lengths in all readable
// versions. Outputs: Both public readers reject each malformed frame and subsequently verify/extract the intact frame
// exactly.
TEST_CASE(flzip_deflate_frames_require_exact_consumption) {
    const std::string frame(kDeflateZeroFrame.begin(), kDeflateZeroFrame.end());
    const flitzzip::ExtractOptions options{.gpu_required = false, .force_cpu = true, .overwrite = true};
    for (std::uint32_t version = 1; version <= 11; ++version) {
        MetadataFixture fixture("native-deflate-frame");
        auto entry = raw_control();
        entry.uncompressed_size = 1024;
        entry.payload_size = frame.size();
        entry.crc32 = 4021661486U;
        entry.blocks[0] = {
            .kind = Kind::Deflate, .uncompressed_len = 1024, .encoded_len = static_cast<std::uint32_t>(frame.size())};
        write_metadata_fixture(fixture.archive, entry, frame, version);
        REQUIRE_EQ(flitzzip::verify_flzip(fixture.archive, options).entries, 1U);

        std::vector<std::string> malformed{frame + '\0', frame + frame};
        for (std::size_t length = 1; length < frame.size(); ++length) {
            malformed.push_back(frame.substr(0, length));
        }
        for (const auto& payload : malformed) {
            auto invalid = entry;
            invalid.payload_size = payload.size();
            invalid.blocks[0].encoded_len = static_cast<std::uint32_t>(payload.size());
            write_metadata_fixture(fixture.archive, invalid, payload, version);
            require_metadata_rejection(fixture.archive, fixture.destination, "deflate block failed to decompress");
        }
        for (const auto length : {1023U, 1025U}) {
            auto invalid = entry;
            invalid.uncompressed_size = length;
            invalid.blocks[0].uncompressed_len = length;
            write_metadata_fixture(fixture.archive, invalid, frame, version);
            require_metadata_rejection(fixture.archive, fixture.destination, "deflate block failed to decompress");
        }
        write_metadata_fixture(fixture.archive, entry, frame, version);
        REQUIRE_EQ(flitzzip::verify_flzip(fixture.archive, options).entries, 1U);
        REQUIRE_EQ(flitzzip::extract_flzip(fixture.archive, fixture.destination, options).output_bytes, 1024U);
        std::ifstream restored(fixture.destination / "payload.bin", std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(restored), {}), std::string(1024, '\0'));
        REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(fixture.destination),
                                 std::filesystem::directory_iterator{}),
                   1);
    }
}

// Purpose: Enforce Deflate input consumption in both whole-chunk and mixed CPU/GPU decode orchestration.
// Inputs: A frame between two raw blocks, an overlong declared frame, and serial/parallel worker budgets.
// Outputs: Consumers reject excess bytes, recover, and modify only the block ranges each consumer owns.
TEST_CASE(flzip_deflate_consumers_preserve_block_boundaries_and_recover) {
    std::vector<std::byte> payload{std::byte{0x11}};
    const auto frame = std::as_bytes(std::span(kDeflateZeroFrame));
    payload.insert(payload.end(), frame.begin(), frame.end());
    payload.push_back(std::byte{0x22});
    for (const auto workers : {1U, 3U}) {
        for (const auto decode : {&flitzzip::decode_chunk_cpu, &flitzzip::decode_cpu_only_blocks_cpu}) {
            const flitzzip::ArchiveCodecOptions options{.worker_count = workers};
            std::array blocks{
                flitzzip::BlockDescriptor{.kind = Kind::Raw, .uncompressed_len = 1, .encoded_len = 1},
                flitzzip::BlockDescriptor{
                    .kind = Kind::Deflate, .uncompressed_len = 1024, .encoded_offset = 1, .encoded_len = 17},
                flitzzip::BlockDescriptor{
                    .kind = Kind::Raw, .uncompressed_len = 1, .encoded_offset = 18, .encoded_len = 1}};
            std::array<std::byte, 1026> output{};
            for (const bool invalid : {false, true, false}) {
                std::ranges::fill(output, std::byte{0x7D});
                blocks[1].encoded_len = invalid ? 18U : 17U;
                bool rejected = false;
                try {
                    decode(payload, blocks, output, options);
                } catch (const flitzzip::ArchiveError& error) {
                    REQUIRE_EQ(std::string(error.what()), "deflate block failed to decompress");
                    rejected = true;
                }
                REQUIRE_EQ(rejected, invalid);
                if (!invalid) {
                    REQUIRE_EQ(std::ranges::count(std::span(output).subspan(1, 1024), std::byte{0}), 1024);
                    const bool whole_chunk = decode == &flitzzip::decode_chunk_cpu;
                    REQUIRE_EQ(output.front(), whole_chunk ? std::byte{0x11} : std::byte{0x7D});
                    REQUIRE_EQ(output.back(), whole_chunk ? std::byte{0x22} : std::byte{0x7D});
                }
            }
        }
    }
}

// Purpose: Reject zero/oversized decoded blocks and unknown kind values before allocating decoder work.
// Inputs: Mutations of one otherwise valid raw record. Outputs: Exact metadata diagnostics and preserved targets.
TEST_CASE(flzip_metadata_rejects_invalid_block_headers) {
    MetadataFixture fixture("native-block-header");
    for (const auto size : {0U, flitzzip::kMaxArchiveBlockBytes + 1U}) {
        auto entry = raw_control();
        entry.blocks[0].uncompressed_len = size;
        entry.uncompressed_size = size;
        write_metadata_fixture(fixture.archive, entry);
        require_metadata_rejection(fixture.archive, fixture.destination,
                                   size == 0 ? "zero decoded length" : "block size limit");
    }
    auto entry = raw_control();
    entry.blocks[0].kind = static_cast<Kind>(255);
    write_metadata_fixture(fixture.archive, entry);
    require_metadata_rejection(fixture.archive, fixture.destination, "unknown encoding kind");
}

// Purpose: Reject invalid compressed-size relationships for every native block representation.
// Inputs: All supported kinds, zero/equal/oversized payload lengths and small bounded wire fixtures.
// Outputs: Both public readers refuse malformed metadata before decoder execution or publication.
TEST_CASE(flzip_metadata_rejects_invalid_payload_lengths_for_all_kinds) {
    MetadataFixture fixture("native-payload-lengths");
    constexpr std::array kinds{Kind::Raw,           Kind::Fill,
                               Kind::Deflate,       Kind::Pattern,
                               Kind::GpuPrefix,     Kind::GpuAdaptivePrefix,
                               Kind::GpuDictionary, Kind::GpuSparsePattern,
                               Kind::CpuZstd,       Kind::GpuLongSparsePattern,
                               Kind::GpuHuffman,    Kind::GpuCompound,
                               Kind::GpuBytePlane,  Kind::GpuBytePlaneContexts};
    const std::string payload(32769, 'x');
    for (const auto kind : kinds) {
        for (const auto length : {0U, 32768U, 32769U}) {
            if ((kind == Kind::Raw && length == 32768U) || (kind == Kind::Fill && length == 0U)) {
                continue;  // These two extent relationships are valid; other tests qualify their actual payloads.
            }
            auto entry = raw_control();
            entry.uncompressed_size = 32768;
            entry.payload_size = length;
            entry.blocks[0] = {.kind = kind, .uncompressed_len = 32768, .encoded_len = length};
            write_metadata_fixture(fixture.archive, entry, payload);
            require_metadata_rejection(fixture.archive, fixture.destination,
                                       kind == Kind::Fill ? "fill block contains payload" : "invalid");
        }
    }
}

// Purpose: Reject holes and overlap independently of codec payload contents.
// Inputs: Valid outer lengths for each payload-bearing kind but nonzero first block offsets.
// Outputs: Both readers report the dense-offset violation before consulting compressed payload bytes.
TEST_CASE(flzip_metadata_rejects_sparse_and_overlapping_payload_offsets) {
    MetadataFixture fixture("native-payload-offsets");
    constexpr std::array<std::pair<Kind, std::uint32_t>, 13> shapes{{std::pair{Kind::Raw, 32768U},
                                                                     {Kind::Deflate, 12U},
                                                                     {Kind::CpuZstd, 12U},
                                                                     {Kind::Pattern, 2U},
                                                                     {Kind::GpuPrefix, 37U},
                                                                     {Kind::GpuAdaptivePrefix, 1024U},
                                                                     {Kind::GpuDictionary, 32U},
                                                                     {Kind::GpuSparsePattern, 64U},
                                                                     {Kind::GpuLongSparsePattern, 64U},
                                                                     {Kind::GpuHuffman, 8230U},
                                                                     {Kind::GpuCompound, 8U},
                                                                     {Kind::GpuBytePlane, 4U},
                                                                     {Kind::GpuBytePlaneContexts, 16U}}};
    const std::string payload(32768, 'x');
    for (const auto& [kind, length] : shapes) {
        for (const auto offset : {1ULL, 0xFFFFFFFFFFFFFFFFULL}) {
            auto entry = raw_control();
            entry.uncompressed_size = 32768;
            entry.payload_size = length;
            entry.blocks[0] = {
                .kind = kind, .uncompressed_len = 32768, .encoded_offset = offset, .encoded_len = length};
            write_metadata_fixture(fixture.archive, entry, payload);
            require_metadata_rejection(fixture.archive, fixture.destination, "sparse or overlapping");
        }
    }
}

// Purpose: Reject file/directory inconsistencies and mismatched aggregate extents before touching destinations.
// Inputs: Otherwise valid raw records with one independently changed aggregate field.
// Outputs: Both verify and extract reject each record at the expected shared metadata boundary.
TEST_CASE(flzip_metadata_rejects_entry_aggregate_inconsistencies) {
    MetadataFixture fixture("native-entry-aggregates");
    for (const bool directory : {false, true}) {
        auto entry = raw_control();
        entry.directory = directory;
        entry.uncompressed_size = 4;
        write_metadata_fixture(fixture.archive, entry);
        require_metadata_rejection(fixture.archive, fixture.destination,
                                   directory ? "directory entry has payload" : "block sizes do not match");
    }
    auto entry = raw_control();
    entry.payload_size = 4;
    write_metadata_fixture(fixture.archive, entry, "foox");
    require_metadata_rejection(fixture.archive, fixture.destination, "payload size does not match");
    entry = raw_control();
    entry.payload_offset = 0xFFFFFFFFFFFFFFFFULL;
    write_metadata_fixture(fixture.archive, entry);
    require_metadata_rejection(fixture.archive, fixture.destination, "payload points");
}

// Purpose: Prove invalid resource options are rejected by create, extract and verify before filesystem work.
// Inputs: Each public chunk/block/worker/inflight limit violated independently, using a nonexistent source.
// Outputs: Specific option diagnostics, unchanged destination and no published archive.
TEST_CASE(flzip_public_operations_reject_invalid_resource_options_first) {
    struct Limits {
        std::uint64_t chunk;
        std::uint32_t block;
        std::uint32_t workers;
        std::uint32_t inflight;
        const char* diagnostic;
    };
    constexpr std::array<Limits, 8> cases{
        {Limits{0, 4096, 1, 1, "chunk size must"},
         {flitzzip::kMaxArchiveChunkBytes + 1, 4096, 1, 1, "chunk size exceeds"},
         {8192, 0, 1, 1, "block size is outside"},
         {8192, flitzzip::kMaxArchiveBlockBytes + 1, 1, 1, "block size is outside"},
         {4096, 8192, 1, 1, "block size cannot exceed"},
         {8193, 4096, 1, 1, "exact multiple"},
         {8192, 4096, flitzzip::kMaxArchiveWorkers + 1, 1, "worker count exceeds"},
         {8192, 4096, 1, flitzzip::kMaxInflightArchiveChunks + 1, "in-flight chunk count exceeds"}}};
    MetadataFixture fixture("native-option-limits");
    for (const auto& limits : cases) {
        for (const int operation : {0, 1, 2}) {
            bool rejected = false;
            try {
                const flitzzip::ExtractOptions extract{.gpu_required = false,
                                                       .force_cpu = true,
                                                       .overwrite = true,
                                                       .chunk_size = limits.chunk,
                                                       .block_size = limits.block,
                                                       .worker_count = limits.workers,
                                                       .max_inflight_chunks = limits.inflight};
                if (operation == 0) {
                    flitzzip::CompressOptions create{.gpu_required = false,
                                                     .force_cpu = true,
                                                     .chunk_size = limits.chunk,
                                                     .block_size = limits.block,
                                                     .worker_count = limits.workers,
                                                     .max_inflight_chunks = limits.inflight};
                    (void)flitzzip::compress_flzip({fixture.root / "missing"}, fixture.archive, create);
                } else if (operation == 1) {
                    (void)flitzzip::extract_flzip(fixture.archive, fixture.destination, extract);
                } else {
                    (void)flitzzip::verify_flzip(fixture.archive, extract);
                }
            } catch (const flitzzip::ArchiveError& error) {
                REQUIRE_TRUE(std::string_view(error.what()).find(limits.diagnostic) != std::string_view::npos);
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
            REQUIRE_TRUE(!std::filesystem::exists(fixture.archive));
        }
    }
}

// Purpose: Guard HIP's final host admission even when an internal caller bypasses the public archive parser.
// Inputs: Independent raw/fill controls and malformed extents for every payload-bearing native block kind.
// Outputs: Valid controls pass; truncation, overflow, unknown kinds and output mismatches fail without GPU dispatch.
TEST_CASE(archive_metadata_hip_layout_rejects_untrusted_extents) {
#if FLITZZIP_ENABLE_HIP
    using flitzzip::BlockDescriptor;
    using flitzzip::BlockKind;
    using flitzzip::hip_detail::validate_decode_layout;
    std::array<std::byte, 4096> payload{};
    BlockDescriptor raw{.kind = BlockKind::Raw, .uncompressed_len = 4096U, .encoded_len = 4096U};
    BlockDescriptor fill{.kind = BlockKind::Fill, .fill_value = 37U, .uncompressed_len = 4096U};
    validate_decode_layout(payload, std::span(&raw, 1U), 4096U, 0U);
    validate_decode_layout({}, std::span(&fill, 1U), 4096U, 0U);
    validate_decode_layout({}, {}, 0U, 0U);

    // Purpose: Require a controlled admission error before any kernel or output buffer exists.
    // Inputs: One hostile descriptor table and declared output length. Outputs: Assertion on missing rejection.
    const auto reject = [&](std::span<const BlockDescriptor> blocks, std::size_t output_bytes) {
        bool rejected = false;
        try {
            validate_decode_layout(payload, blocks, output_bytes, 0U);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    };
    reject({}, 1U);
    reject(std::span(&raw, 1U), 4095U);
    reject(std::span(&raw, 1U), 4097U);
    auto changed = raw;
    changed.uncompressed_len = 0U;
    reject(std::span(&changed, 1U), 0U);
    changed = raw;
    --changed.encoded_len;
    reject(std::span(&changed, 1U), 4096U);
    changed = fill;
    changed.encoded_len = 1U;
    reject(std::span(&changed, 1U), 4096U);
    changed = raw;
    changed.kind = static_cast<BlockKind>(255U);
    reject(std::span(&changed, 1U), 4096U);
    for (const auto kind :
         {BlockKind::Raw, BlockKind::Deflate, BlockKind::Pattern, BlockKind::GpuPrefix, BlockKind::GpuAdaptivePrefix,
          BlockKind::GpuDictionary, BlockKind::GpuSparsePattern, BlockKind::CpuZstd, BlockKind::GpuLongSparsePattern,
          BlockKind::GpuHuffman, BlockKind::GpuCompound, BlockKind::GpuBytePlane, BlockKind::GpuBytePlaneContexts}) {
        const auto encoded = kind == BlockKind::Raw ? 4096U : kind == BlockKind::Pattern ? 8U : 512U;
        for (const std::uint64_t offset : {4097ULL - encoded, 4097ULL, std::numeric_limits<std::uint64_t>::max()}) {
            const BlockDescriptor invalid{
                .kind = kind, .uncompressed_len = 4096U, .encoded_offset = offset, .encoded_len = encoded};
            reject(std::span(&invalid, 1U), 4096U);
        }
    }
    for (const auto kind : {BlockKind::Deflate, BlockKind::CpuZstd}) {
        const BlockDescriptor empty{.kind = kind, .uncompressed_len = 4096U};
        reject(std::span(&empty, 1U), 4096U);
    }
#endif
}
