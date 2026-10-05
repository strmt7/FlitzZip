#include "core/archive.hpp"
#include "core/byte_plane_block.hpp"
#include "core/checksum.hpp"
#include "gpu/gpu_codec.hpp"
#include "test_suzip_helpers.hpp"
#include "test_util.hpp"

#include <array>
#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>

namespace {

// Purpose: Supply an independent wire fixture whose inverse permutation has observable full records and tails.
// Inputs: An admitted plane width and bounded decoded extent.
// Outputs: Returns a plain pattern stage alternating two bytes, framed without using the production writer.
std::vector<std::byte> byte_plane_fixture(std::uint8_t width) {
    return {static_cast<std::byte>(width), std::byte{3}, std::byte{0}, std::byte{0}, std::byte{'A'}, std::byte{'B'}};
}

// Purpose: Describe an exact independently built version-ten frame.
// Inputs: Complete frame bytes and a bounded decoded extent.
// Outputs: Returns one GPU byte-plane descriptor with no hidden ownership.
superzip::BlockDescriptor byte_plane_descriptor(std::span<const std::byte> payload, std::uint32_t bytes) {
    return {.kind = superzip::BlockKind::GpuBytePlane,
            .uncompressed_len = bytes,
            .encoded_len = static_cast<std::uint32_t>(payload.size())};
}

// Purpose: Compute expected fixture bytes independently using explicit plane/record coordinates.
// Inputs: Bounded source geometry with a final partial record allowed.
// Outputs: Returns original-order bytes including an unchanged alternating partial tail.
std::vector<std::byte> expected_byte_plane_fixture(std::uint8_t width, std::size_t bytes) {
    std::vector<std::byte> result(bytes);
    const auto records = bytes / width;
    for (std::size_t index = 0U; index < bytes; ++index) {
        const auto offset = index < records * width ? (index % width) * records + index / width : index;
        result[index] = offset % 2U == 0U ? std::byte{'A'} : std::byte{'B'};
    }
    return result;
}

// Purpose: Demand rejection from both independent CPU and real HIP paths before any kernel dispatch.
// Inputs: An intentionally malformed wire fixture or descriptor.
// Outputs: Requires ArchiveError and zero device launches, including the integrity consumer.
void require_byte_plane_rejection(std::span<const std::byte> payload, const superzip::BlockDescriptor& block) {
    for (const bool hip : {false, true}) {
        if (hip && !superzip::query_gpu_info().available) {
            continue;
        }
        const superzip::GpuCodecOptions options{
            .require_gpu = hip, .force_cpu = !hip, .telemetry = std::make_shared<superzip::GpuTelemetry>()};
        std::vector<std::byte> output(64U);
        bool rejected = false;
        try {
            (void)superzip::decode_chunk(payload, std::span(&block, 1U), output, options);
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(superzip::snapshot_gpu_telemetry(*options.telemetry).kernel_launches, 0U);
        rejected = false;
        try {
            (void)superzip::crc_decoded_chunk(payload, std::span(&block, 1U), output.size(), options);
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(superzip::snapshot_gpu_telemetry(*options.telemetry).kernel_launches, 0U);
    }
}

}  // namespace

// Purpose: Verify every plane width and remainder through independent CPU, actual HIP and device CRC consumers.
// Inputs: Handcrafted pattern frames, tiny/segment/block boundary lengths and mixed raw/fill layouts.
// Outputs: Requires exact original bytes, positive actual HIP launches and independent source CRC equality.
TEST_CASE(suzip_byte_plane_independent_readers_tails_and_gpu_crc) {
    for (const std::uint8_t width : {2U, 4U, 8U}) {
        for (const std::uint32_t base : {16U, 255U, 4096U, 65536U, 262144U, 16777208U}) {
            for (std::uint32_t tail = 0U; tail < width; ++tail) {
                const auto bytes = base + tail;
                auto payload = byte_plane_fixture(width);
                const auto outer = byte_plane_descriptor(payload, bytes);
                payload.push_back(std::byte{'Z'});
                const std::array blocks{
                    outer,
                    superzip::BlockDescriptor{.kind = superzip::BlockKind::Raw,
                                              .uncompressed_len = 1U,
                                              .encoded_offset = outer.encoded_len,
                                              .encoded_len = 1U},
                    superzip::BlockDescriptor{.kind = superzip::BlockKind::Fill,
                                              .fill_value = 'Q',
                                              .uncompressed_len = 3U,
                                              .encoded_offset = std::numeric_limits<std::uint64_t>::max()}};
                auto expected = expected_byte_plane_fixture(width, bytes);
                expected.insert(expected.end(), {std::byte{'Z'}, std::byte{'Q'}, std::byte{'Q'}, std::byte{'Q'}});
                for (const bool hip : {false, true}) {
                    if (hip && !superzip::query_gpu_info().available) {
                        continue;
                    }
                    const superzip::GpuCodecOptions options{
                        .require_gpu = hip, .force_cpu = !hip, .telemetry = std::make_shared<superzip::GpuTelemetry>()};
                    std::vector<std::byte> decoded(expected.size());
                    REQUIRE_EQ(superzip::decode_chunk(payload, blocks, decoded, options), hip);
                    REQUIRE_EQ(decoded, expected);
                    const auto verified = superzip::crc_decoded_chunk(payload, blocks, expected.size(), options);
                    REQUIRE_EQ(verified.crc32, superzip::crc32(expected));
                    REQUIRE_EQ(verified.gpu_used, hip);
                    REQUIRE_EQ(superzip::snapshot_gpu_telemetry(*options.telemetry).kernel_launches > 0U, hip);
                }
            }
        }
    }
}

// Purpose: Reject malformed and recursive frames without letting serialization become an allocation request.
// Inputs: Every byte value at each header position, truncations, nonimproving sizes and invalid outer metadata.
// Outputs: Requires CPU/HIP decode and integrity rejection before any kernel launch.
TEST_CASE(suzip_byte_plane_malformed_frames_precede_device_work) {
    const auto original = byte_plane_fixture(4U);
    for (std::size_t bytes = 0U; bytes < superzip::kGpuBytePlaneHeaderBytes; ++bytes) {
        const auto short_payload = std::span(original).first(bytes);
        require_byte_plane_rejection(short_payload, byte_plane_descriptor(short_payload, 64U));
    }
    for (const std::size_t location : {0U, 1U, 2U, 3U}) {
        for (unsigned int value = 0U; value <= 255U; ++value) {
            if ((location == 0U && superzip::is_gpu_byte_plane_width(static_cast<std::uint8_t>(value))) ||
                (location == 1U && superzip::is_gpu_compound_stage(static_cast<superzip::BlockKind>(value))) ||
                (location >= 2U && value == 0U)) {
                continue;
            }
            auto malformed = original;
            malformed[location] = static_cast<std::byte>(value);
            require_byte_plane_rejection(malformed, byte_plane_descriptor(malformed, 64U));
        }
    }
    for (const std::uint32_t size : {0U, 3U, 6U, 16777217U, std::numeric_limits<std::uint32_t>::max()}) {
        require_byte_plane_rejection(original, byte_plane_descriptor(original, size));
    }
    auto block = byte_plane_descriptor(original, 64U);
    block.fill_value = 1U;
    require_byte_plane_rejection(original, block);
    block.fill_value = 0U;
    block.encoded_len += 1U;
    require_byte_plane_rejection(original, block);
    block.encoded_offset = std::numeric_limits<std::uint64_t>::max();
    require_byte_plane_rejection(original, block);
}

// Purpose: Exercise the public version-ten container and preserve every previous version's refusal of its new kind.
// Inputs: A handcrafted frame/index/footer with independent CRC and the portable extraction consumer.
// Outputs: Requires byte-exact CPU extraction and real HIP verification; downgrade is rejected at the index writer.
TEST_CASE(suzip_byte_plane_container_and_version_boundary) {
    using namespace superzip_test;
    const auto root = test_temp_dir("byte-plane-container");
    const auto archive = root / "data.suzip";
    const auto payload = byte_plane_fixture(4U);
    const auto expected = expected_byte_plane_fixture(4U, 4099U);
    superzip::ArchiveIndex index;
    index.version = 10U;
    index.entries.push_back({.path = "data.bin",
                             .uncompressed_size = expected.size(),
                             .payload_size = payload.size(),
                             .crc32 = superzip::crc32(expected),
                             .blocks = {byte_plane_descriptor(payload, static_cast<std::uint32_t>(expected.size()))}});
    {
        std::ofstream file(archive, std::ios::binary);
        file.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        const auto offset = static_cast<std::uint64_t>(file.tellp());
        superzip::write_archive_index(file, index);
        write_test_footer(file, offset, static_cast<std::uint64_t>(file.tellp()) - offset, index.version);
    }
    const superzip::ExtractOptions cpu{.gpu_required = false, .force_cpu = true};
    REQUIRE_EQ(superzip::verify_suzip(archive, cpu).entries, 1U);
    REQUIRE_EQ(superzip::extract_suzip(archive, root / "restored", cpu).entries, 1U);
    {
        std::ifstream file(root / "restored/data.bin", std::ios::binary);
        const std::string actual(std::istreambuf_iterator<char>(file), {});
        REQUIRE_EQ(actual, std::string(reinterpret_cast<const char*>(expected.data()), expected.size()));
    }
    if (superzip::query_gpu_info().available) {
        const auto verified = superzip::verify_suzip(archive, {.gpu_required = true});
        REQUIRE_TRUE(verified.gpu_used);
        REQUIRE_TRUE(verified.gpu_runtime.kernel_launches > 0U);
    }
    for (std::uint32_t version = 1U; version < 10U; ++version) {
        index.version = version;
        std::ostringstream encoded(std::ios::binary);
        bool rejected = false;
        try {
            superzip::write_archive_index(encoded, index);
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Exercise real GPU forward transforms and complete-cost selection independently of source naming.
// Inputs: Integer byte patterns with varying low fields, odd tails, owned/borrowed dispatch and numeric policy.
// Outputs: Requires emitted plane frames, independently reconstructed transform bytes and unchanged numeric output.
TEST_CASE(suzip_neutron_byte_plane_writer_gpu_only_and_isolated) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> input(65539U);
    std::uint32_t random = 0x714AFED1U;
    for (std::size_t offset = 0U; offset < input.size(); ++offset) {
        random ^= random << 13U;
        random ^= random >> 17U;
        random ^= random << 5U;
        input[offset] = offset % 4U < 2U ? static_cast<std::byte>(random & 255U) : std::byte{0};
    }
    const superzip::GpuCodecOptions ordinary{.require_gpu = true, .block_size = 262144U, .compression_level = 9};
    const auto baseline = superzip::encode_chunk(input, ordinary);
    auto neutron = ordinary;
    neutron.compression_mode = superzip::NativeCompressionMode::NeutronStar;
    neutron.telemetry = std::make_shared<superzip::GpuTelemetry>();
    int checkpoints = 0;
    neutron.encode_checkpoint = [&] { ++checkpoints; };
    const auto encoded = superzip::encode_chunk(input, neutron);
    REQUIRE_EQ(encoded.blocks.size(), 1U);
    REQUIRE_EQ(encoded.blocks.front().kind, superzip::BlockKind::GpuBytePlane);
    REQUIRE_TRUE(encoded.payload.size() < baseline.payload.size());
    REQUIRE_TRUE(superzip::snapshot_gpu_telemetry(*neutron.telemetry).kernel_launches > 0U);
    const auto stage = superzip::parse_gpu_byte_plane_block(encoded.payload, encoded.blocks.front());
    std::vector<std::byte> transformed(input.size());
    (void)superzip::decode_chunk(stage.payload, std::span(&stage.inner, 1U), transformed,
                                 {.require_gpu = false, .force_cpu = true});
    const auto records = input.size() / stage.width;
    for (std::size_t plane = 0U; plane < stage.width; ++plane) {
        for (std::size_t record = 0U; record < records; ++record) {
            REQUIRE_EQ(transformed[plane * records + record], input[record * stage.width + plane]);
        }
    }
    REQUIRE_TRUE(std::ranges::equal(std::span(transformed).subspan(records * stage.width),
                                    std::span(input).subspan(records * stage.width)));
    neutron.encode_checkpoint = {};
    const auto owned = superzip::encode_owned_chunk(input, neutron);
    REQUIRE_EQ(owned.payload, encoded.payload);
    REQUIRE_EQ(owned.blocks.front().kind, encoded.blocks.front().kind);
    for (const bool hip : {false, true}) {
        std::vector<std::byte> decoded(input.size());
        const superzip::GpuCodecOptions options{.require_gpu = hip, .force_cpu = !hip};
        REQUIRE_EQ(superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options), hip);
        REQUIRE_EQ(decoded, input);
        REQUIRE_EQ(superzip::crc_decoded_chunk(encoded.payload, encoded.blocks, input.size(), options).crc32,
                   superzip::crc32(input));
    }
    REQUIRE_EQ(superzip::encode_chunk(input, ordinary).payload, baseline.payload);
    const int final_checkpoint = checkpoints;
    REQUIRE_TRUE(final_checkpoint > 2);
    checkpoints = 0;
    neutron.encode_checkpoint = [&] {
        if (++checkpoints == final_checkpoint) {
            throw superzip::ArchiveError("cancel final byte-plane trial");
        }
    };
    bool cancelled = false;
    try {
        (void)superzip::encode_chunk(input, neutron);
    } catch (const superzip::ArchiveError& error) {
        cancelled = std::string_view(error.what()) == "cancel final byte-plane trial";
    }
    REQUIRE_TRUE(cancelled);
    REQUIRE_EQ(superzip::encode_chunk(input, ordinary).payload, baseline.payload);
    for (const auto options : {superzip::GpuCodecOptions{.require_gpu = false, .force_cpu = true},
                               superzip::GpuCodecOptions{.require_gpu = false}}) {
        auto invalid = options;
        invalid.compression_level = 9;
        invalid.compression_mode = superzip::NativeCompressionMode::NeutronStar;
        bool rejected = false;
        try {
            (void)superzip::encode_chunk(input, invalid);
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Preserve raw and tiny Neutron results through borrowed, owned and independent-batch submission.
// Inputs: Deterministic high-entropy bytes with small extents, odd tails and a larger raw winner.
// Outputs: Requires exact readback, identical owned bytes and per-file CRCs without any CPU creation fallback.
TEST_CASE(suzip_neutron_byte_planes_preserve_raw_tiny_and_batch_ownership) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    const superzip::GpuCodecOptions options{.require_gpu = true,
                                            .block_size = 262144U,
                                            .compression_level = 9,
                                            .compression_mode = superzip::NativeCompressionMode::NeutronStar};
    for (const std::size_t bytes : {0U, 1U, 2U, 3U, 7U, 8U, 15U, 16U, 513U, 32769U}) {
        std::vector<std::byte> input(bytes);
        std::uint32_t random = 0x731F68A1U;
        for (auto& byte : input) {
            random ^= random << 13U;
            random ^= random >> 17U;
            random ^= random << 5U;
            byte = static_cast<std::byte>(random & 255U);
        }
        const auto borrowed = superzip::encode_chunk(input, options);
        const auto owned = superzip::encode_owned_chunk(input, options);
        REQUIRE_EQ(owned.payload, borrowed.payload);
        REQUIRE_EQ(owned.source_crc32, superzip::crc32(input));
        std::vector<std::byte> decoded(bytes);
        (void)superzip::decode_chunk(owned.payload, owned.blocks, decoded, {.require_gpu = true});
        REQUIRE_EQ(decoded, input);
        if (bytes == 32769U) {
            REQUIRE_EQ(owned.blocks.front().kind, superzip::BlockKind::Raw);
            REQUIRE_EQ(owned.payload, input);
        }
        if (bytes > 0U) {
            auto dense = input;
            dense.insert(dense.end(), input.begin(), input.end());
            const std::array lengths{static_cast<std::uint32_t>(bytes), static_cast<std::uint32_t>(bytes)};
            const auto batch = superzip::encode_owned_block_batch(dense, lengths, options);
            REQUIRE_EQ(batch.block_crc32.size(), 2U);
            REQUIRE_EQ(batch.block_crc32[0], superzip::crc32(input));
            REQUIRE_EQ(batch.block_crc32[1], superzip::crc32(input));
            decoded.resize(dense.size());
            REQUIRE_TRUE(
                superzip::decode_chunk(batch.encoded.payload, batch.encoded.blocks, decoded, {.require_gpu = true}));
            REQUIRE_EQ(decoded, dense);
        }
    }
}

// Purpose: Qualify the payload-free inner fill stage independently from the production writer.
// Inputs: Handcrafted complete fill frames at each admitted width and odd decoded lengths.
// Outputs: Requires exact CPU/HIP inverse output and matching independent integrity checks.
TEST_CASE(suzip_byte_plane_fill_stage_independent_readback) {
    for (const std::uint8_t width : {2U, 4U, 8U}) {
        const std::array payload{static_cast<std::byte>(width), std::byte{1}, std::byte{'J'}, std::byte{0}};
        const auto block = byte_plane_descriptor(payload, 4099U);
        const std::vector<std::byte> expected(4099U, std::byte{'J'});
        for (const bool hip : {false, true}) {
            if (hip && !superzip::query_gpu_info().available) {
                continue;
            }
            const superzip::GpuCodecOptions options{.require_gpu = hip, .force_cpu = !hip};
            std::vector<std::byte> decoded(expected.size());
            REQUIRE_EQ(superzip::decode_chunk(payload, std::span(&block, 1U), decoded, options), hip);
            REQUIRE_EQ(decoded, expected);
            REQUIRE_EQ(superzip::crc_decoded_chunk(payload, std::span(&block, 1U), decoded.size(), options).crc32,
                       superzip::crc32(expected));
        }
    }
}
