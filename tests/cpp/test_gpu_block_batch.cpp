#include "test_util.hpp"
#include "test_suzip_helpers.hpp"

#include "core/archive_encode_batch.hpp"
#include "core/checksum.hpp"
#include "core/result.hpp"
#include "gpu/gpu_codec.hpp"

#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>

namespace {

// Purpose: Generate unequal independent blocks covering codec and CRC boundaries without filesystem I/O.
// Inputs: lengths select positive block sizes; each position selects a deterministic content family.
// Outputs: Returns dense fill, pattern, prefix, random, and misleading sampled-pattern bytes.
std::vector<std::byte> batch_fixture(std::span<const std::uint32_t> lengths) {
    std::vector<std::byte> input;
    std::uint32_t random = 0x98765432U;
    for (std::size_t block = 0; block < lengths.size(); ++block) {
        for (std::uint32_t i = 0; i < lengths[block]; ++i) {
            random ^= random << 13U;
            random ^= random >> 17U;
            random ^= random << 5U;
            const auto family = block % 6U;
            const auto value = family == 0   ? 73U
                               : family == 1 ? i % 7U
                               : family == 2 ? random % 5U
                               : family == 3 ? 193U + random % 5U
                               : family == 4 ? random % 256U
                                             : (i < 8192U ? i % 3U : random % 256U);
            input.push_back(static_cast<std::byte>(value));
        }
    }
    return input;
}

// Purpose: Compare batched descriptors, bytes, and checksums with separately encoded independent blocks.
// Inputs: lengths and options select bounded RAM fixtures and an explicit CPU or required-HIP backend.
// Outputs: Requires exact encoding identity and byte-exact decoding through every applicable backend.
void check_batch_identity(std::span<const std::uint32_t> lengths, superzip::GpuCodecOptions options) {
    const auto input = batch_fixture(lengths);
    const auto batch = superzip::encode_owned_block_batch(input, lengths, options);
    REQUIRE_EQ(batch.encoded.blocks.size(), lengths.size());
    REQUIRE_EQ(batch.block_crc32.size(), lengths.size());
    REQUIRE_TRUE(batch.encoded.source_crc32_available);
    REQUIRE_EQ(batch.encoded.source_crc32, superzip::crc32(input));
    REQUIRE_EQ(batch.encoded.gpu_used, !options.force_cpu && !input.empty());
    std::size_t source_offset = 0;
    std::size_t payload_offset = 0;
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        const auto source = std::span(input).subspan(source_offset, lengths[i]);
        const auto separate =
            superzip::encode_owned_chunk(std::vector<std::byte>(source.begin(), source.end()), options);
        REQUIRE_EQ(separate.blocks.size(), 1U);
        const auto& expected = separate.blocks.front();
        const auto& actual = batch.encoded.blocks[i];
        REQUIRE_EQ(actual.kind, expected.kind);
        REQUIRE_EQ(actual.fill_value, expected.fill_value);
        REQUIRE_EQ(actual.uncompressed_len, expected.uncompressed_len);
        REQUIRE_EQ(actual.encoded_len, expected.encoded_len);
        REQUIRE_EQ(actual.encoded_offset, payload_offset);
        const auto payload = std::span(batch.encoded.payload).subspan(payload_offset, actual.encoded_len);
        REQUIRE_TRUE(std::ranges::equal(payload, separate.payload));
        REQUIRE_EQ(batch.block_crc32[i], superzip::crc32(source));
        REQUIRE_EQ(batch.block_crc32[i], separate.source_crc32);
        source_offset += lengths[i];
        payload_offset += actual.encoded_len;
    }
    REQUIRE_EQ(payload_offset, batch.encoded.payload.size());
    std::vector<std::byte> decoded(input.size());
    if (!options.force_cpu) {
        superzip::decode_chunk(batch.encoded.payload, batch.encoded.blocks, decoded, options);
        REQUIRE_TRUE(decoded == input);
    }
    options.require_gpu = false;
    options.force_cpu = true;
    superzip::decode_chunk(batch.encoded.payload, batch.encoded.blocks, decoded, options);
    REQUIRE_TRUE(decoded == input);
}

