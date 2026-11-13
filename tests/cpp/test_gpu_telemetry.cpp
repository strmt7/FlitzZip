#include "test_util.hpp"

#include "gpu/gpu_codec.hpp"
#include "gpu/hip_device.hpp"
#include "gpu/device_memory_budget.hpp"
#include "gpu/pinned_host_budget.hpp"
#include "gpu/worker_phase_clock.hpp"
#include "gpu/kernel_observation.hpp"
#include "gpu/dictionary_matcher.hpp"
#include "lz4.h"
#include "core/checksum.hpp"
#include "core/decoded_chunk.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#if FLITZZIP_ENABLE_HIP
#include "gpu/hip_kernel_api.hpp"
#endif

#include <array>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <bit>
#include <future>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct WorkerClockReadProbe {
    std::uint32_t& reads;

    auto operator()() const {
        ++reads;
        return std::chrono::steady_clock::time_point{std::chrono::steady_clock::duration{reads}};
    }
};

// Independent upstream LZ4 readback accompanies the real HIP reader. All bytes
// stay in memory, and the requested observation supplies no profiling events.
void require_unmeasured_dictionary_readback(std::span<const std::byte> input,
                                            const flitzzip::dictionary::EncodedBatch& encoded) {
    REQUIRE_TRUE(encoded.gpu_used);
    REQUIRE_EQ(encoded.observation_mode, flitzzip::GpuObservationMode::CountersOnly);
    REQUIRE_TRUE(!encoded.index_ms && !encoded.encode_ms && !encoded.compact_ms && !encoded.device_ms);
    REQUIRE_TRUE(encoded.explicit_kernel_launches > 0U);
    REQUIRE_EQ(encoded.segments.size(), 1U);
    const auto& segment = encoded.segments.front();
    REQUIRE_EQ(segment.input_bytes, input.size());
    REQUIRE_TRUE(input.size() < static_cast<std::size_t>(std::numeric_limits<int>::max()));
    REQUIRE_TRUE(segment.payload.size() < static_cast<std::size_t>(std::numeric_limits<int>::max()));
    std::vector<std::byte> restored(input.size());
    const auto bytes = LZ4_decompress_safe(reinterpret_cast<const char*>(segment.payload.data()),
                                           reinterpret_cast<char*>(restored.data()),
                                           static_cast<int>(segment.payload.size()), static_cast<int>(restored.size()));
    REQUIRE_EQ(bytes, static_cast<int>(input.size()));
    REQUIRE_TRUE(std::ranges::equal(restored, input));
    const auto decoded =
        flitzzip::dictionary::decode_segments(encoded.segments, flitzzip::GpuObservationMode::CountersOnly);
    REQUIRE_TRUE(decoded.gpu_used);
    REQUIRE_EQ(decoded.observation_mode, flitzzip::GpuObservationMode::CountersOnly);
    REQUIRE_TRUE(!decoded.decode_ms);
    REQUIRE_TRUE(std::ranges::equal(decoded.bytes, input));
}

}  // namespace

TEST_CASE(gpu_observation_dictionary_empty_and_invalid_mode_admission) {
    using Mode = flitzzip::GpuObservationMode;
    for (const auto mode : {Mode::Measured, Mode::CountersOnly}) {
        REQUIRE_EQ(flitzzip::dictionary::find_matches({}, 1, mode).observation_mode, mode);
        REQUIRE_EQ(flitzzip::dictionary::encode_segments({}, 1, mode).observation_mode, mode);
        REQUIRE_EQ(flitzzip::dictionary::encode_neutron_segments({}, {}, {}, mode).observation_mode, mode);
        REQUIRE_EQ(flitzzip::dictionary::decode_segments({}, mode).observation_mode, mode);
    }
    const auto reject = [](auto action) {
        bool rejected = false;
        try {
            action();
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    };
    constexpr auto invalid = static_cast<Mode>(255U);
    reject([&] { (void)flitzzip::dictionary::find_matches({}, 1, invalid); });
    reject([&] { (void)flitzzip::dictionary::encode_segments({}, 1, invalid); });
    reject([&] { (void)flitzzip::dictionary::encode_neutron_segments({}, {}, {}, invalid); });
    reject([&] { (void)flitzzip::dictionary::decode_segments({}, invalid); });
}

TEST_CASE(gpu_observation_dictionary_counters_only_cached_and_deferred_readback) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> input(4096U);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<std::byte>((i % 17U) * 13U);
    }
    for (const auto level : {1, 9}) {
        const auto encoded =
            flitzzip::dictionary::encode_segments(input, level, flitzzip::GpuObservationMode::CountersOnly);
        require_unmeasured_dictionary_readback(input, encoded);
    }
    const auto matches = flitzzip::dictionary::find_matches(input, 1, flitzzip::GpuObservationMode::CountersOnly);
    REQUIRE_TRUE(matches.gpu_used);
    REQUIRE_TRUE(!matches.index_ms && !matches.search_ms);
    REQUIRE_EQ(matches.observation_mode, flitzzip::GpuObservationMode::CountersOnly);
    REQUIRE_EQ(matches.matches.size(), input.size());
}

TEST_CASE(gpu_observation_neutron_counters_only_cancellation_and_recovery) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> input(4096U);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<std::byte>((i % 19U) * 11U);
    }
    std::uint32_t checkpoints = 0;
    bool rejected = false;
    try {
        (void)flitzzip::dictionary::encode_neutron_segments(
            input,
            [&] {
                if (++checkpoints == 4U) {
                    throw std::runtime_error("cancel unmeasured Neutron");
                }
            },
            {}, flitzzip::GpuObservationMode::CountersOnly);
    } catch (const std::runtime_error& error) {
        if (std::string_view{error.what()} != "cancel unmeasured Neutron") {
            throw;
        }
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(checkpoints, 4U);
    const auto encoded =
        flitzzip::dictionary::encode_neutron_segments(input, {}, {}, flitzzip::GpuObservationMode::CountersOnly);
    require_unmeasured_dictionary_readback(input, encoded);
}

TEST_CASE(gpu_observation_native_counters_only_required_and_optional_attempts) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> input(4096U);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<std::byte>((i / 7U + i % 23U) % 26U + 65U);
    }
    for (const bool required : {false, true}) {
        for (const auto mode :
             {flitzzip::NativeCompressionMode::Standard, flitzzip::NativeCompressionMode::NeutronStar}) {
            if (!required && mode == flitzzip::NativeCompressionMode::NeutronStar) {
                continue;
            }
            flitzzip::GpuCodecOptions options;
            options.require_gpu = required;
            options.block_size = 4096U;
            options.compression_level = 9;
            options.compression_mode = mode;
            options.telemetry = std::make_shared<flitzzip::GpuTelemetry>(flitzzip::GpuObservationMode::CountersOnly);
            const auto encoded = flitzzip::encode_chunk(input, options);
            std::vector<std::byte> cpu(input.size());
            flitzzip::decode_chunk_cpu(encoded.payload, encoded.blocks, cpu, {});
            REQUIRE_TRUE(std::ranges::equal(cpu, input));
            std::vector<std::byte> gpu(input.size());
            REQUIRE_TRUE(flitzzip::decode_chunk(encoded.payload, encoded.blocks, gpu, options));
            REQUIRE_TRUE(std::ranges::equal(gpu, input));
            const auto stats = flitzzip::snapshot_gpu_telemetry(*options.telemetry);
            REQUIRE_EQ(stats.timing_status, flitzzip::GpuTimingStatus::NotRequested);
            REQUIRE_TRUE(std::isnan(stats.kernel_ms));
            REQUIRE_TRUE(stats.encode_chunks > 0U && stats.decode_chunks > 0U && stats.kernel_launches > 0U);
            REQUIRE_TRUE(!options.telemetry->timing_recorded.load());
        }
    }
}

