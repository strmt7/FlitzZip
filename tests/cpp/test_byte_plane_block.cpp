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
#include <utility>

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
void require_byte_plane_rejection(std::span<const std::byte> payload, const superzip::BlockDescriptor& block,
                                  std::size_t output_bytes = 64U) {
    for (const bool hip : {false, true}) {
        if (hip && !superzip::query_gpu_info().available) {
            continue;
        }
        const superzip::GpuCodecOptions options{
            .require_gpu = hip, .force_cpu = !hip, .telemetry = std::make_shared<superzip::GpuTelemetry>()};
        std::vector<std::byte> output(output_bytes);
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

// Purpose: Construct a context frame independently from the production writer.
// Inputs: Admitted width and decoded extent; planes alternate fill, two-byte pattern and raw representations.
// Outputs: Returns a dense closed-stage table and original-order oracle bytes, including the partial tail.
std::pair<std::vector<std::byte>, std::vector<std::byte>> context_plane_fixture(std::uint8_t width,
                                                                                std::uint32_t bytes) {
    std::vector<std::byte> frame(4U + 6U * width, std::byte{0});
    frame[0] = static_cast<std::byte>(width);
    const auto records = bytes / width;
    std::vector<std::byte> expected(bytes);
    for (std::size_t plane = 0U; plane < width; ++plane) {
        const auto count = records + (plane + 1U == width ? bytes % width : 0U);
        const auto kind = plane % 3U == 0U   ? superzip::BlockKind::Fill
                          : plane % 3U == 1U ? superzip::BlockKind::Pattern
                                             : superzip::BlockKind::Raw;
        const auto length = kind == superzip::BlockKind::Fill ? 0U : kind == superzip::BlockKind::Pattern ? 2U : count;
        const auto offset = 4U + 6U * plane;
        frame[offset] = static_cast<std::byte>(kind);
        frame[offset + 1U] = kind == superzip::BlockKind::Fill ? static_cast<std::byte>('A' + plane) : std::byte{0};
        for (std::size_t byte = 0U; byte < 4U; ++byte) {
            frame[offset + 2U + byte] = static_cast<std::byte>((length >> (byte * 8U)) & 255U);
        }
        if (kind == superzip::BlockKind::Pattern) {
            frame.push_back(static_cast<std::byte>('A' + plane));
            frame.push_back(static_cast<std::byte>('a' + plane));
        }
        for (std::size_t record = 0U; record < count; ++record) {
            const auto value = kind == superzip::BlockKind::Raw ? static_cast<std::byte>((plane * 31U + record) % 251U)
                               : kind == superzip::BlockKind::Fill || record % 2U == 0U
                                   ? static_cast<std::byte>('A' + plane)
                                   : static_cast<std::byte>('a' + plane);
            if (kind == superzip::BlockKind::Raw) {
                frame.push_back(value);
            }
            const auto position = record < records ? record * width + plane : records * width + record - records;
            expected[position] = value;
        }
    }
    return {std::move(frame), std::move(expected)};
}

}  // namespace

// Purpose: Keep the context-stage catalog and every extent/fill refusal independent from production classification.
// Inputs: All serialized byte kinds, an explicit format catalog and independently constructed dense tables.
// Outputs: Requires the exact accepted catalog and rejection of noncanonical fill, raw and compressed extents.
TEST_CASE(suzip_byte_plane_context_stage_catalog_and_extent_admission) {
    const std::array<std::uint8_t, 9> admitted_kinds{0U, 1U, 3U, 4U, 5U, 6U, 7U, 9U, 10U};
    // Purpose: Build an independent two-plane table. Inputs: First child metadata. Outputs: A dense wire frame.
    const auto make_frame = [](std::uint8_t kind, std::uint8_t fill, std::uint32_t bytes) {
        std::vector<std::byte> frame(16U + bytes, std::byte{0});
        frame[0] = std::byte{2};
        frame[4] = static_cast<std::byte>(kind);
        frame[5] = static_cast<std::byte>(fill);
        frame[10] = std::byte{1};
        for (std::size_t byte = 0U; byte < sizeof(bytes); ++byte) {
            frame[6U + byte] = static_cast<std::byte>((bytes >> (byte * 8U)) & 255U);
        }
        return frame;
    };
    // Purpose: Exercise actual frame admission. Inputs: Independent wire bytes. Outputs: Acceptance, without decode.
    const auto admitted = [](const std::vector<std::byte>& frame) {
        const superzip::BlockDescriptor outer{.kind = superzip::BlockKind::GpuBytePlaneContexts,
                                              .uncompressed_len = 128U,
                                              .encoded_len = static_cast<std::uint32_t>(frame.size())};
        try {
            (void)superzip::parse_gpu_byte_plane_contexts(frame, outer);
            return true;
        } catch (const superzip::ArchiveError&) {
            return false;
        }
    };
    for (std::uint16_t kind = 0U; kind < 256U; ++kind) {
        const auto value = static_cast<std::uint8_t>(kind);
        const bool expected = std::find(admitted_kinds.begin(), admitted_kinds.end(), value) != admitted_kinds.end();
        const auto bytes = kind == 0U ? 64U : kind == 1U ? 0U : 2U;
        REQUIRE_EQ(admitted(make_frame(value, 0U, bytes)), expected);
        if (expected && kind != 1U) {
            REQUIRE_TRUE(!admitted(make_frame(value, 255U, bytes)));
        }
    }
    for (const std::uint8_t fill : {0U, 1U, 255U}) {
        REQUIRE_TRUE(admitted(make_frame(1U, fill, 0U)));
        REQUIRE_TRUE(!admitted(make_frame(1U, fill, 1U)));
    }
    for (const std::uint32_t bytes : {0U, 1U, 63U, 65U}) {
        REQUIRE_TRUE(!admitted(make_frame(0U, 0U, bytes)));
    }
    for (const std::uint8_t kind : {3U, 4U, 5U, 6U, 7U, 9U, 10U}) {
        for (const std::uint32_t bytes : {0U, 64U, 65U}) {
            REQUIRE_TRUE(!admitted(make_frame(kind, 0U, bytes)));
        }
    }
}

// Purpose: Independently qualify dense context tables, mixed child codecs and every tail through CPU/HIP readers.
// Inputs: Handcrafted version-eleven frames at segment/block boundaries and all admitted widths.
// Outputs: Requires byte-exact reconstruction, actual HIP decoding and GPU CRC equal to the independent oracle.
TEST_CASE(suzip_byte_plane_context_independent_readers_and_crc) {
    for (const std::uint8_t width : {2U, 4U, 8U}) {
        for (const std::uint32_t base : {128U, 4096U, 65536U, 262144U}) {
            for (std::uint32_t tail = 0U; tail < width; ++tail) {
                const auto [frame, expected] = context_plane_fixture(width, base + tail);
                const superzip::BlockDescriptor outer{.kind = superzip::BlockKind::GpuBytePlaneContexts,
                                                      .uncompressed_len = base + tail,
                                                      .encoded_len = static_cast<std::uint32_t>(frame.size())};
                for (const bool hip : {false, true}) {
                    if (hip && !superzip::query_gpu_info().available) {
                        continue;
                    }
                    const superzip::GpuCodecOptions options{
                        .require_gpu = hip, .force_cpu = !hip, .telemetry = std::make_shared<superzip::GpuTelemetry>()};
                    std::vector<std::byte> decoded(expected.size());
                    (void)superzip::decode_chunk(frame, std::span(&outer, 1U), decoded, options);
                    REQUIRE_EQ(decoded, expected);
                    const auto checksum =
                        superzip::crc_decoded_chunk(frame, std::span(&outer, 1U), decoded.size(), options);
                    REQUIRE_EQ(checksum.crc32, superzip::crc32(expected));
                    REQUIRE_EQ(checksum.gpu_used, hip);
                    REQUIRE_EQ(superzip::snapshot_gpu_telemetry(*options.telemetry).kernel_launches > 0U, hip);
                }
            }
        }
    }
}

// Purpose: Refuse malformed context tables before any device work and preserve the explicit version boundary.
// Inputs: A valid independent frame mutated in header, stage kind/fill, extent, density and version.
// Outputs: Both readers/integrity consumers reject malformed bytes; only version eleven accepts the new kind.
TEST_CASE(suzip_byte_plane_context_malformed_and_version_admission) {
    const auto [frame, expected] = context_plane_fixture(2U, 128U);
    const superzip::BlockDescriptor outer{.kind = superzip::BlockKind::GpuBytePlaneContexts,
                                          .uncompressed_len = 128U,
                                          .encoded_len = static_cast<std::uint32_t>(frame.size())};
    for (const std::size_t location : {0U, 1U, 2U, 3U, 4U, 6U, 11U, 12U}) {
        auto changed = frame;
        changed[location] = location == 4U ? std::byte{13} : std::byte{255};
        require_byte_plane_rejection(changed, outer, 128U);
    }
    for (const std::uint8_t kind : {2U, 8U, 11U, 12U, 13U, 255U}) {
        auto changed = frame;
        changed[10] = static_cast<std::byte>(kind);
        require_byte_plane_rejection(changed, outer, 128U);
    }
    for (const std::size_t size : std::array<std::size_t, 5>{0U, 3U, 15U, frame.size() - 1U, frame.size() + 1U}) {
        auto changed = frame;
        changed.resize(size, std::byte{0});
        auto descriptor = outer;
        descriptor.encoded_len = static_cast<std::uint32_t>(changed.size());
        require_byte_plane_rejection(changed, descriptor, 128U);
    }
    const auto stages = superzip::parse_gpu_byte_plane_contexts(frame, outer);
    REQUIRE_EQ(stages.width, 2U);
    superzip::ArchiveIndex index;
    index.entries.push_back({.path = "context.bin",
                             .uncompressed_size = expected.size(),
                             .payload_size = frame.size(),
                             .crc32 = superzip::crc32(expected),
                             .blocks = {outer}});
    index.version = 11U;
    const auto root = test_temp_dir("plane-context-container");
    const auto archive = root / "context.suzip";
    {
        std::ofstream file(archive, std::ios::binary);
        file.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
        const auto offset = static_cast<std::uint64_t>(file.tellp());
        superzip::write_archive_index(file, index);
        superzip_test::write_test_footer(file, offset, static_cast<std::uint64_t>(file.tellp()) - offset,
                                         index.version);
    }
    REQUIRE_EQ(superzip::verify_suzip(archive, {.gpu_required = false, .force_cpu = true}).entries, 1U);
    REQUIRE_EQ(superzip::extract_suzip(archive, root / "restored", {.gpu_required = false, .force_cpu = true}).entries,
               1U);
    {
        std::ifstream file(root / "restored/context.bin", std::ios::binary);
        const std::string actual(std::istreambuf_iterator<char>(file), {});
        REQUIRE_EQ(actual, std::string(reinterpret_cast<const char*>(expected.data()), expected.size()));
    }
    if (superzip::query_gpu_info().available) {
        REQUIRE_TRUE(superzip::verify_suzip(archive, {.gpu_required = true}).gpu_used);
    }
    for (std::uint32_t version = 1U; version <= 11U; ++version) {
        index.version = version;
        std::ostringstream stream(std::ios::binary);
        bool rejected = false;
        try {
            superzip::write_archive_index(stream, index);
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_EQ(rejected, version < 11U);
        if (!rejected) {
            std::istringstream admitted(stream.str(), std::ios::binary);
            const auto parsed = superzip::read_archive_index(admitted);
            REQUIRE_EQ(parsed.version, 11U);
            REQUIRE_EQ(parsed.entries.front().blocks.front().kind, outer.kind);
            for (std::uint32_t older = 1U; older < 11U; ++older) {
                auto downgraded = stream.str();
                for (std::size_t byte = 0U; byte < sizeof(older); ++byte) {
                    downgraded[4U + byte] = static_cast<char>((older >> (byte * 8U)) & 255U);
                }
                std::istringstream input(downgraded, std::ios::binary);
                bool refused = false;
                try {
                    (void)superzip::read_archive_index(input);
                } catch (const superzip::ArchiveError&) {
                    refused = true;
                }
                REQUIRE_TRUE(refused);
            }
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Qualify actual context creation, independent transform bytes and isolation from ordinary efforts.
// Inputs: Whole interleaved integer fields, owned/borrowed HIP dispatch and independent CPU/HIP readers.
// Outputs: Requires a smaller context frame, original-byte readback, real telemetry and unchanged ordinary output.
TEST_CASE(suzip_neutron_plane_context_writer_gpu_only_and_isolated) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> input(65536U);
    std::uint32_t random = 0x97A491C3U;
    for (std::size_t offset = 0U; offset < input.size(); offset += 2U) {
        random ^= random << 13U;
        random ^= random >> 17U;
        random ^= random << 5U;
        input[offset] = static_cast<std::byte>(random & 255U);
        input[offset + 1U] = std::byte{0};
    }
    const superzip::GpuCodecOptions ordinary{.require_gpu = true, .block_size = 262144U, .compression_level = 9};
    const auto baseline = superzip::encode_chunk(input, ordinary);
    auto neutron = ordinary;
    neutron.compression_mode = superzip::NativeCompressionMode::NeutronStar;
    neutron.telemetry = std::make_shared<superzip::GpuTelemetry>();
    const auto encoded = superzip::encode_chunk(input, neutron);
    REQUIRE_EQ(encoded.blocks.size(), 1U);
    REQUIRE_EQ(encoded.blocks.front().kind, superzip::BlockKind::GpuBytePlaneContexts);
    REQUIRE_TRUE(encoded.payload.size() < baseline.payload.size());
    const auto stages = superzip::parse_gpu_byte_plane_contexts(encoded.payload, encoded.blocks.front());
    std::vector<std::byte> transformed(input.size());
    (void)superzip::decode_chunk(stages.payload, std::span(stages.blocks).first(stages.width), transformed,
                                 {.require_gpu = false, .force_cpu = true});
    REQUIRE_EQ(stages.width, 2U);
    for (std::size_t record = 0U; record < input.size() / 2U; ++record) {
        REQUIRE_EQ(transformed[record], input[record * 2U]);
        REQUIRE_EQ(transformed[input.size() / 2U + record], input[record * 2U + 1U]);
    }
    for (const bool hip : {false, true}) {
        std::vector<std::byte> decoded(input.size());
        (void)superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, {.require_gpu = hip, .force_cpu = !hip});
        REQUIRE_EQ(decoded, input);
    }
    REQUIRE_TRUE(superzip::snapshot_gpu_telemetry(*neutron.telemetry).kernel_launches > 0U);
    const auto owned = superzip::encode_owned_chunk(input, neutron);
    REQUIRE_EQ(owned.payload, encoded.payload);
    const auto after = superzip::encode_chunk(input, ordinary);
    REQUIRE_EQ(after.payload, baseline.payload);
    REQUIRE_EQ(after.blocks.front().kind, baseline.blocks.front().kind);
}

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