// Purpose: Reject invalid layouts before any GPU launch or transfer.
// Inputs: size and lengths describe one intentionally invalid independent-block layout.
// Outputs: Requires ArchiveError and unchanged dispatch counters.
void require_invalid_batch(std::size_t size, std::span<const std::uint32_t> lengths) {
    superzip::GpuCodecOptions options;
    options.block_size = superzip::kMinArchiveBlockBytes;
    options.telemetry = std::make_shared<superzip::GpuTelemetry>();
    bool rejected = false;
    try {
        (void)superzip::encode_owned_block_batch(std::vector<std::byte>(size), lengths, options);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    const auto counters = superzip::snapshot_gpu_telemetry(*options.telemetry);
    REQUIRE_EQ(counters.encode_chunks, 0U);
    REQUIRE_EQ(counters.kernel_launches, 0U);
    REQUIRE_EQ(counters.h2d_bytes, 0U);
}

}  // namespace

// Purpose: Preserve per-block CPU codec policy and CRCs at all nine effort settings.
// Inputs: Unequal RAM-only blocks including segment tails, prefix alphabets, fill, and raw data.
// Outputs: Requires identical payloads to separate encoding and exact CPU decode.
TEST_CASE(gpu_block_batch_cpu_identity) {
    constexpr std::array<std::uint32_t, 12> lengths{1, 31,    4095,  4096,   4097,   65537,
                                                    2, 65535, 65536, 131073, 262144, 65539};
    for (int level = 1; level <= 9; ++level) {
        check_batch_identity(
            lengths, {.require_gpu = false, .force_cpu = true, .block_size = 256U * 1024U, .compression_level = level});
    }
}

// Purpose: Prove unequal HIP blocks retain independent encoding and device CRC semantics.
// Inputs: All effort settings and every product block-size option on an available HIP device.
// Outputs: Requires exact separate-encode bytes plus independent CPU and HIP roundtrips; absent HIP skips this case.
TEST_CASE(gpu_block_batch_hip_identity) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    constexpr std::array<std::uint32_t, 12> lengths{1, 31,    4095,  4096,   4097,   65537,
                                                    2, 65535, 65536, 131073, 262144, 65539};
    for (int level = 1; level <= 9; ++level) {
        check_batch_identity(lengths, {.block_size = 256U * 1024U, .compression_level = level});
    }
    for (std::uint32_t size = 256U * 1024U; size <= superzip::kMaxArchiveBlockBytes; size *= 2U) {
        const std::array<std::uint32_t, 4> edges{3, size - 1U, size, 65537};
        check_batch_identity(edges, {.block_size = size, .compression_level = 9});
    }
}

