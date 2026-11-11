#include "core/archive_block_version.hpp"
#include "core/archive.hpp"
#include "core/checksum.hpp"
#include "core/result.hpp"
#include "gpu/gpu_codec.hpp"
#include "test_flzip_helpers.hpp"
#include "test_util.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using flitzzip::BlockDescriptor;
using flitzzip::BlockKind;

struct PreflightFixture {
    BlockKind kind;
    std::uint32_t version;
    std::vector<std::byte> payload;
    std::size_t reserved;
};

// Independent wire literals: secondary fill expands a 16-byte pattern;
// whole-plane fill and two/four/eight independent fill planes all reconstruct 'A'.
std::vector<PreflightFixture> preflight_fixtures() {
    std::vector<PreflightFixture> fixtures{
        {BlockKind::GpuCompound,
         9U,
         {std::byte{1}, std::byte{3}, std::byte{65}, std::byte{0}, std::byte{16}, std::byte{0}, std::byte{0},
          std::byte{0}},
         3U},
        {BlockKind::GpuBytePlane, 10U, {std::byte{2}, std::byte{1}, std::byte{65}, std::byte{0}}, 3U},
    };
    for (const auto width : std::array<std::uint8_t, 3>{2U, 4U, 8U}) {
        std::vector<std::byte> bytes{std::byte{width}, std::byte{0}, std::byte{0}, std::byte{0}};
        for (std::uint8_t plane = 0; plane < width; ++plane) {
            bytes.insert(bytes.end(),
                         {std::byte{1}, std::byte{65}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}});
        }
        fixtures.push_back({BlockKind::GpuBytePlaneContexts, 11U, std::move(bytes), 1U});
    }
    return fixtures;
}

// The expected decoded bytes and version are independently fixed by the wire fixture.
BlockDescriptor fixture_descriptor(const PreflightFixture& fixture) {
    return {.kind = fixture.kind,
            .uncompressed_len = 4096U,
            .encoded_len = static_cast<std::uint32_t>(fixture.payload.size())};
}

// Legal first entry proves that rejection precedes even unrelated earlier publication.
// Only the native index/footer serializer is reused; payload framing is handcrafted above.
void write_preflight_archive(const std::filesystem::path& path, const PreflightFixture& fixture) {
    const std::array<std::byte, 4> earlier{std::byte{'s'}, std::byte{'a'}, std::byte{'f'}, std::byte{'e'}};
    const std::vector<std::byte> later(4096U, std::byte{65});
    flitzzip::ArchiveIndex index{.version = fixture.version};
    index.entries.push_back({.path = "earlier.bin",
                             .uncompressed_size = earlier.size(),
                             .payload_size = earlier.size(),
                             .crc32 = flitzzip::crc32(earlier),
                             .blocks = {{.kind = BlockKind::Raw, .uncompressed_len = 4U, .encoded_len = 4U}}});
    index.entries.push_back({.path = "later.bin",
                             .uncompressed_size = later.size(),
                             .payload_offset = earlier.size(),
                             .payload_size = fixture.payload.size(),
                             .crc32 = flitzzip::crc32(later),
                             .blocks = {fixture_descriptor(fixture)}});
    std::ofstream output(path, std::ios::binary);
    REQUIRE_TRUE(output.is_open());
    output.write(reinterpret_cast<const char*>(earlier.data()), static_cast<std::streamsize>(earlier.size()));
    output.write(reinterpret_cast<const char*>(fixture.payload.data()),
                 static_cast<std::streamsize>(fixture.payload.size()));
    index.index_offset = earlier.size() + fixture.payload.size();
    flitzzip::write_archive_index(output, index);
    REQUIRE_TRUE(output.good());
    const auto end = static_cast<std::uint64_t>(static_cast<std::streamoff>(output.tellp()));
    flitzzip_test::write_test_footer(output, index.index_offset, end - index.index_offset, fixture.version);
    output.close();
    REQUIRE_TRUE(output.good());
}