TEST_CASE(gpu_observation_event_provider_is_never_called_for_counters_only) {
    std::uint32_t calls = 0;
    const auto create = [&]() -> std::unique_ptr<int> {
        ++calls;
        throw std::runtime_error("event provider rejected");
    };
    const auto events = flitzzip::make_gpu_observation_events(flitzzip::GpuObservationMode::CountersOnly, create);
    REQUIRE_TRUE(!events);
    REQUIRE_EQ(calls, 0U);
    bool rejected = false;
    try {
        (void)flitzzip::make_gpu_observation_events(flitzzip::GpuObservationMode::Measured, create);
    } catch (const std::runtime_error& error) {
        if (std::string_view{error.what()} != "event provider rejected") {
            throw;
        }
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(calls, 1U);
    rejected = false;
    try {
        (void)flitzzip::make_gpu_observation_events(static_cast<flitzzip::GpuObservationMode>(255U), create);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(calls, 1U);
}

TEST_CASE(gpu_observation_counters_do_not_fabricate_zero_timing) {
    flitzzip::GpuTelemetry telemetry{flitzzip::GpuObservationMode::CountersOnly};
    flitzzip::record_gpu_completed_kernels(&telemetry, 3U);
    flitzzip::record_gpu_kernel_work(&telemetry, 2U, std::numeric_limits<double>::quiet_NaN());
    flitzzip::record_gpu_h2d_bytes(&telemetry, 17U);
    flitzzip::record_gpu_encode_stage_time(&telemetry, flitzzip::GpuEncodeStage::Readiness,
                                           std::chrono::steady_clock::duration{});
    const auto stats = flitzzip::snapshot_gpu_telemetry(telemetry);
    REQUIRE_EQ(stats.kernel_launches, 5U);
    REQUIRE_EQ(stats.h2d_bytes, 17U);
    REQUIRE_EQ(stats.timing_status, flitzzip::GpuTimingStatus::NotRequested);
    REQUIRE_TRUE(std::isnan(stats.kernel_ms));
    for (const auto value : stats.encode_stage_worker_seconds) {
        REQUIRE_TRUE(std::isnan(value));
    }
    for (const auto value : stats.classification_stage_worker_seconds) {
        REQUIRE_TRUE(std::isnan(value));
    }
    for (const auto value : stats.owned_decode_stage_worker_seconds) {
        REQUIRE_TRUE(std::isnan(value));
    }
    REQUIRE_TRUE(!telemetry.timing_recorded.load());
    REQUIRE_EQ(telemetry.kernel_microseconds.load(), 0U);
}

TEST_CASE(gpu_observation_empty_zero_and_failed_samples_are_distinct) {
    flitzzip::GpuTelemetry telemetry;
    REQUIRE_EQ(flitzzip::snapshot_gpu_telemetry(telemetry).timing_status, flitzzip::GpuTimingStatus::NoSamples);
    flitzzip::record_gpu_kernel_launch(&telemetry, 0.0);
    const auto zero = flitzzip::snapshot_gpu_telemetry(telemetry);
    REQUIRE_EQ(zero.timing_status, flitzzip::GpuTimingStatus::Measured);
    REQUIRE_EQ(std::fpclassify(zero.kernel_ms), FP_ZERO);
    flitzzip::record_gpu_kernel_launch(&telemetry, std::numeric_limits<double>::quiet_NaN());
    REQUIRE_EQ(flitzzip::snapshot_gpu_telemetry(telemetry).timing_status, flitzzip::GpuTimingStatus::Unavailable);
}

TEST_CASE(gpu_observation_measured_work_requires_paired_sample) {
    for (const bool sampled : {false, true}) {
        flitzzip::GpuTelemetry telemetry;
        if (sampled) {
            flitzzip::record_gpu_kernel_work(&telemetry, 2U, 1.25);
        }
        const auto before = flitzzip::snapshot_gpu_telemetry(telemetry);
        bool rejected = false;
        try {
            flitzzip::record_gpu_completed_kernels(&telemetry, 3U);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        const auto after = flitzzip::snapshot_gpu_telemetry(telemetry);
        REQUIRE_EQ(after.kernel_launches, before.kernel_launches);
        // Rejection preserves the representation as well as numeric state,
        // including signed zero and any future unavailable-sample NaN payload.
        REQUIRE_EQ(std::bit_cast<std::uint64_t>(after.kernel_ms), std::bit_cast<std::uint64_t>(before.kernel_ms));
        REQUIRE_EQ(after.timing_status, before.timing_status);
        REQUIRE_EQ(telemetry.timing_recorded.load(), sampled);
        flitzzip::record_gpu_completed_kernels(&telemetry, 0U);
        flitzzip::record_gpu_kernel_work(&telemetry, 1U, 0.0);
        const auto recovered = flitzzip::snapshot_gpu_telemetry(telemetry);
        REQUIRE_EQ(recovered.kernel_launches, sampled ? 3U : 1U);
        // Storage has exact microsecond units; its public double conversion
        // permits only rounding noise, far below one stored tick.
        REQUIRE_EQ(telemetry.kernel_microseconds.load(), sampled ? 1250U : 0U);
        REQUIRE_TRUE(std::abs(recovered.kernel_ms - (sampled ? 1.25 : 0.0)) <=
                     2.0 * std::numeric_limits<double>::epsilon());
        REQUIRE_EQ(recovered.timing_status, flitzzip::GpuTimingStatus::Measured);
    }
    flitzzip::record_gpu_completed_kernels(nullptr, 3U);
}

TEST_CASE(gpu_observation_completion_and_measurement_failures_preserve_actual_work) {
    for (const auto mode : {flitzzip::GpuObservationMode::Measured, flitzzip::GpuObservationMode::CountersOnly}) {
        for (const auto failure : {0, 1, 2}) {
            flitzzip::GpuTelemetry telemetry{mode};
            const bool measured = mode == flitzzip::GpuObservationMode::Measured;
            std::uint32_t completions = 0;
            std::uint32_t queries = 0;
            bool rejected = false;
            try {
                flitzzip::observe_gpu_kernel_work(
                    &telemetry, 3U,
                    [&](bool profile) {
                        REQUIRE_EQ(profile, measured);
                        ++completions;
                        if (failure == 1) {
                            throw std::runtime_error("observation fixture failure");
                        }
                    },
                    [&] {
                        ++queries;
                        if (failure == 2) {
                            throw std::runtime_error("observation fixture failure");
                        }
                        return 0.0;
                    });
            } catch (const std::runtime_error& error) {
                if (std::string_view{error.what()} != "observation fixture failure") {
                    throw;
                }
                rejected = true;
            }
            REQUIRE_EQ(completions, 1U);
            REQUIRE_EQ(queries, measured && failure != 1 ? 1U : 0U);
            REQUIRE_EQ(rejected, failure == 1 || (measured && failure == 2));
            const auto stats = flitzzip::snapshot_gpu_telemetry(telemetry);
            REQUIRE_EQ(stats.kernel_launches, failure == 1 ? 0U : 3U);
            REQUIRE_EQ(stats.timing_status, !measured  ? flitzzip::GpuTimingStatus::NotRequested
                                            : rejected ? flitzzip::GpuTimingStatus::Unavailable
                                                       : flitzzip::GpuTimingStatus::Measured);
        }
    }
}

TEST_CASE(gpu_observation_aggregation_retains_missing_and_invalid_coverage) {
    using Status = flitzzip::GpuTimingStatus;
    constexpr std::array values{Status::NoSamples, Status::NotRequested, Status::Measured, Status::Incomplete,
                                Status::Unavailable};
    constexpr std::array<std::array<Status, 5>, 5> expected{{
        {Status::NoSamples, Status::NotRequested, Status::Measured, Status::Incomplete, Status::Unavailable},
        {Status::NotRequested, Status::NotRequested, Status::Incomplete, Status::Incomplete, Status::Incomplete},
        {Status::Measured, Status::Incomplete, Status::Measured, Status::Incomplete, Status::Unavailable},
        {Status::Incomplete, Status::Incomplete, Status::Incomplete, Status::Incomplete, Status::Incomplete},
        {Status::Unavailable, Status::Incomplete, Status::Unavailable, Status::Incomplete, Status::Unavailable},
    }};
    for (std::size_t lhs = 0; lhs < values.size(); ++lhs) {
        for (std::size_t rhs = 0; rhs < values.size(); ++rhs) {
            REQUIRE_EQ(flitzzip::combine_gpu_timing_status(values[lhs], values[rhs]), expected[lhs][rhs]);
            for (const auto third : values) {
                REQUIRE_EQ(flitzzip::combine_gpu_timing_status(expected[lhs][rhs], third),
                           flitzzip::combine_gpu_timing_status(
                               values[lhs], flitzzip::combine_gpu_timing_status(values[rhs], third)));
            }
        }
    }
    REQUIRE_EQ(flitzzip::combine_gpu_timing_status(Status::Incomplete, Status::Unavailable), Status::Incomplete);
    REQUIRE_EQ(flitzzip::combine_gpu_timing_status(Status::NotRequested, Status::Unavailable), Status::Incomplete);
    flitzzip::GpuRuntimeStats mixed;
    mixed.kernel_launches = 7U;
    mixed.kernel_ms = 1.0;
    mixed.timing_status = flitzzip::combine_gpu_timing_status(Status::Measured, Status::NotRequested);
    flitzzip::finalize_gpu_timing_stats(mixed);
    REQUIRE_EQ(mixed.kernel_launches, 7U);
    REQUIRE_EQ(mixed.timing_status, Status::Incomplete);
    REQUIRE_TRUE(std::isnan(mixed.kernel_ms));
    REQUIRE_TRUE(std::isnan(mixed.encode_stage_worker_seconds.front()));
}

// Read counts are the oracle: these controls never read a real clock or measure performance.
TEST_CASE(worker_phase_clock_disabled_skips_reads_and_recorders) {
    std::uint32_t reads = 0;
    std::uint32_t records = 0;
    flitzzip::WorkerPhaseClock clock{false, WorkerClockReadProbe{reads}};
    auto nested = clock;
    for (auto* phase : {&clock, &nested}) {
        phase->finish([&](auto) { ++records; });
        phase->finish([&](auto) { ++records; });
    }
    REQUIRE_EQ(reads, 0U);
    REQUIRE_EQ(records, 0U);
}

TEST_CASE(worker_phase_clock_nested_boundaries_do_not_restart_clock) {
    std::uint32_t reads = 0;
    std::vector<std::chrono::steady_clock::rep> intervals;
    flitzzip::WorkerPhaseClock clock{true, WorkerClockReadProbe{reads}};
    REQUIRE_EQ(reads, 1U);
    clock.finish([&](auto elapsed) { intervals.push_back(elapsed.count()); });
    auto nested = clock;
    REQUIRE_EQ(reads, 2U);
    nested.finish([&](auto elapsed) { intervals.push_back(elapsed.count()); });
    clock.finish([&](auto elapsed) { intervals.push_back(elapsed.count()); });
    REQUIRE_EQ(reads, 4U);
    REQUIRE_EQ(intervals, (std::vector<std::chrono::steady_clock::rep>{1, 1, 2}));
}

TEST_CASE(worker_phase_clock_recorder_failure_preserves_boundary) {
    std::uint32_t reads = 0;
    flitzzip::WorkerPhaseClock clock{true, WorkerClockReadProbe{reads}};
    bool rejected = false;
    try {
        clock.finish([](auto) { throw std::runtime_error("phase recorder rejected"); });
    } catch (const std::runtime_error& error) {
        if (std::string_view{error.what()} != "phase recorder rejected") {
            throw;
        }
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::chrono::steady_clock::rep recovered = 0;
    clock.finish([&](auto elapsed) { recovered = elapsed.count(); });
    REQUIRE_EQ(reads, 3U);
    REQUIRE_EQ(recovered, 2);
}

TEST_CASE(worker_phase_clock_cpu_owned_decode_untracked) {
    const std::array payload{std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'}, std::byte{'5'},
                             std::byte{'6'}, std::byte{'7'}, std::byte{'8'}, std::byte{'9'}};
    const flitzzip::BlockDescriptor block{
        .kind = flitzzip::BlockKind::Raw,
        .uncompressed_len = 9U,
        .encoded_len = 9U,
    };
    const flitzzip::GpuCodecOptions options{.require_gpu = false, .force_cpu = true};
    auto decoded = flitzzip::decode_owned_chunk(payload, std::span{&block, 1U}, payload.size(), options);
    REQUIRE_TRUE(std::ranges::equal(decoded.bytes(), payload));
    REQUIRE_EQ(decoded.crc32, 0xCBF43926U);
    REQUIRE_TRUE(!decoded.gpu_used);
    auto moved = std::move(decoded);
    REQUIRE_TRUE(decoded.bytes().empty());
    REQUIRE_TRUE(std::ranges::equal(moved.bytes(), payload));
}

// Purpose: Isolate unequal GPU capacities instead of charging one adapter's work to another.
// Inputs: Simulated memory snapshots from two device ordinals; no GPU hardware or allocations are used.
// Outputs: Requires independent admission, retained same-device caps, live pressure rejection and clean epoch reset.
TEST_CASE(device_memory_budget_isolates_adapters) {
    flitzzip::DeviceMemoryBudget budget;
    REQUIRE_TRUE(budget.try_reserve(0, 500U, 1000U, 1000U));
    REQUIRE_TRUE(budget.try_reserve(1, 1000U, 2000U, 2000U));
    REQUIRE_EQ(budget.reserved_bytes(0), 500U);
    REQUIRE_EQ(budget.reserved_bytes(1), 1000U);
    REQUIRE_TRUE(!budget.try_reserve(0, 251U, 1000U, 1000U));
    REQUIRE_TRUE(!budget.try_reserve(1, 501U, 2000U, 2000U));
    REQUIRE_TRUE(!budget.try_reserve(0, 100U, 100U, 1000U));
    REQUIRE_TRUE(budget.try_reserve(0, 250U, 1000U, 1000U));
    REQUIRE_TRUE(budget.release(1, 1000U));
    REQUIRE_EQ(budget.reserved_bytes(0), 750U);
    REQUIRE_TRUE(budget.release(0, 750U));
    REQUIRE_TRUE(budget.try_reserve(0, 1500U, 2000U, 2000U));
    REQUIRE_TRUE(budget.release(0, 1500U));
    REQUIRE_EQ(budget.reserved_bytes(0), 0U);
    REQUIRE_EQ(budget.reserved_bytes(1), 0U);
}

// Purpose: Reject invalid counters and release errors without overflow or lost live reservations.
// Inputs: Zero/negative identity, unknown/impossible memory, over-release and maximum unsigned byte counts.
// Outputs: Requires unchanged accounting after rejection and exact admission up to the reserve boundary.
TEST_CASE(device_memory_budget_rejects_invalid_accounting) {
    flitzzip::DeviceMemoryBudget budget;
    for (const auto device : {-1, 0}) {
        REQUIRE_TRUE(!budget.try_reserve(device, 0U, 1000U, 1000U));
        REQUIRE_TRUE(!budget.try_reserve(device, 1U, 0U, 0U));
        REQUIRE_TRUE(!budget.try_reserve(device, 1U, 1001U, 1000U));
    }
    REQUIRE_TRUE(!budget.try_reserve(-1, 1U, 1000U, 1000U));
    REQUIRE_TRUE(budget.try_reserve(0, 1U, 1U, 1U));
    REQUIRE_TRUE(!budget.release(0, 0U));
    REQUIRE_TRUE(!budget.release(0, 2U));
    REQUIRE_TRUE(!budget.release(1, 1U));
    REQUIRE_EQ(budget.reserved_bytes(0), 1U);
    REQUIRE_TRUE(budget.release(0, 1U));
    REQUIRE_TRUE(!budget.release(0, 1U));
    const auto maximum = std::numeric_limits<std::size_t>::max();
    const auto admitted = maximum - maximum / 20U;
    REQUIRE_TRUE(!budget.try_reserve(0, maximum, maximum, maximum));
    REQUIRE_TRUE(budget.try_reserve(0, admitted, maximum, maximum));
    REQUIRE_TRUE(!budget.try_reserve(0, 1U, maximum, maximum));
    REQUIRE_TRUE(budget.release(0, admitted));
}

// Purpose: Serialize concurrent plans separately for each device without over-admitting either adapter.
// Inputs: Sixteen joined workers each request 25 bytes from a 150-byte usable simulated device budget.
// Outputs: Requires exactly six accepted plans per device and zero outstanding bytes after matched releases.
TEST_CASE(device_memory_budget_concurrent_admission) {
    flitzzip::DeviceMemoryBudget budget;
    std::barrier phase(16);
    std::array<std::atomic<unsigned int>, 2> accepted{};
    {
        std::vector<std::jthread> workers;
        for (int worker = 0; worker < 16; ++worker) {
            workers.emplace_back([&, device = worker % 2] {
                const auto owned = budget.try_reserve(device, 25U, 200U, 200U);
                if (owned) {
                    accepted[static_cast<std::size_t>(device)].fetch_add(1U);
                }
                phase.arrive_and_wait();
                if (owned && !budget.release(device, 25U)) {
                    accepted[static_cast<std::size_t>(device)].store(99U);
                }
            });
        }
    }
    for (int device = 0; device < 2; ++device) {
        REQUIRE_EQ(accepted[static_cast<std::size_t>(device)].load(), 6U);
        REQUIRE_EQ(budget.reserved_bytes(device), 0U);
    }
}

// Purpose: Keep host-output counters callable without a HIP backend or device initialization.
// Inputs: Optional telemetry, zero counts, and distinct cumulative allocation/output values.
// Outputs: Requires exact independent host counters and unchanged device-allocation accounting in every build.
TEST_CASE(gpu_host_pinned_counters_are_backend_independent) {
    flitzzip::GpuTelemetry telemetry;
    flitzzip::record_gpu_host_pinned_allocation_bytes(nullptr, 512U);
    flitzzip::record_gpu_host_pinned_output_bytes(nullptr, 1024U);
    flitzzip::record_gpu_host_pinned_allocation_bytes(&telemetry, 0U);
    flitzzip::record_gpu_host_pinned_output_bytes(&telemetry, 0U);
    flitzzip::record_gpu_host_pinned_allocation_bytes(&telemetry, 512U);
    flitzzip::record_gpu_host_pinned_output_bytes(&telemetry, 1024U);
    flitzzip::record_gpu_host_pinned_output_bytes(&telemetry, 1024U);
    const auto stats = flitzzip::snapshot_gpu_telemetry(telemetry);
    REQUIRE_EQ(stats.host_pinned_allocation_bytes, 512U);
    REQUIRE_EQ(stats.host_pinned_output_bytes, 2048U);
    REQUIRE_EQ(stats.device_allocation_bytes, 0U);
    REQUIRE_EQ(stats.kernel_launches, 0U);
}

// Purpose: Report no GPU work for empty jobs while still rejecting invalid empty decoded layouts.
// Inputs: Required-HIP borrowed/owned inputs and empty outputs, plus zero/positive-length malformed blocks.
// Outputs: Requires zero dispatch/allocation/transfer telemetry and ArchiveError before invalid layouts are accepted.
TEST_CASE(gpu_empty_jobs_preserve_validation_and_work_identity) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    flitzzip::GpuCodecOptions options;
    options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
    const auto borrowed = flitzzip::encode_chunk({}, options);
    const auto owned = flitzzip::encode_owned_chunk({}, options);
    REQUIRE_TRUE(!borrowed.gpu_used && !owned.gpu_used);
    REQUIRE_TRUE(borrowed.source_crc32_available && owned.source_crc32_available);
    REQUIRE_EQ(borrowed.source_crc32, 0U);
    REQUIRE_EQ(owned.source_crc32, 0U);
    REQUIRE_TRUE(!flitzzip::decode_chunk({}, {}, {}, options));
    const auto checked = flitzzip::crc_decoded_chunk({}, {}, 0U, options);
    REQUIRE_TRUE(!checked.gpu_used);
    REQUIRE_EQ(checked.crc32, 0U);
    for (const auto length : {0U, 1U}) {
        const flitzzip::BlockDescriptor block{.kind = flitzzip::BlockKind::Fill, .uncompressed_len = length};
        bool decode_rejected = false;
        bool crc_rejected = false;
        try {
            (void)flitzzip::decode_chunk({}, std::span(&block, 1U), {}, options);
        } catch (const flitzzip::ArchiveError&) {
            decode_rejected = true;
        }
        try {
            (void)flitzzip::crc_decoded_chunk({}, std::span(&block, 1U), 0U, options);
        } catch (const flitzzip::ArchiveError&) {
            crc_rejected = true;
        }
        REQUIRE_TRUE(decode_rejected && crc_rejected);
    }
    const auto stats = flitzzip::snapshot_gpu_telemetry(*options.telemetry);
    REQUIRE_EQ(stats.encode_chunks + stats.decode_chunks + stats.kernel_launches, 0U);
    REQUIRE_EQ(stats.h2d_bytes + stats.d2h_bytes + stats.device_allocation_bytes, 0U);
}

// Purpose: Verify actual copied host output is checksummed with the admitted parallel CPU budget after HIP decode.
// Inputs: Required-HIP raw blocks with an irregular tail and a multi-task checksum extent.
// Outputs: Requires full byte equality and serial CRC equality; no device-produced CRC substitutes for host validation.
TEST_CASE(owned_decoded_chunk_parallel_crc_validates_host_output) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    constexpr std::uint32_t block_size = 16U * 1024U * 1024U;
    constexpr std::uint32_t tail = 37U;
    std::vector<std::byte> payload(block_size + tail);
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<std::byte>((index * 31U + index / 997U) & 255U);
    }
    const std::array<flitzzip::BlockDescriptor, 2> blocks{{
        {.kind = flitzzip::BlockKind::Raw, .uncompressed_len = block_size, .encoded_len = block_size},
        {.kind = flitzzip::BlockKind::Raw, .uncompressed_len = tail, .encoded_offset = block_size, .encoded_len = tail},
    }};
    flitzzip::GpuCodecOptions options;
    options.block_size = block_size;
    options.worker_count = 8U;
    options.host_output_pool = flitzzip::make_owned_decode_pool(options);
    const auto decoded = flitzzip::decode_owned_chunk(payload, blocks, payload.size(), options);
    REQUIRE_TRUE(decoded.gpu_used);
    REQUIRE_TRUE(std::ranges::equal(decoded.bytes(), payload));
    REQUIRE_EQ(decoded.crc32, flitzzip::crc32(payload));
}

// Purpose: Enforce host-relative and absolute pin limits without overflow or dropped concurrent reservations.
// Inputs: Independent budgets, shrinking RAM snapshots, oversized requests, and eight simultaneous contenders.
// Outputs: Requires exact bounded admission, balanced release, and reuse without a HIP device dependency.
TEST_CASE(pinned_host_budget_admission_and_concurrency) {
    flitzzip::PinnedHostBudget budget;
    constexpr std::uint64_t mib = 1024U * 1024U;
    REQUIRE_TRUE(!budget.try_reserve(0, 8192U * mib));
    REQUIRE_TRUE(!budget.try_reserve(1, 15));
    REQUIRE_EQ(flitzzip::PinnedHostBudget::allowance(1024U * mib), 64U * mib);
    REQUIRE_EQ(flitzzip::PinnedHostBudget::allowance(std::numeric_limits<std::uint64_t>::max()),
               flitzzip::PinnedHostBudget::maximum_bytes);
    REQUIRE_TRUE(
        !budget.try_reserve(std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max()));
    REQUIRE_TRUE(budget.try_reserve(32U * mib, 1024U * mib));
    REQUIRE_TRUE(!budget.try_reserve(1, 512U * mib));
    REQUIRE_TRUE(budget.try_reserve(32U * mib, 1024U * mib));
    REQUIRE_TRUE(!budget.try_reserve(1, 1024U * mib));
    budget.release(64U * mib);
    std::barrier start(8);
    std::barrier reserved(8);
    std::atomic<unsigned> admitted{0};
    std::array<std::future<void>, 8> workers;
    for (auto& worker : workers) {
        worker = std::async(std::launch::async, [&] {
            start.arrive_and_wait();
            const auto owned = budget.try_reserve(128U * mib, std::numeric_limits<std::uint64_t>::max());
            if (owned) {
                admitted.fetch_add(1U, std::memory_order_relaxed);
            }
            reserved.arrive_and_wait();
            if (owned) {
                budget.release(128U * mib);
            }
        });
    }
    for (auto& worker : workers) {
        worker.get();
    }
    REQUIRE_EQ(admitted.load(), 4U);
    REQUIRE_EQ(budget.reserved_bytes(), 0U);
    REQUIRE_TRUE(budget.try_reserve(flitzzip::PinnedHostBudget::maximum_bytes, 8192U * mib));
    budget.release(flitzzip::PinnedHostBudget::maximum_bytes);
}

// Purpose: Preserve CPU concurrency while required-HIP owned output respects live aggregate pin capacity.
// Inputs: CPU/automatic/required-HIP options, zero/large windows, and requested depths above and below capacity.
// Outputs: Requires bounded admission and consistent capacity-derived slots without requiring a GPU device.
TEST_CASE(owned_decoded_chunk_concurrency_admission_policy) {
    flitzzip::GpuCodecOptions options;
    REQUIRE_TRUE(flitzzip::hip_host_output_capacity_bytes() <= flitzzip::PinnedHostBudget::maximum_bytes);
    const auto admitted = flitzzip::resolve_owned_decode_inflight(32U, 128U * 1024U * 1024U, options);
    REQUIRE_TRUE(admitted >= 1U && admitted <= 4U);
    REQUIRE_EQ(flitzzip::resolve_owned_decode_inflight(32U, flitzzip::PinnedHostBudget::maximum_bytes + 1U, options),
               1U);
    REQUIRE_EQ(flitzzip::resolve_owned_decode_inflight(1U, 128U * 1024U * 1024U, options), 1U);
    REQUIRE_EQ(flitzzip::resolve_owned_decode_inflight(32U, 0U, options), 32U);
    options.force_cpu = true;
    REQUIRE_EQ(flitzzip::resolve_owned_decode_inflight(32U, 128U * 1024U * 1024U, options), 32U);
    options.force_cpu = false;
    options.require_gpu = false;
    REQUIRE_EQ(flitzzip::resolve_owned_decode_inflight(32U, 128U * 1024U * 1024U, options), 32U);
}

// Purpose: Prove real pinned decode ownership survives concurrent workers and destruction on another thread.
// Inputs: Four distinct 4 MiB required-HIP raw chunks, independent telemetry, and the normal host pin admission.
// Outputs: Requires exact restored bytes/CRC, pin telemetry matching admitted owners, and unchanged release accounting.
TEST_CASE(owned_decoded_chunk_pinned_concurrent_cross_thread_release) {
    const auto gpu = flitzzip::query_gpu_info();
    if (!gpu.available) {
        return;
    }
    const auto before = flitzzip::snapshot_hip_pinned_host_stats();
    std::array<std::future<flitzzip::DecodedChunk>, 4> workers;
    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::async(std::launch::async, [index] {
            constexpr std::size_t size = 4U * 1024U * 1024U;
            std::vector<std::byte> payload(size, static_cast<std::byte>(index + 1U));
            const flitzzip::BlockDescriptor block{
                .kind = flitzzip::BlockKind::Raw,
                .uncompressed_len = size,
                .encoded_len = size,
            };
            flitzzip::GpuCodecOptions options;
            options.block_size = size;
            options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
            auto decoded = flitzzip::decode_owned_chunk(payload, std::span(&block, 1U), size, options);
            REQUIRE_TRUE(decoded.gpu_used);
            REQUIRE_TRUE(std::ranges::equal(decoded.bytes(), payload));
            REQUIRE_EQ(decoded.crc32, flitzzip::crc32(payload));
            const auto pinned = decoded.storage.get_deleter().release == flitzzip::release_hip_host_output;
            REQUIRE_EQ(flitzzip::snapshot_gpu_telemetry(*options.telemetry).host_pinned_allocation_bytes,
                       pinned ? size : 0U);
            return decoded;
        });
    }
    for (auto& worker : workers) {
        auto owned = worker.get();
        REQUIRE_EQ(owned.bytes().size(), 4U * 1024U * 1024U);
    }
    const auto after = flitzzip::snapshot_hip_pinned_host_stats();
    REQUIRE_EQ(after.reserved_bytes, before.reserved_bytes);
    REQUIRE_EQ(after.release_failures, before.release_failures);
    REQUIRE_EQ(flitzzip::query_gpu_info().runtime_version, gpu.runtime_version);
}