// Purpose: Exercise empty/max-count batches and reject inconsistent layout or backend requirements.
// Inputs: RAM-only bounded valid and invalid length tables; no GPU is required for validation checks.
// Outputs: Requires exact coverage admission, no dispatch on invalid layouts, and explicit policy conflict rejection.
TEST_CASE(gpu_block_batch_layout_validation) {
    check_batch_identity({}, {.require_gpu = false, .force_cpu = true});
    const std::vector<std::uint32_t> maximum(superzip::kMaxEncodeBatchBlocks, 1U);
    check_batch_identity(maximum, {.require_gpu = false, .force_cpu = true});
    if (superzip::query_gpu_info().available) {
        check_batch_identity(maximum, {});
    }
    require_invalid_batch(1, {});
    for (const auto lengths : {std::vector<std::uint32_t>{0}, {2}, {1, 1}, {1, 0}, {0, 1}}) {
        require_invalid_batch(1, lengths);
    }
    require_invalid_batch(0, std::array<std::uint32_t, 1>{1});
    require_invalid_batch(2, std::array<std::uint32_t, 1>{1});
    require_invalid_batch(superzip::kMinArchiveBlockBytes + 1U,
                          std::array<std::uint32_t, 1>{superzip::kMinArchiveBlockBytes + 1U});
    require_invalid_batch(257, std::vector<std::uint32_t>(257, 1U));
    bool rejected = false;
    try {
        (void)superzip::encode_owned_block_batch({}, {}, {.require_gpu = true, .force_cpu = true});
    } catch (const superzip::GpuError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Bound production grouping by count, bytes, file kind, and selected CPU policy.
// Inputs: Synthetic manifest metadata only, including exact and just-over byte caps.
// Outputs: Requires deterministic grouping without reading files or launching HIP work.
TEST_CASE(gpu_block_batch_archive_policy) {
    superzip::CompressOptions options;
    options.block_size = 256U * 1024U;
    options.chunk_size = 8U * 1024U * 1024U;
    std::vector<superzip::ManifestEntry> entries(70);
    for (auto& entry : entries) {
        entry.size = 4096;
    }
    REQUIRE_EQ(superzip::archive_encode_batch_count(entries, options), 64U);
    options.force_cpu = true;
    REQUIRE_EQ(superzip::archive_encode_batch_count(entries, options), 0U);
    options.force_cpu = false;
    for (auto& entry : entries) {
        entry.size = options.block_size;
    }
    REQUIRE_EQ(superzip::archive_encode_batch_count(entries, options), 32U);
    options.chunk_size = 2U * options.block_size;
    REQUIRE_EQ(superzip::archive_encode_batch_count(entries, options), 2U);
    options.chunk_size -= 1;
    REQUIRE_EQ(superzip::archive_encode_batch_count(entries, options), 0U);
    options.chunk_size = 8U * 1024U * 1024U;
    entries[2].directory = true;
    REQUIRE_EQ(superzip::archive_encode_batch_count(entries, options), 2U);
    entries[2].directory = false;
    entries[2].size = 0;
    REQUIRE_EQ(superzip::archive_encode_batch_count(entries, options), 2U);
    entries[2].size = options.block_size + 1U;
    REQUIRE_EQ(superzip::archive_encode_batch_count(entries, options), 2U);
    entries[1].size = 0;
    REQUIRE_EQ(superzip::archive_encode_batch_count(entries, options), 0U);
}

namespace {

// Purpose: Independently serialize per-file encoding as a byte-level oracle for the batched archive scheduler.
// Inputs: manifest points to bounded fixture files; options selects the same backend/effort as production.
// Outputs: Returns a complete version-three archive built without the production batch writer.
std::string separate_archive_bytes(const superzip::Manifest& manifest, const superzip::GpuCodecOptions& options) {
    superzip::ArchiveIndex index;
    std::ostringstream output(std::ios::binary);
    for (const auto& source : manifest.entries) {
        superzip::ArchiveEntry entry{.path = source.archive_path,
                                     .directory = source.directory,
                                     .uncompressed_size = source.size,
                                     .payload_offset = static_cast<std::uint64_t>(output.tellp())};
        if (!source.directory) {
            std::vector<std::byte> bytes(static_cast<std::size_t>(source.size));
            std::ifstream input(source.source_path, std::ios::binary);
            input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            REQUIRE_EQ(input.gcount(), static_cast<std::streamsize>(bytes.size()));
            const auto encoded = superzip::encode_owned_chunk(std::move(bytes), options);
            entry.blocks = encoded.blocks;
            entry.crc32 = encoded.source_crc32;
            entry.payload_size = encoded.payload.size();
            if (!encoded.payload.empty()) {
                output.write(reinterpret_cast<const char*>(encoded.payload.data()),
                             static_cast<std::streamsize>(encoded.payload.size()));
            }
        }
        index.entries.push_back(std::move(entry));
    }
    const auto offset = static_cast<std::uint64_t>(output.tellp());
    superzip::write_archive_index(output, index);
    const auto size = static_cast<std::uint64_t>(output.tellp()) - offset;
    superzip_test::write_test_footer(output, offset, size);
    return output.str();
}

// Purpose: Populate deterministic small, empty, and larger files for production scheduler transition tests.
// Inputs: root is an isolated temporary fixture directory; payload stays below two MiB.
// Outputs: Creates 73 regular files and a directory boundary, with no timing measurement.
void write_archive_batch_fixture(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "nested");
    std::vector<std::uint32_t> lengths(70);
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        lengths[i] = static_cast<std::uint32_t>(4093U + i);
    }
    const auto bytes = batch_fixture(lengths);
    std::size_t offset = 0;
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        std::ofstream file(root / ("file-" + std::to_string(100U + i)), std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data() + offset), lengths[i]);
        REQUIRE_TRUE(file.good());
        offset += lengths[i];
    }
    std::ofstream(root / "file-135-empty", std::ios::binary);
    const std::vector<char> large(256U * 1024U + 1U, 'x');
    std::ofstream file(root / "file-145-large", std::ios::binary);
    file.write(large.data(), static_cast<std::streamsize>(large.size()));
    REQUIRE_TRUE(file.good());
    std::ofstream(root / "nested" / "empty", std::ios::binary);
}

}  // namespace

