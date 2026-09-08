#include "test_util.hpp"

#include "gpu/gpu_codec.hpp"
#include "core/checksum.hpp"

#include <array>
#include <cmath>
#include <future>
#include <limits>
#include <thread>

// Purpose: Preserve finite timing precision and distinguish real zero duration from unavailable data.
// Inputs: Synthetic event durations; no HIP device or workload is required.
// Outputs: Requires nearest-microsecond rounding, correct launch counts, and a finite empty snapshot.
TEST_CASE(gpu_telemetry_finite_event_rounding) {
    superzip::GpuTelemetry telemetry;
    REQUIRE_EQ(superzip::snapshot_gpu_telemetry(telemetry).kernel_ms, 0.0);
    for (const auto value : {0.0, -0.0, 0.00049, 0.00050, 1.234}) {
        superzip::record_gpu_kernel_launch(&telemetry, value);
    }
    const auto stats = superzip::snapshot_gpu_telemetry(telemetry);
    REQUIRE_EQ(stats.kernel_launches, 5U);
    REQUIRE_EQ(telemetry.kernel_microseconds.load(), 1235U);
    REQUIRE_EQ(stats.kernel_ms, 1.235);
    superzip::record_gpu_kernel_launch(nullptr, std::numeric_limits<double>::quiet_NaN());
}

// Purpose: Keep invalid HIP event values out of integer conversion and preserve unavailable timing thereafter.
// Inputs: Negative, non-finite, and out-of-range synthetic durations followed by a valid duration.
// Outputs: Requires NaN timing, intact execution counters, and a sticky unavailable marker without throwing.
TEST_CASE(gpu_telemetry_invalid_events_remain_unavailable) {
    for (const auto value :
         {-0.000001, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
          -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::max(), std::ldexp(1.0, 64) / 1000.0}) {
        superzip::GpuTelemetry telemetry;
        superzip::record_gpu_encode_chunk(&telemetry);
        superzip::record_gpu_kernel_launch(&telemetry, 1.0);
        superzip::record_gpu_kernel_launch(&telemetry, value);
        superzip::record_gpu_kernel_launch(&telemetry, 2.0);
        const auto stats = superzip::snapshot_gpu_telemetry(telemetry);
        REQUIRE_EQ(stats.encode_chunks, 1U);
        REQUIRE_EQ(stats.kernel_launches, 3U);
        REQUIRE_TRUE(std::isnan(stats.kernel_ms));
        REQUIRE_EQ(telemetry.kernel_microseconds.load(), std::numeric_limits<std::uint64_t>::max());
    }
}

// Purpose: Protect totals when individually valid times exceed the accumulator's representable range.
// Inputs: A synthetic near-limit counter and durations crossing its reserved unavailable value.
// Outputs: Requires no wraparound and permanent unavailable timing after overflow.
TEST_CASE(gpu_telemetry_total_overflow_is_unavailable) {
    superzip::GpuTelemetry telemetry;
    telemetry.kernel_microseconds.store(std::numeric_limits<std::uint64_t>::max() - 10U);
    superzip::record_gpu_kernel_launch(&telemetry, 0.009);
    REQUIRE_TRUE(std::isfinite(superzip::snapshot_gpu_telemetry(telemetry).kernel_ms));
    superzip::record_gpu_kernel_launch(&telemetry, 0.001);
    REQUIRE_TRUE(std::isnan(superzip::snapshot_gpu_telemetry(telemetry).kernel_ms));
    superzip::record_gpu_kernel_launch(&telemetry, 1.0);
    REQUIRE_EQ(telemetry.kernel_microseconds.load(), std::numeric_limits<std::uint64_t>::max());
}

// Purpose: Exercise atomic accumulation and invalidation with bounded concurrent writers.
// Inputs: Four joined CPU threads, each recording 4096 synthetic one-microsecond events.
// Outputs: Requires exact valid sums and no lost unavailable state when another writer invalidates the total.
TEST_CASE(gpu_telemetry_concurrent_accumulation) {
    for (const bool invalidate : {false, true}) {
        superzip::GpuTelemetry telemetry;
        {
            std::vector<std::jthread> workers;
            for (int worker = 0; worker < 4; ++worker) {
                workers.emplace_back([&telemetry, worker, invalidate] {
                    for (int event = 0; event < 4096; ++event) {
                        superzip::record_gpu_kernel_launch(&telemetry, 0.001);
                    }
                    if (invalidate && worker == 0) {
                        superzip::record_gpu_kernel_launch(&telemetry, -0.01);
                    }
                });
            }
        }
        const auto stats = superzip::snapshot_gpu_telemetry(telemetry);
        REQUIRE_EQ(stats.kernel_launches, 16384U + (invalidate ? 1U : 0U));
        if (invalidate) {
            REQUIRE_TRUE(std::isnan(stats.kernel_ms));
        } else {
            REQUIRE_EQ(stats.kernel_ms, 16.384);
        }
    }
}