// Purpose: Preserve CPU-only decode and release pinned storage when admitted output fails validation.
// Inputs: A CPU Zstandard block in automatic mode and a malformed HIP raw payload with bounded output.
// Outputs: Requires no pinning for CPU decode, ArchiveError for corruption, and unchanged aggregate pin accounting.
TEST_CASE(owned_decoded_chunk_pinned_failure_and_cpu_only_policy) {
    constexpr std::size_t size = 1024U * 1024U;
    std::vector<std::byte> input(size);
    for (std::size_t index = 0; index < input.size(); ++index) {
        input[index] = static_cast<std::byte>(index & 15U);
    }
    flitzzip::GpuCodecOptions cpu;
    cpu.force_cpu = true;
    cpu.require_gpu = false;
    cpu.block_size = size;
    const auto encoded = flitzzip::encode_chunk(input, cpu);
    REQUIRE_EQ(encoded.blocks.front().kind, flitzzip::BlockKind::CpuZstd);
    flitzzip::GpuCodecOptions automatic = cpu;
    automatic.force_cpu = false;
    automatic.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
    const auto before = flitzzip::snapshot_hip_pinned_host_stats();
    const auto decoded = flitzzip::decode_owned_chunk(encoded.payload, encoded.blocks, size, automatic);
    REQUIRE_TRUE(!decoded.gpu_used);
    REQUIRE_TRUE(decoded.storage.get_deleter().release == nullptr);
    REQUIRE_TRUE(std::ranges::equal(decoded.bytes(), input));
    REQUIRE_EQ(flitzzip::snapshot_gpu_telemetry(*automatic.telemetry).host_pinned_allocation_bytes, 0U);
    if (flitzzip::query_gpu_info().available) {
        for (const bool pooled : {false, true}) {
            flitzzip::GpuCodecOptions gpu;
            if (pooled) {
                gpu.host_output_pool = flitzzip::make_owned_decode_pool(gpu);
            }
            const flitzzip::BlockDescriptor invalid{
                .kind = flitzzip::BlockKind::Raw, .uncompressed_len = size, .encoded_len = size};
            bool rejected = false;
            try {
                (void)flitzzip::decode_owned_chunk({}, std::span(&invalid, 1U), size, gpu);
            } catch (const flitzzip::ArchiveError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
        }
    }
    const auto after = flitzzip::snapshot_hip_pinned_host_stats();
    REQUIRE_EQ(after.reserved_bytes, before.reserved_bytes);
    REQUIRE_EQ(after.release_failures, before.release_failures);
    REQUIRE_TRUE(flitzzip::try_allocate_hip_host_output(flitzzip::kMaxArchiveChunkBytes + 1U) == nullptr);
}

// Purpose: Prove cache reuse, mixed-size admission, cross-thread ownership, and cleanup after the operation drops its
// pool. Inputs: Bounded required-HIP raw output, a smaller reused extent, and final owners retained beyond operation
// scope. Outputs: Requires exact bytes/CRC, truthful fresh/reused telemetry, and reservation restoration after the last
// owner.
TEST_CASE(owned_decoded_chunk_pinned_pool_reuse_and_lifetime) {
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const auto before = flitzzip::snapshot_hip_pinned_host_stats();
    flitzzip::GpuCodecOptions options;
    options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
    options.host_output_pool = flitzzip::make_owned_decode_pool(options);
    constexpr std::size_t large_size = 4U * 1024U * 1024U;
    const std::vector<std::byte> payload(large_size, std::byte{0x19});
    const flitzzip::BlockDescriptor large{
        .kind = flitzzip::BlockKind::Raw, .uncompressed_len = large_size, .encoded_len = large_size};
    auto first = flitzzip::decode_owned_chunk(payload, std::span(&large, 1U), large_size, options);
    const auto pinned = first.storage.get_deleter().pool != nullptr;
    auto* first_pointer = first.storage.get();
    first = {};
    constexpr std::size_t small_size = 1024U * 1024U;
    const flitzzip::BlockDescriptor small{
        .kind = flitzzip::BlockKind::Raw, .uncompressed_len = small_size, .encoded_len = small_size};
    auto second = std::async(std::launch::async, [&] {
                      return flitzzip::decode_owned_chunk(std::span(payload).first(small_size), std::span(&small, 1U),
                                                          small_size, options);
                  }).get();
    REQUIRE_TRUE(std::ranges::equal(second.bytes(), std::span(payload).first(small_size)));
    REQUIRE_EQ(second.crc32, flitzzip::crc32(std::span(payload).first(small_size)));
    if (pinned) {
        REQUIRE_TRUE(second.storage.get() == first_pointer);
        REQUIRE_TRUE(!second.storage.get_deleter().new_pinned_allocation);
        const auto stats = flitzzip::snapshot_gpu_telemetry(*options.telemetry);
        REQUIRE_EQ(stats.host_pinned_allocation_bytes, large_size);
        REQUIRE_EQ(stats.host_pinned_output_bytes, large_size + small_size);
    }
    options.host_output_pool.reset();
    second = {};
    const auto after = flitzzip::snapshot_hip_pinned_host_stats();
    REQUIRE_EQ(after.reserved_bytes, before.reserved_bytes);
    REQUIRE_EQ(after.release_failures, before.release_failures);
    options.force_cpu = true;
    REQUIRE_TRUE(flitzzip::make_owned_decode_pool(options) == nullptr);
    options.force_cpu = false;
    options.require_gpu = false;
    REQUIRE_TRUE(flitzzip::make_owned_decode_pool(options) == nullptr);
}

// Purpose: Prove that owned decoding fills every byte and remains safe when ownership moves.
// Inputs: Fill, raw, and irregular-tail blocks through CPU and, when present, required HIP.
// Outputs: Requires exact bytes, CRC, backend identity, and an empty moved-from view.
TEST_CASE(owned_decoded_chunk_roundtrip_and_move) {
    constexpr std::size_t block_size = 4096U;
    std::vector<std::byte> input(block_size * 2U + 17U, std::byte{0x7b});
    for (std::size_t i = block_size; i < input.size(); ++i) {
        input[i] = static_cast<std::byte>((i * 31U + i / 97U) & 255U);
    }
    const auto gpu_available = flitzzip::query_gpu_info().available;
    for (const bool use_gpu : {false, true}) {
        if (use_gpu && !gpu_available) {
            continue;
        }
        flitzzip::GpuCodecOptions options;
        options.force_cpu = !use_gpu;
        options.require_gpu = use_gpu;
        options.block_size = block_size;
        options.compression_level = 1;
        const auto encoded = flitzzip::encode_chunk(input, options);
        auto decoded = flitzzip::decode_owned_chunk(encoded.payload, encoded.blocks, input.size(), options);
        REQUIRE_TRUE(std::ranges::equal(decoded.bytes(), input));
        REQUIRE_EQ(decoded.crc32, flitzzip::crc32(input));
        REQUIRE_EQ(decoded.gpu_used, use_gpu);
        auto moved = std::move(decoded);
        REQUIRE_TRUE(decoded.bytes().empty());
        REQUIRE_TRUE(std::ranges::equal(moved.bytes(), input));
        flitzzip::DecodedChunk assigned;
        assigned = std::move(moved);
        REQUIRE_TRUE(moved.bytes().empty());
        REQUIRE_TRUE(std::ranges::equal(assigned.bytes(), input));
    }
}

// Purpose: Reject malformed output extents before returning any owned decoded bytes.
// Inputs: Zero/oversized blocks, missing/excess output, invalid encoding kinds, and truncated raw payload.
// Outputs: Requires ArchiveError and a well-defined empty owner for an empty chunk.
TEST_CASE(owned_decoded_chunk_rejects_invalid_layouts) {
    flitzzip::GpuCodecOptions options;
    options.require_gpu = false;
    options.force_cpu = true;
    const std::array payload{std::byte{0x42}};
    const std::array<std::pair<flitzzip::BlockDescriptor, std::size_t>, 8> invalid = {{
        {{.kind = flitzzip::BlockKind::Fill, .uncompressed_len = 0}, 1},
        {{.kind = flitzzip::BlockKind::Fill, .uncompressed_len = flitzzip::kMaxArchiveBlockBytes + 1U}, 1},
        {{.kind = flitzzip::BlockKind::Fill, .uncompressed_len = 1}, 0},
        {{.kind = flitzzip::BlockKind::Fill, .uncompressed_len = 1}, 2},
        {{.kind = flitzzip::BlockKind::Fill, .uncompressed_len = 1}, flitzzip::kMaxArchiveChunkBytes + 1U},
        {{.kind = flitzzip::BlockKind::Raw, .uncompressed_len = 2, .encoded_len = 2}, 2},
        {{.kind = static_cast<flitzzip::BlockKind>(255), .uncompressed_len = 1}, 1},
        {{.kind = flitzzip::BlockKind::Fill, .uncompressed_len = 1, .encoded_len = 1}, 1},
    }};
    for (const auto& [block, size] : invalid) {
        bool rejected = false;
        try {
            (void)flitzzip::decode_owned_chunk(payload, std::span(&block, 1U), size, options);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    const auto empty = flitzzip::decode_owned_chunk({}, {}, 0U, options);
    REQUIRE_TRUE(empty.bytes().empty());
    REQUIRE_EQ(empty.crc32, 0U);
    REQUIRE_TRUE(!empty.gpu_used);
}

// Purpose: Preserve required-GPU refusal rather than decoding a CPU-only block into the new owner.
// Inputs: CPU-generated Deflate and Zstandard blocks with required HIP and no fallback permission.
// Outputs: Requires GpuError on both HIP-enabled and CPU-only builds.
TEST_CASE(owned_decoded_chunk_required_gpu_rejects_cpu_blocks) {
    for (const auto size : {1024U, 4096U}) {
        std::vector<std::byte> input(size);
        for (std::size_t i = 0; i < input.size(); ++i) {
            input[i] = static_cast<std::byte>(i & 15U);
        }
        flitzzip::GpuCodecOptions cpu;
        cpu.require_gpu = false;
        cpu.force_cpu = true;
        cpu.block_size = 4096U;
        const auto encoded = flitzzip::encode_chunk(input, cpu);
        REQUIRE_EQ(encoded.blocks.front().kind,
                   size == 1024U ? flitzzip::BlockKind::Deflate : flitzzip::BlockKind::CpuZstd);
        flitzzip::GpuCodecOptions gpu;
        gpu.require_gpu = true;
        gpu.block_size = 4096U;
        bool rejected = false;
        try {
            (void)flitzzip::decode_owned_chunk(encoded.payload, encoded.blocks, input.size(), gpu);
        } catch (const flitzzip::GpuError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Preserve explicit CPU compatibility selection without attempting unsupported HIP decode.
// Inputs: Independently CPU-produced Deflate/Zstd blocks and optional-HIP options on either build configuration.
// Outputs: Requires exact bytes/CRC and zero device activity, while required HIP still rejects the same archives.
TEST_CASE(optional_hip_selects_cpu_only_blocks_before_device_work) {
    for (const auto size : {1024U, 4096U}) {
        std::vector<std::byte> source(size);
        for (std::size_t index = 0; index < source.size(); ++index) {
            source[index] = static_cast<std::byte>(index & 15U);
        }
        flitzzip::GpuCodecOptions cpu;
        cpu.require_gpu = false;
        cpu.force_cpu = true;
        cpu.block_size = 4096U;
        const auto encoded = flitzzip::encode_chunk(source, cpu);
        REQUIRE_EQ(encoded.blocks.front().kind,
                   size == 1024U ? flitzzip::BlockKind::Deflate : flitzzip::BlockKind::CpuZstd);
        auto optional = cpu;
        optional.force_cpu = false;
        optional.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
        std::vector<std::byte> decoded(source.size());
        REQUIRE_TRUE(!flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, optional));
        REQUIRE_TRUE(decoded == source);
        const auto checked = flitzzip::crc_decoded_chunk(encoded.payload, encoded.blocks, source.size(), optional);
        REQUIRE_TRUE(!checked.gpu_used);
        REQUIRE_EQ(checked.crc32, flitzzip::crc32(source));
        const auto stats = flitzzip::snapshot_gpu_telemetry(*optional.telemetry);
        REQUIRE_EQ(stats.kernel_launches + stats.h2d_bytes + stats.d2h_bytes + stats.device_allocation_bytes, 0U);
    }
}

// Purpose: Distinguish expected absent-HIP CPU operation from real available-device work.
// Inputs: Live readiness plus borrowed, owned and independent-block optional encoding on a small RAM fixture.
// Outputs: Requires exact CPU/HIP readback and one published telemetry count per successful optional operation;
// the CPU-only build requires no device activity.
TEST_CASE(optional_hip_encoding_uses_explicit_availability) {
    const bool available = flitzzip::hip_device_available();
    flitzzip::GpuCodecOptions options;
    options.require_gpu = false;
    options.block_size = 4096U;
    options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
    const std::vector<std::byte> source(4096U, std::byte{73});
    const std::array<std::uint32_t, 1> lengths{4096U};
    for (const auto& encoded : {flitzzip::encode_chunk(source, options), flitzzip::encode_owned_chunk(source, options),
                                flitzzip::encode_owned_block_batch(source, lengths, options).encoded}) {
        REQUIRE_EQ(encoded.gpu_used, available);
        std::vector<std::byte> decoded(source.size());
        auto cpu = options;
        cpu.force_cpu = true;
        REQUIRE_TRUE(!flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, cpu));
        REQUIRE_TRUE(decoded == source);
        std::fill(decoded.begin(), decoded.end(), std::byte{0});
        REQUIRE_EQ(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options), available);
        REQUIRE_TRUE(decoded == source);
        const auto checked = flitzzip::crc_decoded_chunk(encoded.payload, encoded.blocks, source.size(), options);
        REQUIRE_EQ(checked.gpu_used, available);
        REQUIRE_EQ(checked.crc32, flitzzip::crc32(source));
    }
    const auto stats = flitzzip::snapshot_gpu_telemetry(*options.telemetry);
    REQUIRE_EQ(stats.encode_chunks, available ? 3U : 0U);
    REQUIRE_EQ(stats.decode_chunks, available ? 6U : 0U);
    REQUIRE_EQ(stats.kernel_launches > 0U, available);
    REQUIRE_EQ(stats.device_allocation_bytes > 0U, available);
}

// Purpose: Preserve thread-local device selection while admitting the actual kernel payload on independent threads.
// Inputs: The live HIP capability state; absent hardware is separately exercised by dependency/package contracts.
// Outputs: Requires repeated successful native admission and unchanged device selection on two calling threads.
TEST_CASE(gpu_kernel_admission_preserves_thread_device_selection) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    // Purpose: Check repeated admission against one thread's live device instead of sharing another thread's cache.
    // Inputs: The thread's default/current device. Outputs: Requires unchanged selection and successful resolution.
    const auto check = [] {
        int before = -1;
        REQUIRE_EQ(hipGetDevice(&before), hipSuccess);
        flitzzip::require_hip_kernel_device_support();
        flitzzip::require_hip_kernel_device_support();
        int after = -1;
        REQUIRE_EQ(hipGetDevice(&after), hipSuccess);
        REQUIRE_EQ(after, before);
        REQUIRE_TRUE(flitzzip::hip_device_available());
    };
    check();
    std::async(std::launch::async, check).get();
#else
    REQUIRE_TRUE(!flitzzip::hip_device_available());
#endif
}

// Observe logical reservations at both inverse-transform boundaries without exhausting physical VRAM.
// Independent full/context frames, cancellation and reuse require only decoded bytes to remain owned.
TEST_CASE(gpu_composed_crc_releases_decode_scratch_before_consumer) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    int device = -1;
    REQUIRE_EQ(hipGetDevice(&device), hipSuccess);
    auto& budget = flitzzip::hip_detail::hip_device_reservation_state();
    const auto baseline = budget.reserved_bytes(device);
    constexpr std::uint32_t bytes = 4099U;
    for (const bool contexts : {false, true}) {
        const std::vector<std::byte> frame =
            contexts ? std::vector<std::byte>{std::byte{2}, std::byte{0},   std::byte{0}, std::byte{0},
                                              std::byte{1}, std::byte{'A'}, std::byte{0}, std::byte{0},
                                              std::byte{0}, std::byte{0},   std::byte{1}, std::byte{'B'},
                                              std::byte{0}, std::byte{0},   std::byte{0}, std::byte{0}}
                     : std::vector<std::byte>{std::byte{4}, std::byte{3},   std::byte{0},
                                              std::byte{0}, std::byte{'A'}, std::byte{'B'}};
        const flitzzip::BlockDescriptor block{.kind = contexts ? flitzzip::BlockKind::GpuBytePlaneContexts
                                                               : flitzzip::BlockKind::GpuBytePlane,
                                              .uncompressed_len = bytes,
                                              .encoded_len = static_cast<std::uint32_t>(frame.size())};
        std::vector<std::byte> expected(bytes);
        for (std::size_t index = 0U; index < expected.size(); ++index) {
            const auto transformed = index < 4096U ? (index % 4U) * 1024U + index / 4U : index;
            expected[index] = contexts ? (index == bytes - 1U || index % 2U ? std::byte{'B'} : std::byte{'A'})
                                       : (transformed % 2U ? std::byte{'B'} : std::byte{'A'});
        }
        for (const unsigned int cancellation : {0U, 1U, 2U}) {
            unsigned int checkpoints = 0U;
            flitzzip::GpuCodecOptions options{.require_gpu = true};
            options.encode_checkpoint = [&] {
                ++checkpoints;
                const auto outputs = checkpoints == 1U ? 1U : 2U;
                REQUIRE_EQ(budget.reserved_bytes(device), baseline + outputs * bytes);
                if (checkpoints == cancellation) {
                    throw flitzzip::ArchiveError("independent scratch-lifetime cancellation");
                }
            };
            bool cancelled = false;
            try {
                const auto verified = flitzzip::crc_decoded_chunk(frame, std::span(&block, 1U), bytes, options);
                REQUIRE_TRUE(verified.gpu_used);
                REQUIRE_EQ(verified.crc32, flitzzip::crc32(expected));
            } catch (const flitzzip::ArchiveError& error) {
                if (!cancellation || std::string_view(error.what()) != "independent scratch-lifetime cancellation") {
                    throw;
                }
                cancelled = true;
            }
            REQUIRE_EQ(cancelled, cancellation != 0U);
            REQUIRE_EQ(checkpoints, cancellation ? cancellation : 2U);
            REQUIRE_EQ(budget.reserved_bytes(device), baseline);
            options.encode_checkpoint = {};
            const auto recovered = flitzzip::crc_decoded_chunk(frame, std::span(&block, 1U), bytes, options);
            REQUIRE_TRUE(recovered.gpu_used);
            REQUIRE_EQ(recovered.crc32, flitzzip::crc32(expected));
            REQUIRE_EQ(budget.reserved_bytes(device), baseline);
        }
    }
#endif
}

// Exercise monotonic accounting without allocating large buffers or simulating host-wide memory pressure.
// Independent owners, empty/moved states, rejected growth and exception unwinding must preserve exact totals.
TEST_CASE(gpu_device_reservation_reduction_preserves_owners_and_unwinding) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    int device = -1;
    REQUIRE_EQ(hipGetDevice(&device), hipSuccess);
    auto& budget = flitzzip::hip_detail::hip_device_reservation_state();
    const auto baseline = budget.reserved_bytes(device);
    {
        flitzzip::hip_detail::HipDeviceMemoryReservation first(64U, "independent reservation fixture");
        flitzzip::hip_detail::HipDeviceMemoryReservation sibling(32U, "independent sibling fixture");
        REQUIRE_EQ(budget.reserved_bytes(device), baseline + 96U);
        first.reduce_to(16U);
        first.reduce_to(16U);
        REQUIRE_EQ(budget.reserved_bytes(device), baseline + 48U);
        bool rejected = false;
        try {
            first.reduce_to(17U);
        } catch (const flitzzip::GpuError& error) {
            rejected = std::string_view(error.what()) == "cannot grow a HIP device memory reservation";
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(budget.reserved_bytes(device), baseline + 48U);
        flitzzip::hip_detail::HipDeviceMemoryReservation moved(std::move(first));
        first.reduce_to(0U);
        flitzzip::hip_detail::HipDeviceMemoryReservation replacement(8U, "independent replacement fixture");
        REQUIRE_EQ(budget.reserved_bytes(device), baseline + 56U);
        replacement = std::move(moved);
        moved.reduce_to(0U);
        REQUIRE_EQ(budget.reserved_bytes(device), baseline + 48U);
        replacement.reduce_to(0U);
        sibling.reduce_to(0U);
        REQUIRE_EQ(budget.reserved_bytes(device), baseline);
        rejected = false;
        try {
            replacement.reduce_to(1U);
        } catch (const flitzzip::GpuError& error) {
            rejected = std::string_view(error.what()) == "cannot grow a HIP device memory reservation";
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(budget.reserved_bytes(device), baseline);
    }
    bool unwound = false;
    try {
        flitzzip::hip_detail::HipDeviceMemoryReservation owner(64U, "independent unwind fixture");
        owner.reduce_to(16U);
        REQUIRE_EQ(budget.reserved_bytes(device), baseline + 16U);
        throw flitzzip::ArchiveError("independent reservation unwind");
    } catch (const flitzzip::ArchiveError& error) {
        if (std::string_view(error.what()) != "independent reservation unwind") {
            throw;
        }
        unwound = true;
    }
    REQUIRE_TRUE(unwound);
    REQUIRE_EQ(budget.reserved_bytes(device), baseline);
    int after = -1;
    REQUIRE_EQ(hipGetDevice(&after), hipSuccess);
    REQUIRE_EQ(after, device);
#endif
}

// Purpose: Keep allocator compatibility decisions tied to the loaded numeric DLL version rather than SDK labels.
// Inputs: Missing metadata, the reproducing older runtime, minimum-version boundaries, and newer numeric versions.
// Outputs: Requires conservative legacy eligibility and exact path-free dotted diagnostic formatting.
TEST_CASE(gpu_stream_allocator_runtime_version_admission) {
    using flitzzip::HipRuntimeVersion;
    REQUIRE_TRUE(!flitzzip::hip_runtime_allows_stream_allocations(std::nullopt));
    for (const auto version : {HipRuntimeVersion{10, 0, 3665, 0}, HipRuntimeVersion{10, 0, 3678, 65535},
                               HipRuntimeVersion{9, 65535, 65535, 65535}}) {
        REQUIRE_TRUE(!flitzzip::hip_runtime_allows_stream_allocations(version));
    }
    for (const auto version : {HipRuntimeVersion{10, 0, 3679, 0}, HipRuntimeVersion{10, 0, 3679, 1},
                               HipRuntimeVersion{10, 1, 0, 0}, HipRuntimeVersion{11, 0, 0, 0}}) {
        REQUIRE_TRUE(flitzzip::hip_runtime_allows_stream_allocations(version));
    }
    REQUIRE_EQ(flitzzip::hip_runtime_version_text(std::nullopt), std::string("unavailable"));
    REQUIRE_EQ(flitzzip::hip_runtime_version_text(HipRuntimeVersion{10, 0, 3679, 0}), std::string("10.0.3679.0"));
}

// Purpose: Detect allocation cross-talk and incomplete frees on joined production codec streams.
// Inputs: Four 128 MiB distinct fill streams using required HIP, plus the exact loaded runtime's capability state.
// Outputs: Requires byte-exact independent roundtrips and zero live pool bytes after all operation owners finish.
TEST_CASE(gpu_concurrent_large_allocations_preserve_data_and_release_pool) {
    const auto before = flitzzip::query_gpu_info();
    if (!before.available) {
        return;
    }
    std::array<std::future<void>, 4> workers;
    for (std::size_t worker = 0; worker < workers.size(); ++worker) {
        workers[worker] = std::async(std::launch::async, [worker] {
            const std::vector<std::byte> input(128U * 1024U * 1024U, static_cast<std::byte>(worker + 1U));
            flitzzip::GpuCodecOptions options;
            options.require_gpu = true;
            options.compression_level = 1;
            const auto encoded = flitzzip::encode_chunk(input, options);
            REQUIRE_TRUE(encoded.gpu_used);
            REQUIRE_TRUE(encoded.source_crc32_available);
            REQUIRE_EQ(encoded.source_crc32, flitzzip::crc32(input));
            std::vector<std::byte> decoded(input.size());
            REQUIRE_TRUE(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
            REQUIRE_TRUE(decoded == input);
        });
    }
    for (auto& worker : workers) {
        worker.get();
    }
    const auto after = flitzzip::query_gpu_info();
    REQUIRE_TRUE(after.available);
    REQUIRE_TRUE(after.runtime_version == before.runtime_version);
    if (after.stream_ordered_allocator_supported) {
        REQUIRE_TRUE(after.pool_used_bytes.has_value());
        REQUIRE_EQ(after.pool_used_bytes.value(), 0U);
    }
}

// Purpose: Reject non-finite or out-of-range diagnostic durations before any HIP work.
// Inputs: NaN, infinities, invalid endpoints, and invalid device-work admission values.
// Outputs: Requires ArchiveError independently of compiled backend or installed GPU hardware.
TEST_CASE(gpu_diagnostic_invalid_options_fail_before_dispatch) {
    const std::array durations = {std::numeric_limits<double>::quiet_NaN(),
                                  std::numeric_limits<double>::infinity(),
                                  -std::numeric_limits<double>::infinity(),
                                  0.0,
                                  31.0,
                                  std::nextafter(1.0, 0.0),
                                  std::nextafter(30.0, 31.0)};
    for (const auto seconds : durations) {
        flitzzip::GpuDiagnosticOptions options;
        options.seconds = seconds;
        bool rejected = false;
        try {
            (void)flitzzip::run_gpu_diagnostic(options);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    for (const auto buffer : {0U, 15U, 513U}) {
        flitzzip::GpuDiagnosticOptions options;
        options.buffer_mib = buffer;
        bool rejected = false;
        try {
            (void)flitzzip::run_gpu_diagnostic(options);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    for (const auto iterations : {0U, 4097U}) {
        flitzzip::GpuDiagnosticOptions options;
        options.inner_iterations = iterations;
        bool rejected = false;
        try {
            (void)flitzzip::run_gpu_diagnostic(options);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Preserve thread-local device selection and explicit missing-backend errors in readiness admission.
// Inputs: The current compiled backend; available HIP is checked concurrently without changing device selection.
// Outputs: Requires stable diagnostics on HIP, or the precise CPU-only GpuError on hosted validation builds.
TEST_CASE(gpu_device_readiness_contract) {
#if FLITZZIP_ENABLE_HIP
    const auto before = flitzzip::query_gpu_info();
    if (!before.available) {
        return;
    }
    std::array<std::future<void>, 4> workers;
    for (auto& worker : workers) {
        worker = std::async(std::launch::async, [] {
            const auto first = flitzzip::query_gpu_info();
            REQUIRE_TRUE(first.available);
            flitzzip::require_hip_device_ready();
            flitzzip::require_hip_device_ready();
            const auto second = flitzzip::query_gpu_info();
            REQUIRE_TRUE(second.available);
            REQUIRE_EQ(first.selected_device, second.selected_device);
            REQUIRE_EQ(first.device_count, second.device_count);
            REQUIRE_EQ(first.gcn_arch, second.gcn_arch);
        });
    }
    for (auto& worker : workers) {
        worker.get();
    }
    flitzzip::require_hip_device_ready();
    REQUIRE_EQ(before.selected_device, flitzzip::query_gpu_info().selected_device);
#else
    bool rejected = false;
    try {
        flitzzip::require_hip_device_ready();
    } catch (const flitzzip::GpuError& error) {
        rejected = true;
        REQUIRE_EQ(std::string(error.what()), "Built without HIP acceleration");
    }
    REQUIRE_TRUE(rejected);
#endif
}

// Purpose: Preserve finite timing precision and distinguish real zero duration from unavailable data.
// Inputs: Synthetic event durations; no HIP device or workload is required.
// Outputs: Requires nearest-microsecond rounding, correct launch counts, and a finite empty snapshot.
TEST_CASE(gpu_telemetry_finite_event_rounding) {
    flitzzip::GpuTelemetry telemetry;
    REQUIRE_TRUE(std::abs(flitzzip::snapshot_gpu_telemetry(telemetry).kernel_ms) < 1e-12);
    flitzzip::record_gpu_dictionary_blocks(&telemetry, 2U);
    flitzzip::record_gpu_sparse_pattern_blocks(&telemetry, 3U);
    for (const auto value : {0.0, -0.0, 0.00049, 0.00050, 1.234}) {
        flitzzip::record_gpu_kernel_launch(&telemetry, value);
    }
    const auto stats = flitzzip::snapshot_gpu_telemetry(telemetry);
    REQUIRE_EQ(stats.kernel_launches, 5U);
    REQUIRE_EQ(stats.dictionary_blocks, 2U);
    REQUIRE_EQ(stats.sparse_pattern_blocks, 3U);
    REQUIRE_EQ(telemetry.kernel_microseconds.load(), 1235U);
    REQUIRE_TRUE(std::abs(stats.kernel_ms - 1.235) < 1e-12);
    flitzzip::record_gpu_kernel_launch(nullptr, std::numeric_limits<double>::quiet_NaN());
}

// Purpose: Preserve independently named HIP encode worker-stage totals under concurrent accumulation.
// Inputs: Four joined writers record synthetic durations; null, zero, negative, and sentinel stages are ignored.
// Outputs: Requires exact summed seconds for populated stages and zero for untouched stages.
TEST_CASE(gpu_encode_worker_stage_accumulation) {
    flitzzip::GpuTelemetry telemetry;
    {
        std::vector<std::jthread> workers;
        for (int worker = 0; worker < 4; ++worker) {
            workers.emplace_back([&telemetry] {
                for (int event = 0; event < 1000; ++event) {
                    flitzzip::record_gpu_encode_stage_time(&telemetry, flitzzip::GpuEncodeStage::Readiness,
                                                           std::chrono::milliseconds(1));
                    flitzzip::record_gpu_encode_stage_time(&telemetry, flitzzip::GpuEncodeStage::Dictionary,
                                                           std::chrono::milliseconds(2));
                }
            });
        }
    }
    flitzzip::record_gpu_encode_stage_time(nullptr, flitzzip::GpuEncodeStage::Prefix, std::chrono::milliseconds(1));
    flitzzip::record_gpu_encode_stage_time(&telemetry, flitzzip::GpuEncodeStage::Prefix, std::chrono::milliseconds(-1));
    flitzzip::record_gpu_encode_stage_time(&telemetry, flitzzip::GpuEncodeStage::Count, std::chrono::milliseconds(1));
    const auto stats = flitzzip::snapshot_gpu_telemetry(telemetry);
    REQUIRE_TRUE(
        std::abs(stats.encode_stage_worker_seconds[static_cast<std::size_t>(flitzzip::GpuEncodeStage::Readiness)] -
                 4.0) < 1e-12);
    REQUIRE_TRUE(
        std::abs(stats.encode_stage_worker_seconds[static_cast<std::size_t>(flitzzip::GpuEncodeStage::Dictionary)] -
                 8.0) < 1e-12);
    REQUIRE_TRUE(
        std::abs(stats.encode_stage_worker_seconds[static_cast<std::size_t>(flitzzip::GpuEncodeStage::Prefix)]) <
        1e-12);
}

// Purpose: Preserve nested classification totals without counting them again as top-level encode stages.
// Inputs: Four joined writers and all invalid substage/duration cases use synthetic intervals only.
// Outputs: Requires exact nested sums, zero unrelated totals, and no invented GPU execution work.
TEST_CASE(gpu_classification_worker_stage_accumulation) {
    flitzzip::GpuTelemetry telemetry;
    {
        std::vector<std::jthread> workers;
        for (int worker = 0; worker < 4; ++worker) {
            workers.emplace_back([&telemetry] {
                for (int event = 0; event < 1000; ++event) {
                    flitzzip::record_gpu_classification_stage_time(
                        &telemetry, flitzzip::GpuClassificationStage::InputUpload, std::chrono::milliseconds(1));
                    flitzzip::record_gpu_classification_stage_time(
                        &telemetry, flitzzip::GpuClassificationStage::SourceChecksum, std::chrono::milliseconds(2));
                }
            });
        }
    }
    for (const auto stage :
         {flitzzip::GpuClassificationStage::Count, static_cast<flitzzip::GpuClassificationStage>(999U)}) {
        flitzzip::record_gpu_classification_stage_time(&telemetry, stage, std::chrono::milliseconds(1));
    }
    for (const auto duration : {std::chrono::milliseconds(0), std::chrono::milliseconds(-1)}) {
        flitzzip::record_gpu_classification_stage_time(&telemetry,
                                                       flitzzip::GpuClassificationStage::CandidateValidation, duration);
    }
    flitzzip::record_gpu_classification_stage_time(nullptr, flitzzip::GpuClassificationStage::InputAllocation,
                                                   std::chrono::milliseconds(1));
    const auto stats = flitzzip::snapshot_gpu_telemetry(telemetry);
    const auto& stages = stats.classification_stage_worker_seconds;
    REQUIRE_TRUE(std::abs(stages[static_cast<std::size_t>(flitzzip::GpuClassificationStage::InputUpload)] - 4.0) <
                 1e-12);
    REQUIRE_TRUE(std::abs(stages[static_cast<std::size_t>(flitzzip::GpuClassificationStage::SourceChecksum)] - 8.0) <
                 1e-12);
    REQUIRE_EQ(std::fpclassify(stages[static_cast<std::size_t>(flitzzip::GpuClassificationStage::InputAllocation)]),
               FP_ZERO);
    REQUIRE_EQ(std::fpclassify(stages[static_cast<std::size_t>(flitzzip::GpuClassificationStage::CandidateValidation)]),
               FP_ZERO);
    for (const auto value : stats.encode_stage_worker_seconds) {
        REQUIRE_EQ(std::fpclassify(value), FP_ZERO);
    }
    REQUIRE_EQ(stats.encode_chunks + stats.decode_chunks + stats.kernel_launches, 0U);
}

// Purpose: Preserve distinct CPU/GPU owned-decode work totals under concurrent recording.
// Inputs: Four joined writers, invalid/null stages, and zero/negative durations.
// Outputs: Requires exact worker-second sums, zero untouched stages, and no GPU execution counter changes.
TEST_CASE(owned_decode_worker_stage_accumulation) {
    flitzzip::GpuTelemetry telemetry;
    {
        std::vector<std::jthread> workers;
        for (int worker = 0; worker < 4; ++worker) {
            workers.emplace_back([&telemetry] {
                for (int event = 0; event < 1000; ++event) {
                    flitzzip::record_owned_decode_stage_time(&telemetry, flitzzip::OwnedDecodeStage::OutputAllocation,
                                                             std::chrono::milliseconds(1));
                    flitzzip::record_owned_decode_stage_time(&telemetry, flitzzip::OwnedDecodeStage::HostChecksum,
                                                             std::chrono::milliseconds(2));
                }
            });
        }
    }
    for (const auto stage : {flitzzip::OwnedDecodeStage::Count, static_cast<flitzzip::OwnedDecodeStage>(999U)}) {
        flitzzip::record_owned_decode_stage_time(&telemetry, stage, std::chrono::milliseconds(1));
    }
    for (const auto duration : {std::chrono::milliseconds(0), std::chrono::milliseconds(-1)}) {
        flitzzip::record_owned_decode_stage_time(&telemetry, flitzzip::OwnedDecodeStage::Materialization, duration);
    }
    flitzzip::record_owned_decode_stage_time(nullptr, flitzzip::OwnedDecodeStage::Materialization,
                                             std::chrono::milliseconds(1));
    const auto stats = flitzzip::snapshot_gpu_telemetry(telemetry);
    const auto& stages = stats.owned_decode_stage_worker_seconds;
    REQUIRE_TRUE(std::abs(stages[static_cast<std::size_t>(flitzzip::OwnedDecodeStage::OutputAllocation)] - 4.0) <
                 1e-12);
    REQUIRE_TRUE(std::abs(stages[static_cast<std::size_t>(flitzzip::OwnedDecodeStage::HostChecksum)] - 8.0) < 1e-12);
    REQUIRE_EQ(std::fpclassify(stages[static_cast<std::size_t>(flitzzip::OwnedDecodeStage::Materialization)]), FP_ZERO);
    REQUIRE_EQ(stats.encode_chunks + stats.decode_chunks + stats.kernel_launches, 0U);
}

// Purpose: Prevent accumulated stage intervals from wrapping into plausible but false measurements.
// Inputs: Encode, nested classification and decode counters start one microsecond below the unavailable marker.
// Outputs: Overflow snapshots are NaN and remain unavailable after later valid intervals.
TEST_CASE(worker_stage_timing_overflow_remains_unavailable) {
    flitzzip::GpuTelemetry telemetry;
    telemetry.encode_stage_worker_microseconds[0].store(std::numeric_limits<std::uint64_t>::max() - 1U);
    telemetry.owned_decode_stage_worker_microseconds[0].store(std::numeric_limits<std::uint64_t>::max() - 1U);
    telemetry.classification_stage_worker_microseconds[0].store(std::numeric_limits<std::uint64_t>::max() - 1U);
    for (const auto duration : {std::chrono::microseconds(1), std::chrono::microseconds(50)}) {
        flitzzip::record_gpu_encode_stage_time(&telemetry, flitzzip::GpuEncodeStage::Readiness, duration);
        flitzzip::record_owned_decode_stage_time(&telemetry, flitzzip::OwnedDecodeStage::OutputAllocation, duration);
        flitzzip::record_gpu_classification_stage_time(&telemetry, flitzzip::GpuClassificationStage::InputAllocation,
                                                       duration);
    }
    const auto stats = flitzzip::snapshot_gpu_telemetry(telemetry);
    REQUIRE_TRUE(std::isnan(stats.encode_stage_worker_seconds[0]));
    REQUIRE_TRUE(std::isnan(stats.owned_decode_stage_worker_seconds[0]));
    REQUIRE_TRUE(std::isnan(stats.classification_stage_worker_seconds[0]));
    REQUIRE_EQ(std::fpclassify(stats.encode_stage_worker_seconds[1]), FP_ZERO);
    REQUIRE_EQ(std::fpclassify(stats.owned_decode_stage_worker_seconds[1]), FP_ZERO);
    REQUIRE_EQ(std::fpclassify(stats.classification_stage_worker_seconds[1]), FP_ZERO);
}

// Purpose: Exercise nested timings through the same CPU, optional-GPU and required-GPU production dispatch.
// Inputs: One bounded input and empty work use independent operation telemetry in each available backend mode.
// Outputs: Requires unchanged source CRC/bytes, exact microsecond containment, finite reported detail and CPU zeros.
TEST_CASE(gpu_classification_production_stage_timing) {
    std::vector<std::byte> input(1024U * 1024U);
    for (std::size_t index = 0; index < input.size(); ++index) {
        input[index] = static_cast<std::byte>((index * 31U + index / 997U) & 255U);
    }
    const auto gpu_available = flitzzip::query_gpu_info().available;
    const auto modes = std::array{
        flitzzip::GpuCodecOptions{.require_gpu = false, .force_cpu = true},
        flitzzip::GpuCodecOptions{.require_gpu = false, .force_cpu = false},
        flitzzip::GpuCodecOptions{.require_gpu = true, .force_cpu = false},
    };
    for (auto options : modes) {
        if (options.require_gpu && !gpu_available) {
            continue;
        }
        options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
        const auto empty = flitzzip::encode_chunk({}, options);
        REQUIRE_TRUE(empty.payload.empty());
        for (const auto value :
             flitzzip::snapshot_gpu_telemetry(*options.telemetry).classification_stage_worker_seconds) {
            REQUIRE_EQ(std::fpclassify(value), FP_ZERO);
        }
        const auto encoded = flitzzip::encode_chunk(input, options);
        REQUIRE_EQ(encoded.gpu_used, !options.force_cpu && gpu_available);
        if (encoded.gpu_used) {
            REQUIRE_TRUE(encoded.source_crc32_available);
            REQUIRE_EQ(encoded.source_crc32, flitzzip::crc32(input));
        }
        std::vector<std::byte> decoded(input.size());
        flitzzip::GpuCodecOptions cpu;
        cpu.require_gpu = false;
        cpu.force_cpu = true;
        flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, cpu);
        REQUIRE_EQ(decoded, input);
        const auto stats = flitzzip::snapshot_gpu_telemetry(*options.telemetry);
        double total = 0.0;
        for (const auto value : stats.classification_stage_worker_seconds) {
            REQUIRE_TRUE(std::isfinite(value) && value >= 0.0);
            total += value;
        }
        if (encoded.gpu_used) {
            REQUIRE_TRUE(total > 0.0);
            std::uint64_t raw_total = 0;
            for (const auto& value : options.telemetry->classification_stage_worker_microseconds) {
                raw_total += value.load(std::memory_order_relaxed);
            }
            const auto raw_parent = options.telemetry
                                        ->encode_stage_worker_microseconds[static_cast<std::size_t>(
                                            flitzzip::GpuEncodeStage::DeviceClassification)]
                                        .load(std::memory_order_relaxed);
            REQUIRE_TRUE(raw_parent != std::numeric_limits<std::uint64_t>::max());
            REQUIRE_TRUE(raw_total > 0U && raw_total <= raw_parent);
        } else {
            REQUIRE_EQ(std::fpclassify(total), FP_ZERO);
        }
    }
}

// Purpose: Observe the shared production allocation, materialization and host-CRC stages without changing bytes.
// Inputs: A bounded raw block runs in CPU and, when available, required-GPU mode with operation telemetry.
// Outputs: Requires independent byte/CRC equality, finite stage totals bounded by the single call, and empty no-work.
TEST_CASE(owned_decode_production_stage_timing) {
    std::vector<std::byte> input(1024U * 1024U);
    for (std::size_t index = 0; index < input.size(); ++index) {
        input[index] = static_cast<std::byte>((index * 31U + index / 997U) & 255U);
    }
    const flitzzip::BlockDescriptor block{.kind = flitzzip::BlockKind::Raw,
                                          .uncompressed_len = static_cast<std::uint32_t>(input.size()),
                                          .encoded_len = static_cast<std::uint32_t>(input.size())};
    for (const auto gpu : {false, true}) {
        if (gpu && !flitzzip::query_gpu_info().available) {
            continue;
        }
        flitzzip::GpuCodecOptions options;
        options.require_gpu = gpu;
        options.force_cpu = !gpu;
        options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
        const auto empty = flitzzip::decode_owned_chunk({}, {}, 0U, options);
        REQUIRE_TRUE(empty.bytes().empty());
        for (const auto value :
             flitzzip::snapshot_gpu_telemetry(*options.telemetry).owned_decode_stage_worker_seconds) {
            REQUIRE_EQ(std::fpclassify(value), FP_ZERO);
        }
        const auto started = std::chrono::steady_clock::now();
        const auto decoded = flitzzip::decode_owned_chunk(input, std::span(&block, 1U), input.size(), options);
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        REQUIRE_EQ(decoded.gpu_used, gpu);
        REQUIRE_TRUE(std::ranges::equal(decoded.bytes(), input));
        REQUIRE_EQ(decoded.crc32, flitzzip::crc32(input));
        const auto stats = flitzzip::snapshot_gpu_telemetry(*options.telemetry);
        double total = 0.0;
        for (const auto value : stats.owned_decode_stage_worker_seconds) {
            REQUIRE_TRUE(std::isfinite(value) && value >= 0.0);
            total += value;
        }
        REQUIRE_TRUE(total > 0.0 && total <= seconds);
    }
}

// Purpose: Keep invalid HIP event values out of integer conversion and preserve unavailable timing thereafter.
// Inputs: Negative, non-finite, and out-of-range synthetic durations followed by a valid duration.
// Outputs: Requires NaN timing, intact execution counters, and a sticky unavailable marker without throwing.
TEST_CASE(gpu_telemetry_invalid_events_remain_unavailable) {
    for (const auto value :
         {-0.000001, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
          -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::max(), std::ldexp(1.0, 64) / 1000.0}) {
        flitzzip::GpuTelemetry telemetry;
        flitzzip::record_gpu_encode_chunk(&telemetry);
        flitzzip::record_gpu_kernel_launch(&telemetry, 1.0);
        flitzzip::record_gpu_kernel_launch(&telemetry, value);
        flitzzip::record_gpu_kernel_launch(&telemetry, 2.0);
        const auto stats = flitzzip::snapshot_gpu_telemetry(telemetry);
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
    flitzzip::GpuTelemetry telemetry;
    telemetry.kernel_microseconds.store(std::numeric_limits<std::uint64_t>::max() - 10U);
    flitzzip::record_gpu_kernel_launch(&telemetry, 0.009);
    REQUIRE_TRUE(std::isfinite(flitzzip::snapshot_gpu_telemetry(telemetry).kernel_ms));
    flitzzip::record_gpu_kernel_launch(&telemetry, 0.001);
    REQUIRE_TRUE(std::isnan(flitzzip::snapshot_gpu_telemetry(telemetry).kernel_ms));
    flitzzip::record_gpu_kernel_launch(&telemetry, 1.0);
    REQUIRE_EQ(telemetry.kernel_microseconds.load(), std::numeric_limits<std::uint64_t>::max());
}

// Purpose: Exercise atomic accumulation and invalidation with bounded concurrent writers.
// Inputs: Four joined CPU threads, each recording 4096 synthetic one-microsecond events.
// Outputs: Requires exact valid sums and no lost unavailable state when another writer invalidates the total.
TEST_CASE(gpu_telemetry_concurrent_accumulation) {
    for (const bool invalidate : {false, true}) {
        flitzzip::GpuTelemetry telemetry;
        {
            std::vector<std::jthread> workers;
            for (int worker = 0; worker < 4; ++worker) {
                workers.emplace_back([&telemetry, worker, invalidate] {
                    for (int event = 0; event < 4096; ++event) {
                        flitzzip::record_gpu_kernel_launch(&telemetry, 0.001);
                    }
                    if (invalidate && worker == 0) {
                        flitzzip::record_gpu_kernel_launch(&telemetry, -0.01);
                    }
                });
            }
        }
        const auto stats = flitzzip::snapshot_gpu_telemetry(telemetry);
        REQUIRE_EQ(stats.kernel_launches, 16384U + (invalidate ? 1U : 0U));
        if (invalidate) {
            REQUIRE_TRUE(std::isnan(stats.kernel_ms));
        } else {
            REQUIRE_TRUE(std::abs(stats.kernel_ms - 16.384) < 1e-12);
        }
    }
}

namespace {

// Purpose: Require usable dispatch timing independently of byte correctness.
// Inputs: telemetry contains one completed production GPU operation.
// Outputs: Requires real launches, finite positive device time, and actual device transfers.
void require_device_timing(const flitzzip::GpuTelemetry& telemetry) {
    const auto stats = flitzzip::snapshot_gpu_telemetry(telemetry);
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
    constexpr std::uint32_t block_size = flitzzip::kMinArchiveBlockBytes;
    for (const int compression_level : {5, 9}) {
        for (const unsigned int family : {0U, 1U, 2U, 3U, 4U, 5U, 0U, 1U, 2U, 3U, 4U, 5U}) {
            std::vector<std::byte> input(block_size + 17U);
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
            flitzzip::GpuCodecOptions options;
            options.block_size = block_size;
            options.compression_level = compression_level;
            options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
            const auto encoded = flitzzip::encode_owned_chunk(input, options);
            REQUIRE_TRUE(encoded.gpu_used);
            REQUIRE_TRUE(encoded.source_crc32_available);
            REQUIRE_EQ(encoded.source_crc32, flitzzip::crc32(input));
            require_device_timing(*options.telemetry);

            options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
            const std::array<std::uint32_t, 2> lengths{block_size, 17U};
            const auto batch = flitzzip::encode_owned_block_batch(input, lengths, options);
            REQUIRE_TRUE(batch.encoded.gpu_used);
            REQUIRE_TRUE(batch.encoded.payload == encoded.payload);
            REQUIRE_EQ(batch.block_crc32.size(), lengths.size());
            REQUIRE_EQ(batch.block_crc32[0], flitzzip::crc32(std::span(input).first(block_size)));
            REQUIRE_EQ(batch.block_crc32[1], flitzzip::crc32(std::span(input).last(17U)));
            require_device_timing(*options.telemetry);

            options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
            const auto crc = flitzzip::crc_decoded_chunk(encoded.payload, encoded.blocks, input.size(), options);
            REQUIRE_TRUE(crc.gpu_used);
            REQUIRE_EQ(crc.crc32, flitzzip::crc32(input));
            require_device_timing(*options.telemetry);

            options.telemetry = std::make_shared<flitzzip::GpuTelemetry>();
            std::vector<std::byte> decoded(input.size());
            REQUIRE_TRUE(flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
            REQUIRE_TRUE(decoded == input);
            require_device_timing(*options.telemetry);
            options.require_gpu = false;
            options.force_cpu = true;
            std::fill(decoded.begin(), decoded.end(), std::byte{0});
            (void)flitzzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options);
            REQUIRE_TRUE(decoded == input);
        }
    }
}

}  // namespace

// Purpose: Detect invalid event measurements on independent production HIP worker streams.
// Inputs: Four bounded concurrent callers with distinct fixtures; unavailable HIP skips hardware assertions.
// Outputs: Requires valid per-operation device times and byte-exact CPU/HIP decoding; worker failures propagate.
TEST_CASE(gpu_telemetry_production_dispatch_timing) {
    if (!flitzzip::query_gpu_info().available) {
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
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const auto result = flitzzip::run_gpu_diagnostic({.seconds = 1.0, .buffer_mib = 16, .inner_iterations = 1});
    REQUIRE_TRUE(result.kernel_launches >= 2U);
    REQUIRE_TRUE(std::isfinite(result.kernel_ms));
    REQUIRE_TRUE(result.kernel_ms > 0.0);
    REQUIRE_TRUE(result.h2d_bytes > 0U);
    REQUIRE_TRUE(result.d2h_bytes > 0U);
    REQUIRE_TRUE(result.checksum > 0U);
}
