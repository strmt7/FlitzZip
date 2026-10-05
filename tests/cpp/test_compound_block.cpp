#include "core/archive.hpp"
#include "core/checksum.hpp"
#include "core/compound_block.hpp"
#include "gpu/gpu_codec.hpp"
#include "test_suzip_helpers.hpp"
#include "test_util.hpp"
#include "lz4.h"

#include <array>
#include <fstream>
#include <limits>

namespace {

// Purpose: Supply independently framed compound payloads instead of testing only writer-produced bytes.
// Inputs: Fill, pattern or independently encoded LZ4 is the second stage; the first is a repeated motif.
// Outputs: Returns canonical version-nine bytes that expand to alternating A/B or all-A source bytes.
std::vector<std::byte> compound_fixture(superzip::BlockKind inner) {
    constexpr std::uint32_t intermediate_size = 1024U;
    std::vector<std::byte> payload{static_cast<std::byte>(inner),
                                   std::byte{3},
                                   inner == superzip::BlockKind::Fill ? std::byte{'A'} : std::byte{0},
                                   std::byte{0},
                                   std::byte{0},
                                   std::byte{4},
                                   std::byte{0},
                                   std::byte{0}};
    if (inner == superzip::BlockKind::Pattern) {
        payload.insert(payload.end(), {std::byte{'A'}, std::byte{'B'}});
    } else if (inner == superzip::BlockKind::GpuDictionary) {
        std::vector<char> motif(intermediate_size);
        for (std::size_t byte = 0U; byte < motif.size(); ++byte) {
            motif[byte] = byte % 2U == 0U ? 'A' : 'B';
        }
        std::vector<char> compressed(LZ4_compressBound(static_cast<int>(motif.size())));
        const auto bytes = LZ4_compress_default(motif.data(), compressed.data(), static_cast<int>(motif.size()),
                                                static_cast<int>(compressed.size()));
        REQUIRE_TRUE(bytes > 0);
        for (const std::uint32_t offset : {0U, static_cast<std::uint32_t>(bytes)}) {
            for (std::size_t byte = 0U; byte < sizeof(offset); ++byte) {
                payload.push_back(static_cast<std::byte>(offset >> (byte * 8U)));
            }
        }
        for (int byte = 0; byte < bytes; ++byte) {
            payload.push_back(static_cast<std::byte>(compressed[byte]));
        }
    }
    return payload;
}

// Purpose: Describe the exact independently framed fixture output.
// Inputs: Complete fixture bytes, never an untrusted external allocation request.
// Outputs: Returns one bounded GPU compound descriptor.
superzip::BlockDescriptor compound_descriptor(std::span<const std::byte> payload) {
    return {.kind = superzip::BlockKind::GpuCompound,
            .uncompressed_len = 4096U,
            .encoded_len = static_cast<std::uint32_t>(payload.size())};
}

}  // namespace

// Purpose: Independently exercise both-stage CPU/HIP decoding and GPU integrity for every admitted fixture.
// Inputs: Handcrafted fill/pattern/LZ4 compositions plus mixed raw/fill layouts and actual device availability.
// Outputs: Requires byte-exact CPU output, real HIP telemetry and equal GPU CRC when HIP is present.
TEST_CASE(suzip_compound_independent_readers_and_gpu_crc) {
    for (const auto inner :
         {superzip::BlockKind::Fill, superzip::BlockKind::Pattern, superzip::BlockKind::GpuDictionary}) {
        auto payload = compound_fixture(inner);
        const auto compound = compound_descriptor(payload);
        payload.push_back(std::byte{'Z'});
        const std::array blocks{compound,
                                superzip::BlockDescriptor{.kind = superzip::BlockKind::Raw,
                                                          .uncompressed_len = 1U,
                                                          .encoded_offset = compound.encoded_len,
                                                          .encoded_len = 1U},
                                superzip::BlockDescriptor{.kind = superzip::BlockKind::Fill,
                                                          .fill_value = 'Q',
                                                          .uncompressed_len = 3U,
                                                          .encoded_offset = std::numeric_limits<std::uint64_t>::max()}};
        std::vector<std::byte> expected(4100U, std::byte{'A'});
        if (inner != superzip::BlockKind::Fill) {
            for (std::size_t byte = 1U; byte < 4096U; byte += 2U) {
                expected[byte] = std::byte{'B'};
            }
        }
        expected[4096] = std::byte{'Z'};
        std::fill(expected.begin() + 4097, expected.end(), std::byte{'Q'});
        for (const bool hip : {false, true}) {
            if (hip && !superzip::query_gpu_info().available) {
                continue;
            }
            superzip::GpuCodecOptions options{
                .require_gpu = hip, .force_cpu = !hip, .telemetry = std::make_shared<superzip::GpuTelemetry>()};
            std::vector<std::byte> decoded(expected.size());
            REQUIRE_EQ(superzip::decode_chunk(payload, blocks, decoded, options), hip);
            REQUIRE_EQ(decoded, expected);
            const auto verified = superzip::crc_decoded_chunk(payload, blocks, expected.size(), options);
            REQUIRE_EQ(verified.crc32, superzip::crc32(expected));
            REQUIRE_EQ(verified.gpu_used, hip);
            const auto stats = superzip::snapshot_gpu_telemetry(*options.telemetry);
            REQUIRE_EQ(stats.kernel_launches > 0U, hip);
        }
    }
}