// Purpose: Validate production batching against complete independently serialized archives and both readers.
// Inputs: Small-file fixtures crossing empty/large/directory boundaries and two HIP effort tiers.
// Outputs: Requires full archive byte identity, fewer encode submissions, progress totals, and exact extracted files.
TEST_CASE(gpu_block_batch_archive_roundtrip) {
    const bool gpu = superzip::query_gpu_info().available;
    const auto root = test_temp_dir("block-batch-archive");
    write_archive_batch_fixture(root / "source");
    const auto manifest = superzip::build_manifest({root / "source"});
    for (const int level : {5, 9}) {
        superzip::CompressOptions options;
        options.gpu_required = gpu;
        options.block_size = 256U * 1024U;
        options.compression_level = level;
        superzip::ProgressSnapshot last;
        const auto archive = root / "batch.suzip";
        const auto stats =
            superzip::compress_suzip({root / "source"}, archive, options,
                                     [&last](const superzip::ProgressSnapshot& progress) { last = progress; });
        const auto reference = separate_archive_bytes(
            manifest, {.require_gpu = gpu, .block_size = options.block_size, .compression_level = level});
        std::ifstream actual(archive, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(actual)), std::istreambuf_iterator<char>());
        REQUIRE_EQ(bytes, reference);
        REQUIRE_EQ(last.processed_bytes, manifest.total_file_bytes);
        REQUIRE_EQ(last.completed_entries, manifest.entries.size());
        REQUIRE_EQ(stats.gpu_used, gpu);
        if (gpu) {
            REQUIRE_TRUE(stats.gpu_runtime.encode_chunks < 10U);
            superzip::verify_suzip(archive, {.gpu_required = true});
        }
        superzip::extract_suzip(archive, root / "extracted",
                                {.gpu_required = false, .force_cpu = true, .overwrite = true});
        for (const auto& entry : manifest.entries) {
            if (!entry.directory) {
                REQUIRE_TRUE(
                    superzip_test::files_are_equal(entry.source_path, root / "extracted" / entry.archive_path));
            }
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Preserve the old destination and release all pinned sources when a batch is interrupted.
// Inputs: An isolated fixture, existing destination sentinel, and a callback throwing after the first source read.
// Outputs: Requires exact sentinel preservation, no staging file, and unlocked source files after failure.
TEST_CASE(gpu_block_batch_archive_abort) {
    const auto root = test_temp_dir("block-batch-abort");
    write_archive_batch_fixture(root / "source");
    const auto archive = root / "batch.suzip";
    {
        std::ofstream old(archive, std::ios::binary);
        old << "sentinel";
    }
    bool interrupted = false;
    try {
        superzip::compress_suzip({root / "source"}, archive, {.gpu_required = false},
                                 [](const superzip::ProgressSnapshot& progress) {
                                     if (progress.processed_bytes != 0) {
                                         throw superzip::ArchiveError("test batch interruption");
                                     }
                                 });
    } catch (const superzip::ArchiveError& error) {
        interrupted = std::string(error.what()) == "test batch interruption";
    }
    REQUIRE_TRUE(interrupted);
    {
        std::ifstream old(archive, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(old)), std::istreambuf_iterator<char>());
        REQUIRE_EQ(bytes, "sentinel");
    }
    REQUIRE_EQ(superzip_test::count_regular_files(root), 74U);
    std::filesystem::remove_all(root);
    REQUIRE_TRUE(!std::filesystem::exists(root));
}

namespace {

struct BatchTiming {
    double milliseconds = 0;
    std::size_t payload_bytes = 0;
    std::uint32_t checksum = 0;
    superzip::GpuRuntimeStats telemetry;
};

// Purpose: Time the production batch API or its unchanged per-file baseline with equal input-copy accounting.
// Inputs: input/lengths are RAM-only immutable fixtures; level and batched select equal-effort HIP submissions.
// Outputs: Returns wall time, exact payload size/combined CRC, and real HIP counters without writing payload files.
BatchTiming time_batch_encoding(const std::vector<std::byte>& input, std::span<const std::uint32_t> lengths, int level,
                                bool batched) {
    superzip::GpuCodecOptions options;
    options.compression_level = level;
    options.block_size = 256U * 1024U;
    options.telemetry = std::make_shared<superzip::GpuTelemetry>();
    BatchTiming timing;
    const auto start = std::chrono::steady_clock::now();
    if (batched) {
        const auto result = superzip::encode_owned_block_batch(input, lengths, options);
        REQUIRE_TRUE(result.encoded.gpu_used);
        timing.payload_bytes = result.encoded.payload.size();
        timing.checksum = result.encoded.source_crc32;
    } else {
        std::size_t offset = 0;
        for (const auto length : lengths) {
            const auto bytes = std::span(input).subspan(offset, length);
            const auto result =
                superzip::encode_owned_chunk(std::vector<std::byte>(bytes.begin(), bytes.end()), options);
            REQUIRE_TRUE(result.gpu_used);
            timing.payload_bytes += result.payload.size();
            timing.checksum = superzip::crc32_combine(timing.checksum, result.source_crc32, length);
            offset += length;
        }
    }
    timing.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    timing.telemetry = superzip::snapshot_gpu_telemetry(*options.telemetry);
    return timing;
}

// Purpose: Emit one parseable measured sample with explicit scope and real execution evidence.
// Inputs: timing, input size, profile, level, round, and mode identify one warmed equal-effort RAM run.
// Outputs: Writes one stdout record; does not claim filesystem throughput or alter codec payloads.
void print_batch_timing(const BatchTiming& timing, std::size_t input_size, const char* profile, int level, int round,
                        bool batched) {
    std::cout << "batch_benchmark mode=" << (batched ? "batch" : "separate") << " profile=" << profile
              << " level=" << level << " round=" << round << " files=64 block_size=262144 input_bytes=" << input_size
              << " payload_bytes=" << timing.payload_bytes
              << " ratio=" << static_cast<double>(input_size) / timing.payload_bytes
              << " encode_wall_ms=" << timing.milliseconds << " encode_chunks=" << timing.telemetry.encode_chunks
              << " kernel_launches=" << timing.telemetry.kernel_launches << " h2d_bytes=" << timing.telemetry.h2d_bytes
              << " d2h_bytes=" << timing.telemetry.d2h_bytes
              << " allocation_bytes=" << timing.telemetry.device_allocation_bytes
              << " kernel_ms=" << timing.telemetry.kernel_ms
              << " gpu_used=true timing_scope=host_encode memory_only=true disk_write_bytes=0\n";
}

}  // namespace

// Purpose: Compare the production batch API with separate encoding in repeated alternating RAM-only runs.
// Inputs: SUPERZIP_BATCH_BENCHMARK=1 explicitly opts in under an external host-resource monitor.
// Outputs: Prints six paired samples per profile/size/effort after warmup; requires exact sizes and CRC identity.
TEST_CASE(gpu_block_batch_benchmark_opt_in) {
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"SUPERZIP_BATCH_BENCHMARK", enabled, 2U) != 1U || enabled[0] != L'1') {
        return;
    }
    REQUIRE_TRUE(superzip::query_gpu_info().available);
    for (const auto size : {4096U, 65536U}) {
        std::vector<std::uint32_t> lengths(64);
        for (std::size_t i = 0; i < lengths.size(); ++i) {
            lengths[i] = size - 31U + static_cast<std::uint32_t>(i);
        }
        for (const auto profile : {0, 1, 2}) {
            auto input = batch_fixture(lengths);
            if (profile != 0) {
                std::uint32_t state = 0x6347218BU;
                for (auto& byte : input) {
                    state ^= state << 13U;
                    state ^= state >> 17U;
                    state ^= state << 5U;
                    byte = static_cast<std::byte>(profile == 1 ? state % 256U : 193U + state % 5U);
                }
            }
            const char* label = profile == 0 ? "Mixed" : profile == 1 ? "Random" : "ShiftedAlphabet";
            for (const int level : {5, 9}) {
                (void)time_batch_encoding(input, lengths, level, false);
                (void)time_batch_encoding(input, lengths, level, true);
                for (int round = 0; round < 6; ++round) {
                    std::array<BatchTiming, 2> results;
                    for (const bool second : {false, true}) {
                        const bool batched = second != (round % 2 != 0);
                        results[batched ? 1 : 0] = time_batch_encoding(input, lengths, level, batched);
                        print_batch_timing(results[batched ? 1 : 0], input.size(), label, level, round, batched);
                    }
                    REQUIRE_EQ(results[0].payload_bytes, results[1].payload_bytes);
                    REQUIRE_EQ(results[0].checksum, results[1].checksum);
                }
            }
        }
    }
}
