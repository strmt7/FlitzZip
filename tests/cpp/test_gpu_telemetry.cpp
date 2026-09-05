#include "test_util.hpp"

#include "gpu/gpu_codec.hpp"

#include <cmath>
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