// Purpose: Reject recursive/CPU/unknown stages, bad extents and noncanonical headers before HIP dispatch.
// Inputs: Independently mutated fixture headers, every kind byte, and the bounded public codec entry point.
// Outputs: Every invalid case throws and leaves device telemetry at zero.
TEST_CASE(suzip_compound_rejects_malformed_framing_before_device_work) {
    const auto valid = compound_fixture(superzip::BlockKind::Pattern);
    std::vector<std::vector<std::byte>> malformed;
    for (std::size_t bytes = 0U; bytes < superzip::kGpuCompoundHeaderBytes; ++bytes) {
        malformed.emplace_back(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(bytes));
    }
    for (unsigned int byte = 0U; byte < 256U; ++byte) {
        const auto kind = static_cast<superzip::BlockKind>(byte);
        if (!superzip::is_gpu_compound_stage(kind) && kind != superzip::BlockKind::Fill) {
            auto changed = valid;
            changed[0] = static_cast<std::byte>(byte);
            malformed.push_back(changed);
        }
        if (!superzip::is_gpu_compound_stage(kind)) {
            auto changed = valid;
            changed[1] = static_cast<std::byte>(byte);
            malformed.push_back(changed);
        }
    }
    for (const std::size_t offset : {2U, 3U}) {
        auto changed = valid;
        changed[offset] = std::byte{1};
        malformed.push_back(changed);
    }
    for (const std::uint32_t extent : {0U, 8U, 10U, 4096U, 0xFFFFFFFFU}) {
        auto changed = valid;
        for (std::size_t byte = 0U; byte < sizeof(extent); ++byte) {
            changed[4U + byte] = static_cast<std::byte>(extent >> (byte * 8U));
        }
        malformed.push_back(changed);
    }
    const bool hip_available = superzip::query_gpu_info().available;
    for (const auto& payload : malformed) {
        const auto descriptor = compound_descriptor(payload);
        for (const bool hip : {false, true}) {
            if (hip && !hip_available) {
                continue;
            }
            superzip::GpuCodecOptions options{
                .require_gpu = hip, .force_cpu = !hip, .telemetry = std::make_shared<superzip::GpuTelemetry>()};
            std::vector<std::byte> output(4096U);
            bool rejected = false;
            try {
                (void)superzip::decode_chunk(payload, std::span(&descriptor, 1U), output, options);
            } catch (const superzip::ArchiveError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
            REQUIRE_EQ(superzip::snapshot_gpu_telemetry(*options.telemetry).kernel_launches, 0U);
        }
    }
}

// Purpose: Exercise real version-nine container verification, detection and CPU extraction.
// Inputs: A handcrafted compound file with independently computed source CRC and exact index/footer versions.
// Outputs: Requires public container consumers to recover the source and required HIP to verify it when present.
TEST_CASE(suzip_compound_container_readback) {
    using namespace superzip_test;
    const auto root = test_temp_dir("compound-container");
    const auto archive = root / "data.suzip";
    const auto payload = compound_fixture(superzip::BlockKind::Fill);
    const std::vector<std::byte> expected(4096U, std::byte{'A'});
    {
        std::ofstream file(archive, std::ios::binary);
        file.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        superzip::ArchiveIndex index;
        index.version = 9U;
        index.entries.push_back({.path = "data.bin",
                                 .uncompressed_size = expected.size(),
                                 .payload_size = payload.size(),
                                 .crc32 = superzip::crc32(expected),
                                 .blocks = {compound_descriptor(payload)}});
        const auto offset = static_cast<std::uint64_t>(file.tellp());
        superzip::write_archive_index(file, index);
        write_test_footer(file, offset, static_cast<std::uint64_t>(file.tellp()) - offset, index.version);
    }
    const superzip::ExtractOptions cpu{.gpu_required = false, .force_cpu = true};
    REQUIRE_EQ(superzip::verify_suzip(archive, cpu).entries, 1U);
    REQUIRE_EQ(superzip::extract_suzip(archive, root / "restored", cpu).entries, 1U);
    std::ifstream restored(root / "restored" / "data.bin", std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(restored), {}), std::string(4096U, 'A'));
    restored.close();
    if (superzip::query_gpu_info().available) {
        const auto verified = superzip::verify_suzip(archive, {.gpu_required = true});
        REQUIRE_TRUE(verified.gpu_used);
        REQUIRE_TRUE(verified.gpu_runtime.kernel_launches > 0U);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Verify production Neutron preserves numeric-level results whether its best result uses composition or not.
// Inputs: A deterministic bounded low-alphabet motif and actual required-HIP creation, decode and CRC paths.
// Outputs: Requires a strictly smaller winner, byte-exact dual readback and deterministic ordinary bytes.
TEST_CASE(suzip_neutron_composition_writer_is_gpu_only_and_isolated) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> input(65536U);
    std::uint32_t random = 0x37A84E91U;
    for (std::size_t index = 0U; index < input.size(); ++index) {
        random ^= random << 13U;
        random ^= random >> 17U;
        random ^= random << 5U;
        input[index] = index < 2048U ? static_cast<std::byte>(random & 3U) : input[index % 2048U];
    }
    const superzip::GpuCodecOptions ordinary{.require_gpu = true, .block_size = 262144U, .compression_level = 9};
    const auto baseline = superzip::encode_chunk(input, ordinary);
    auto neutron = ordinary;
    neutron.compression_mode = superzip::NativeCompressionMode::NeutronStar;
    neutron.telemetry = std::make_shared<superzip::GpuTelemetry>();
    const auto encoded = superzip::encode_chunk(input, neutron);
    REQUIRE_TRUE(encoded.payload.size() < baseline.payload.size());
    REQUIRE_TRUE(superzip::snapshot_gpu_telemetry(*neutron.telemetry).kernel_launches > 0U);
    for (const bool hip : {false, true}) {
        std::vector<std::byte> decoded(input.size());
        superzip::GpuCodecOptions options{.require_gpu = hip, .force_cpu = !hip};
        REQUIRE_EQ(superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options), hip);
        REQUIRE_EQ(decoded, input);
        REQUIRE_EQ(superzip::crc_decoded_chunk(encoded.payload, encoded.blocks, input.size(), options).crc32,
                   superzip::crc32(input));
    }
    const auto after = superzip::encode_chunk(input, ordinary);
    REQUIRE_EQ(after.payload, baseline.payload);
    REQUIRE_EQ(after.blocks.size(), baseline.blocks.size());
    for (std::size_t index = 0U; index < after.blocks.size(); ++index) {
        REQUIRE_EQ(after.blocks[index].kind, baseline.blocks[index].kind);
        REQUIRE_EQ(after.blocks[index].encoded_len, baseline.blocks[index].encoded_len);
    }
    int checkpoints = 0;
    neutron.encode_checkpoint = [&] {
        if (++checkpoints >= 2) {
            throw superzip::ArchiveError("cancel composition test");
        }
    };
    bool cancelled = false;
    try {
        (void)superzip::encode_chunk(input, neutron);
    } catch (const superzip::ArchiveError&) {
        cancelled = true;
    }
    REQUIRE_TRUE(cancelled);
    REQUIRE_EQ(superzip::encode_chunk(input, ordinary).payload, baseline.payload);
}