namespace {

// Purpose: Require usable dispatch timing independently of byte correctness.
// Inputs: telemetry contains one completed production GPU operation.
// Outputs: Requires real launches, finite positive device time, and actual device transfers.
void require_device_timing(const superzip::GpuTelemetry& telemetry) {
    const auto stats = superzip::snapshot_gpu_telemetry(telemetry);
    REQUIRE_TRUE(stats.kernel_launches > 0U);
    REQUIRE_TRUE(std::isfinite(stats.kernel_ms));
    REQUIRE_TRUE(stats.kernel_ms > 0.0);
    REQUIRE_TRUE(stats.h2d_bytes > 0U);
    REQUIRE_TRUE(stats.device_allocation_bytes > 0U);
}

// Purpose: Exercise dispatch timestamps across all production encoding families and decoded CRC paths.
// Inputs: seed selects deterministic RAM bytes; uses small repeated operations on the calling thread's HIP stream.
// Outputs: Requires independent CPU CRC/output agreement and positive timing for every GPU operation.
void check_production_dispatch_timing(std::uint32_t seed) {
    constexpr std::uint32_t block_size = superzip::kMinArchiveBlockBytes;
    for (unsigned int iteration = 0; iteration < 24U; ++iteration) {
        std::vector<std::byte> input(block_size + 17U);
        const auto family = iteration % 6U;
        for (std::size_t i = 0; i < input.size(); ++i) {
            seed ^= seed << 13U;
            seed ^= seed >> 17U;
            seed ^= seed << 5U;
            const auto value = family == 0   ? 41U
                               : family == 1 ? i % 7U
                               : family == 2 ? seed % 5U
                               : family == 3 ? 193U + seed % 5U
                               : family == 4 ? seed % 256U
                                             : (i < 8192U ? i % 3U : seed % 256U);
            input[i] = static_cast<std::byte>(value);
        }
        superzip::GpuCodecOptions options;
        options.block_size = block_size;
        options.compression_level = iteration < 12U ? 5 : 9;
        options.telemetry = std::make_shared<superzip::GpuTelemetry>();
        const auto encoded = superzip::encode_owned_chunk(input, options);
        REQUIRE_TRUE(encoded.gpu_used);
        REQUIRE_TRUE(encoded.source_crc32_available);
        REQUIRE_EQ(encoded.source_crc32, superzip::crc32(input));
        require_device_timing(*options.telemetry);

        options.telemetry = std::make_shared<superzip::GpuTelemetry>();
        const std::array<std::uint32_t, 2> lengths{block_size, 17U};
        const auto batch = superzip::encode_owned_block_batch(input, lengths, options);
        REQUIRE_TRUE(batch.encoded.gpu_used);
        REQUIRE_TRUE(batch.encoded.payload == encoded.payload);
        REQUIRE_EQ(batch.block_crc32.size(), lengths.size());
        REQUIRE_EQ(batch.block_crc32[0], superzip::crc32(std::span(input).first(block_size)));
        REQUIRE_EQ(batch.block_crc32[1], superzip::crc32(std::span(input).last(17U)));
        require_device_timing(*options.telemetry);

        options.telemetry = std::make_shared<superzip::GpuTelemetry>();
        const auto crc = superzip::crc_decoded_chunk(encoded.payload, encoded.blocks, input.size(), options);
        REQUIRE_TRUE(crc.gpu_used);
        REQUIRE_EQ(crc.crc32, superzip::crc32(input));
        require_device_timing(*options.telemetry);

        options.telemetry = std::make_shared<superzip::GpuTelemetry>();
        std::vector<std::byte> decoded(input.size());
        REQUIRE_TRUE(superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
        REQUIRE_TRUE(decoded == input);
        require_device_timing(*options.telemetry);
        options.require_gpu = false;
        options.force_cpu = true;
        std::fill(decoded.begin(), decoded.end(), std::byte{0});
        (void)superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options);
        REQUIRE_TRUE(decoded == input);
    }
}

}  // namespace

// Purpose: Detect invalid event measurements on independent production HIP worker streams.
// Inputs: Four bounded concurrent callers with distinct fixtures; unavailable HIP skips hardware assertions.
// Outputs: Requires valid per-operation device times and byte-exact CPU/HIP decoding; worker failures propagate.
TEST_CASE(gpu_telemetry_production_dispatch_timing) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::future<void>> workers;
    for (std::uint32_t worker = 0; worker < 4U; ++worker) {
        workers.push_back(std::async(std::launch::async, check_production_dispatch_timing, 0x76543210U + worker));
    }
    for (auto& worker : workers) {
        worker.get();
    }
}

// Purpose: Cover dispatch timing on the diagnostic's compute and shared-memory checksum kernels.
// Inputs: A bounded short RAM-only diagnostic; unavailable HIP skips hardware assertions.
// Outputs: Requires positive finite device time and nonzero checksum/transfer/launch evidence.
TEST_CASE(gpu_telemetry_diagnostic_dispatch_timing) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    const auto result = superzip::run_gpu_diagnostic({.seconds = 1.0, .buffer_mib = 16, .inner_iterations = 1});
    REQUIRE_TRUE(result.kernel_launches >= 2U);
    REQUIRE_TRUE(std::isfinite(result.kernel_ms));
    REQUIRE_TRUE(result.kernel_ms > 0.0);
    REQUIRE_TRUE(result.h2d_bytes > 0U);
    REQUIRE_TRUE(result.d2h_bytes > 0U);
    REQUIRE_TRUE(result.checksum > 0U);
}