template <typename Action>
// Require the public archive error boundary, not an unrelated setup/allocation exception.
void require_archive_rejection(Action&& action) {
    bool rejected = false;
    try {
        action();
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

}  // namespace

TEST_CASE(flzip_payload_preflight_headers_preserve_legacy_geometry_and_version_floors) {
    for (const auto& fixture : preflight_fixtures()) {
        const auto block = fixture_descriptor(fixture);
        REQUIRE_EQ(flitzzip::minimum_archive_version_for_encoded_header(block, fixture.payload), fixture.version);
        for (std::size_t bytes = 0; bytes < fixture.payload.size(); ++bytes) {
            require_archive_rejection([&] {
                (void)flitzzip::minimum_archive_version_for_encoded_header(block,
                                                                           std::span(fixture.payload).first(bytes));
            });
        }
        auto changed = fixture;
        changed.payload[fixture.reserved] = std::byte{1};
        require_archive_rejection(
            [&] { (void)flitzzip::minimum_archive_version_for_encoded_header(block, changed.payload); });
    }
}

TEST_CASE(flzip_payload_preflight_header_admission_does_not_admit_missing_codec_bodies) {
    struct HeaderFixture {
        BlockDescriptor block;
        std::vector<std::byte> prefix;
        std::uint32_t version;
    };
    const std::array<HeaderFixture, 3> fixtures{{
        {{.kind = BlockKind::GpuCompound, .uncompressed_len = 4096U, .encoded_len = 12U},
         {std::byte{3}, std::byte{3}, std::byte{0}, std::byte{0}, std::byte{64}, std::byte{0}, std::byte{0},
          std::byte{0}},
         9U},
        {{.kind = BlockKind::GpuBytePlane, .uncompressed_len = 4096U, .encoded_len = 6U},
         {std::byte{2}, std::byte{3}, std::byte{0}, std::byte{0}},
         10U},
        {{.kind = BlockKind::GpuBytePlaneContexts, .uncompressed_len = 128U, .encoded_len = 80U},
         {std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{65}, std::byte{0},
          std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{64}, std::byte{0},
          std::byte{0}, std::byte{0}},
         11U},
    }};
    for (const auto& fixture : fixtures) {
        REQUIRE_EQ(flitzzip::minimum_archive_version_for_encoded_header(fixture.block, fixture.prefix),
                   fixture.version);
        require_archive_rejection(
            [&] { (void)flitzzip::inspect_encoded_block_version(fixture.block, fixture.prefix); });
        require_archive_rejection([&] {
            if (fixture.block.kind == BlockKind::GpuCompound) {
                (void)flitzzip::parse_gpu_compound_block(fixture.prefix, fixture.block);
            } else if (fixture.block.kind == BlockKind::GpuBytePlane) {
                (void)flitzzip::parse_gpu_byte_plane_block(fixture.prefix, fixture.block);
            } else {
                (void)flitzzip::parse_gpu_byte_plane_contexts(fixture.prefix, fixture.block);
            }
        });
    }
    REQUIRE_EQ(flitzzip::encoded_block_version_prefix_bytes(BlockKind::GpuCompound), 8U);
    REQUIRE_EQ(flitzzip::encoded_block_version_prefix_bytes(BlockKind::GpuBytePlane), 4U);
    REQUIRE_EQ(flitzzip::encoded_block_version_prefix_bytes(BlockKind::GpuBytePlaneContexts), 52U);
    REQUIRE_EQ(flitzzip::encoded_block_version_prefix_bytes(BlockKind::Raw), 0U);
}

TEST_CASE(flzip_payload_preflight_rejects_later_bad_headers_before_decode_or_publication) {
    const flitzzip_test::ScopedTestDirectory directory{test_temp_dir("payload-header-preflight-reject")};
    std::size_t serial = 0;
    for (auto fixture : preflight_fixtures()) {
        fixture.payload[fixture.reserved] = std::byte{1};
        const auto archive = directory.path / (std::to_string(serial++) + ".flzip");
        write_preflight_archive(archive, fixture);
        for (const bool hip : {false, true}) {
            const flitzzip::ExtractOptions options{.gpu_required = hip, .force_cpu = !hip};
            const auto destination = directory.path / (std::to_string(serial++) + "-output");
            std::uint32_t callbacks = 0;
            const auto progress = [&](const flitzzip::ProgressSnapshot&) { ++callbacks; };
            require_archive_rejection([&] { (void)flitzzip::verify_flzip(archive, options, progress); });
            require_archive_rejection([&] { (void)flitzzip::extract_flzip(archive, destination, options, progress); });
            REQUIRE_EQ(callbacks, 0U);
            REQUIRE_TRUE(!std::filesystem::exists(destination));
        }
    }
    std::filesystem::remove_all(directory.path);
}

TEST_CASE(flzip_payload_preflight_preserves_cpu_and_required_hip_legacy_readback) {
    // Compilation and hardware admission are separate. Always exercise CPU readback, then either actual HIP
    // readback or explicit required-HIP rejection. The canonical HIP cohort independently requires a real device.
    const bool hip_available = flitzzip::query_gpu_info().available;
    constexpr std::array backends{false, true};
    const flitzzip_test::ScopedTestDirectory directory{test_temp_dir("payload-header-preflight-readback")};
    std::size_t serial = 0;
    for (const auto& fixture : preflight_fixtures()) {
        const auto archive = directory.path / (std::to_string(serial++) + ".flzip");
        write_preflight_archive(archive, fixture);
        for (const bool hip : backends) {
            const flitzzip::ExtractOptions options{.gpu_required = hip, .force_cpu = !hip};
            const auto destination = directory.path / (std::to_string(serial++) + "-output");
            if (hip && !hip_available) {
                bool rejected = false;
                try {
                    (void)flitzzip::verify_flzip(archive, options);
                } catch (const flitzzip::GpuError&) {
                    rejected = true;
                }
                REQUIRE_TRUE(rejected);
                rejected = false;
                try {
                    (void)flitzzip::extract_flzip(archive, destination, options);
                } catch (const flitzzip::GpuError&) {
                    rejected = true;
                }
                REQUIRE_TRUE(rejected);
                REQUIRE_TRUE(!std::filesystem::exists(destination / "earlier.bin"));
                REQUIRE_TRUE(!std::filesystem::exists(destination / "later.bin"));
                continue;
            }
            const auto verified = flitzzip::verify_flzip(archive, options);
            REQUIRE_EQ(verified.gpu_used, hip);
            const auto extracted = flitzzip::extract_flzip(archive, destination, options);
            REQUIRE_EQ(extracted.gpu_used, hip);
            for (const auto& name : {"earlier.bin", "later.bin"}) {
                std::ifstream input(destination / name, std::ios::binary);
                const std::string actual{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
                REQUIRE_EQ(actual, std::string(name) == "earlier.bin" ? std::string("safe") : std::string(4096U, 'A'));
            }
        }
    }
    std::filesystem::remove_all(directory.path);
}
