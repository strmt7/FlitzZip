#include "test_util.hpp"
#include "gpu/hip_device.hpp"

#include <array>
#include <limits>
#include <vector>

#if FLITZZIP_ENABLE_HIP
#include "rans_gpu_fixtures.hpp"
#include "rans_fixtures.hpp"
#include "hip_runtime_fixture.hpp"
#include "gpu/hip_crc.hpp"
#include "gpu/hip_rans.hpp"
#include "gpu/hip_rans_encoder.hpp"

namespace {
using namespace flitzzip;
using namespace flitzzip::hip_detail;
using namespace flitzzip_test;

std::size_t encoder_reserved_bytes() {
    int device = -1;
    REQUIRE_EQ(hipGetDevice(&device), hipSuccess);
    return hip_device_reservation_state().reserved_bytes(device);
}

template <typename Error, typename Operation> void require_encoder_error(Operation&& operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const Error&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

std::vector<std::byte> encoder_source(std::uint32_t bytes) {
    std::vector<std::byte> source(bytes);
    for (std::uint32_t i = 0U; i < bytes; ++i) {
        source[i] = static_cast<std::byte>((i * 73U + i / 31U) & 15U);
    }
    source.front() = std::byte{0xFF};
    return source;
}

std::uint32_t independent_encoder_crc(std::span<const std::byte> bytes) {
    std::uint32_t value = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        value ^= static_cast<std::uint32_t>(byte);
        for (unsigned int bit = 0U; bit < 8U; ++bit) {
            value = (value >> 1U) ^ ((value & 1U) != 0U ? 0xEDB88320U : 0U);
        }
    }
    return value ^ 0xFFFFFFFFU;
}

void require_encoder_readback(std::span<const std::byte> payload, std::span<const std::byte> expected,
                              bool device_readback) {
    GpuRansBlockView block(payload, static_cast<std::uint32_t>(expected.size()));
    std::vector<std::uint32_t> models(block.model_workspace_words());
    std::vector<std::byte> actual(expected.size());
    block.decode(actual, models);
    REQUIRE_EQ(actual, std::vector<std::byte>(expected.begin(), expected.end()));
    if (!device_readback) {
        return;
    }
    HipDeviceMemoryReservation reservation(payload.size() + expected.size() + 4U, "encoder readback fixture");
    CountDeviceBuffer<std::byte> input(static_cast<std::uint32_t>(payload.size()));
    CountDeviceBuffer<std::byte> output(static_cast<std::uint32_t>(expected.size()));
    input.reset(payload);
    GpuTelemetry telemetry{GpuObservationMode::CountersOnly};
    decode_rans_block_device(block, {input.data(), payload.size()}, {output.data(), expected.size()}, models,
                             &telemetry);
    REQUIRE_EQ(output.read(static_cast<std::uint32_t>(expected.size())), actual);
    REQUIRE_EQ(compute_crc32_device(output.data(), expected.size(), &telemetry, "encoder independent decoded CRC"),
               independent_encoder_crc(expected));
    REQUIRE_TRUE(snapshot_gpu_telemetry(telemetry).kernel_launches > 0U);
    REQUIRE_EQ(input.read(static_cast<std::uint32_t>(payload.size())),
               std::vector<std::byte>(payload.begin(), payload.end()));
    output.release_checked();
    input.release_checked();
}

}  // namespace
#endif

TEST_CASE(gpu_rans_encoder_workspace_counts_padding_all_models_and_returned_payload) {
#if FLITZZIP_ENABLE_HIP
    constexpr std::size_t models = 256U * 12U + 65536U + (65536U + 66048U + 65536U + 256U) * 4U;
    for (const std::size_t bytes : {1U, 3U, 255U, 256U, 257U, kRansCountMaxBytes}) {
        const auto plan = rans_encode_workspace_requirements(bytes);
        const auto segments = (bytes + 255U) / 256U;
        const auto slots = 2U * bytes + 4U * segments;
        const auto padded = ((slots + 3U) / 4U) * 4U;
        REQUIRE_EQ(plan.max_segments, segments);
        REQUIRE_EQ(plan.slot_bytes, slots);
        REQUIRE_EQ(plan.scratch_bytes, models + padded + segments * 12U + (segments + 1U) * 4U + bytes);
        REQUIRE_EQ(plan.device_bytes, plan.scratch_bytes + rans_statistics_workspace_requirements().device_bytes);
        REQUIRE_EQ(plan.host_bytes, sizeof(RansEncoder) + 65536U * 4U + (segments + 1U) * 4U + bytes);
    }
    for (const auto bytes : std::array<std::size_t, 3>{0U, std::size_t{kRansCountMaxBytes} + 1U,
                                                       std::numeric_limits<std::size_t>::max()}) {
        require_encoder_error<ArchiveError>([&] { (void)rans_encode_workspace_requirements(bytes); });
    }
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

#if FLITZZIP_ENABLE_HIP
namespace {

class EncoderFaultInput {
  public:
    explicit EncoderFaultInput(std::uint32_t bytes)
        : source(bytes, std::byte{0xFF}), reservation(bytes + 2U, "rANS runtime fault input"), input(bytes) {
        input.reset(source);
    }
    std::span<const std::byte> resident() const {
        return {input.data(), source.size()};
    }
    std::vector<std::byte> source;
    HipDeviceMemoryReservation reservation;
    CountDeviceBuffer<std::byte> input;
};

void require_fault_candidate(RansEncoder& encoder, const EncoderFaultInput& input) {
    const auto payload = encoder.encode(16U, 8U, 8U, input.source.size());
    REQUIRE_TRUE(payload.has_value());
    REQUIRE_EQ(*payload, rans_singleton_payload(static_cast<std::uint32_t>(input.source.size()), 16U, 8U, 8U, 255U));
    require_encoder_readback(*payload, input.source, false);
}

std::array<std::size_t, kHipCallKinds> successful_trial_calls(const EncoderFaultInput& input) {
    HipFaultRuntime runtime;
    RansEncoder encoder(input.resident(), nullptr, {}, runtime.calls());
    const auto before = runtime.counts;
    require_fault_candidate(encoder, input);
    auto counts = runtime.counts;
    for (std::size_t i = 0U; i < counts.size(); ++i) {
        counts[i] -= before[i];
    }
    encoder.close_checked();
    REQUIRE_TRUE(runtime.valid);
    REQUIRE_EQ(runtime.live_count(), 0U);
    return counts;
}

void require_trial_runtime_failures(HipCall kind) {
    EncoderFaultInput input(33025U);
    const auto baseline = encoder_reserved_bytes();
    const auto counts = successful_trial_calls(input);
    const auto total = counts[static_cast<std::size_t>(kind)];
    REQUIRE_TRUE(total > 0U && total < 256U);
    for (std::size_t ordinal = 1U; ordinal <= total; ++ordinal) {
        HipFaultRuntime runtime;
        {
            RansEncoder encoder(input.resident(), nullptr, {}, runtime.calls());
            runtime.fail(kind, ordinal);
            require_encoder_error<GpuError>([&] { (void)encoder.encode(16U, 8U, 8U, input.source.size()); });
            REQUIRE_EQ(runtime.injected, 1U);
            REQUIRE_EQ(runtime.live_count(), 2U);
            require_fault_candidate(encoder, input);
            encoder.close_checked();
        }
        REQUIRE_TRUE(runtime.valid);
        REQUIRE_EQ(runtime.live_count(), 0U);
        REQUIRE_EQ(encoder_reserved_bytes(), baseline);
    }
    REQUIRE_EQ(input.input.read(static_cast<std::uint32_t>(input.source.size())), input.source);
    input.input.release_checked();
}

}  // namespace
#endif

TEST_CASE(gpu_rans_encoder_runtime_constructor_failures_release_acquired_storage) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    EncoderFaultInput input(513U);
    const auto baseline = encoder_reserved_bytes();
    std::array<std::size_t, kHipCallKinds> counts{};
    {
        HipFaultRuntime runtime;
        RansEncoder encoder(input.resident(), nullptr, {}, runtime.calls());
        counts = runtime.counts;
        encoder.close_checked();
        REQUIRE_TRUE(runtime.valid);
        REQUIRE_EQ(runtime.live_count(), 0U);
    }
    for (const auto kind :
         std::array{HipCall::allocate, HipCall::copy, HipCall::clear, HipCall::launch, HipCall::synchronize}) {
        const auto total = counts[static_cast<std::size_t>(kind)];
        REQUIRE_TRUE(total > 0U && total < 32U);
        for (std::size_t ordinal = 1U; ordinal <= total; ++ordinal) {
            HipFaultRuntime runtime;
            runtime.fail(kind, ordinal);
            require_encoder_error<GpuError>(
                [&] { RansEncoder encoder(input.resident(), nullptr, {}, runtime.calls()); });
            REQUIRE_EQ(runtime.injected, 1U);
            REQUIRE_EQ(runtime.live_count(), 0U);
            REQUIRE_EQ(encoder_reserved_bytes(), baseline);
            {
                RansEncoder recovered(input.resident(), nullptr, {}, runtime.calls());
                require_fault_candidate(recovered, input);
                recovered.close_checked();
            }
            REQUIRE_TRUE(runtime.valid);
            REQUIRE_EQ(runtime.live_count(), 0U);
            REQUIRE_EQ(encoder_reserved_bytes(), baseline);
        }
    }
    input.input.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_encoder_runtime_copy_failures_discard_partial_trials_and_recover) {
#if FLITZZIP_ENABLE_HIP
    if (query_gpu_info().available) {
        require_trial_runtime_failures(HipCall::copy);
    }
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_encoder_runtime_clear_failures_discard_partial_trials_and_recover) {
#if FLITZZIP_ENABLE_HIP
    if (query_gpu_info().available) {
        require_trial_runtime_failures(HipCall::clear);
    }
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_encoder_runtime_launch_failures_discard_partial_trials_and_recover) {
#if FLITZZIP_ENABLE_HIP
    if (query_gpu_info().available) {
        require_trial_runtime_failures(HipCall::launch);
    }
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_encoder_runtime_completion_failures_discard_partial_trials_and_recover) {
#if FLITZZIP_ENABLE_HIP
    if (query_gpu_info().available) {
        require_trial_runtime_failures(HipCall::synchronize);
    }
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_encoder_runtime_failed_child_drain_preserves_error_and_prohibits_reuse) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    EncoderFaultInput input(33025U);
    const auto baseline = encoder_reserved_bytes();
    HipFaultRuntime runtime;
    {
        RansEncoder encoder(input.resident(), nullptr, {}, runtime.calls());
        runtime.fail(HipCall::launch, 1U);
        runtime.fail_drain_after_primary = true;
        std::string message;
        try {
            (void)encoder.encode(16U, 8U, 8U, input.source.size());
        } catch (const GpuError& error) {
            message = error.what();
        }
        REQUIRE_TRUE(message.find("count rANS segment boundaries") != std::string::npos);
        REQUIRE_EQ(runtime.injected, 2U);
        const auto stopped = runtime.counts;
        // Even an immediately pruned trial must reject a terminal child state.
        require_encoder_error<GpuError>([&] { (void)encoder.encode(16U, 8U, 8U, 0U); });
        REQUIRE_EQ(runtime.counts, stopped);
        encoder.close_checked();
    }
    REQUIRE_TRUE(runtime.valid);
    REQUIRE_EQ(runtime.live_count(), 0U);
    REQUIRE_EQ(encoder_reserved_bytes(), baseline);
    input.input.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_encoder_runtime_checked_free_failures_retain_ownership_and_budget) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    EncoderFaultInput input(513U);
    const auto baseline = encoder_reserved_bytes();
    const auto plan = rans_encode_workspace_requirements(input.source.size());
    for (const auto ordinal : std::array{1U, 2U}) {
        HipFaultRuntime runtime;
        {
            RansEncoder encoder(input.resident(), nullptr, {}, runtime.calls());
            require_fault_candidate(encoder, input);
            runtime.fail(HipCall::release, ordinal);
            require_encoder_error<GpuError>([&] { encoder.close_checked(); });
            REQUIRE_EQ(runtime.injected, 1U);
            REQUIRE_EQ(runtime.live_count(), ordinal == 1U ? 2U : 1U);
            const auto retained =
                ordinal == 1U ? plan.device_bytes : rans_statistics_workspace_requirements().device_bytes;
            REQUIRE_EQ(encoder_reserved_bytes(), baseline + retained);
            require_encoder_error<GpuError>([&] { (void)encoder.encode(16U, 8U, 8U, input.source.size()); });
            encoder.close_checked();
        }
        REQUIRE_TRUE(runtime.valid);
        REQUIRE_EQ(runtime.live_count(), 0U);
        REQUIRE_EQ(encoder_reserved_bytes(), baseline);
    }
    input.input.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_encoder_runtime_buffer_snapshots_allocator_pair_before_acquisition) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    HipFaultRuntime runtime;
    auto memory = runtime.calls().memory;
    HipDeviceMemoryReservation reservation(256U, "runtime allocator snapshot fixture");
    HipDeviceBuffer<std::uint32_t> storage(256U, "runtime allocator snapshot", memory);
    const auto pointer = storage.get();
    REQUIRE_TRUE(pointer != nullptr);
    memory = {nullptr, nullptr, nullptr};
    require_encoder_error<GpuError>([&] { storage.allocate_checked(256U, "reject live-owner replacement"); });
    REQUIRE_EQ(runtime.counts[static_cast<std::size_t>(HipCall::allocate)], 1U);
    REQUIRE_EQ(storage.get(), pointer);
    storage.reset_checked("free copied allocator pair");
    REQUIRE_EQ(runtime.live_count(), 0U);
    REQUIRE_TRUE(runtime.valid);
    require_encoder_error<GpuError>([&] { HipDeviceBuffer<std::byte> invalid(1U, "invalid allocator pair", memory); });
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_encoder_runtime_incomplete_dispatch_is_rejected_before_acquisition) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    EncoderFaultInput input(513U);
    const auto baseline = encoder_reserved_bytes();
    using Damage = void (*)(HipRuntimeCalls&);
    const std::array<Damage, 6U> damage{[](HipRuntimeCalls& calls) { calls.memory.allocate = nullptr; },
                                        [](HipRuntimeCalls& calls) { calls.memory.release = nullptr; },
                                        [](HipRuntimeCalls& calls) { calls.copy = nullptr; },
                                        [](HipRuntimeCalls& calls) { calls.clear = nullptr; },
                                        [](HipRuntimeCalls& calls) { calls.launch = nullptr; },
                                        [](HipRuntimeCalls& calls) { calls.synchronize = nullptr; }};
    for (const auto invalidate : damage) {
        HipFaultRuntime runtime;
        auto calls = runtime.calls();
        invalidate(calls);
        require_encoder_error<GpuError>([&] { RansEncoder encoder(input.resident(), nullptr, {}, calls); });
        REQUIRE_EQ(runtime.counts, (std::array<std::size_t, kHipCallKinds>{}));
        REQUIRE_EQ(runtime.live_count(), 0U);
        REQUIRE_EQ(encoder_reserved_bytes(), baseline);
        REQUIRE_TRUE(runtime.valid);
    }
    input.input.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_encoder_actual_counts_models_and_streams_cover_all_options) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    const auto source = encoder_source(33025U);
    HipDeviceMemoryReservation input_reservation(source.size() + 2U, "rANS encoder fixture input");
    CountDeviceBuffer<std::byte> input(static_cast<std::uint32_t>(source.size()));
    input.reset(source);
    const auto before = encoder_reserved_bytes();
    GpuTelemetry telemetry{GpuObservationMode::CountersOnly};
    RansEncoder encoder({input.data(), source.size()}, &telemetry);
    const auto plan = rans_encode_workspace_requirements(source.size());
    REQUIRE_EQ(encoder_reserved_bytes(), before + plan.device_bytes);
    REQUIRE_EQ(snapshot_gpu_telemetry(telemetry).device_allocation_bytes, plan.device_bytes);
    std::size_t accepted = 0U;
    for (const auto shift : std::array{8U, 9U, 10U, 11U, 12U}) {
        for (const auto bits : std::array{0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}) {
            for (const auto precision : std::array{8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U}) {
                const auto payload = encoder.encode(precision, bits, shift, source.size());
                REQUIRE_TRUE(payload.has_value());
                REQUIRE_TRUE(payload->size() < source.size());
                require_encoder_readback(*payload, source, precision == 16U && (bits == 0U || bits == 8U));
                ++accepted;
            }
        }
    }
    REQUIRE_EQ(accepted, 405U);
    REQUIRE_EQ(snapshot_gpu_telemetry(telemetry).device_allocation_bytes, plan.device_bytes);
    REQUIRE_EQ(input.read(static_cast<std::uint32_t>(source.size())), source);
    encoder.close_checked();
    REQUIRE_EQ(encoder_reserved_bytes(), before);
    input.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_encoder_singleton_bytes_strict_ties_and_unconditional_model_reuse) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    const std::vector<std::byte> source(513U, std::byte{0xFF});
    HipDeviceMemoryReservation input_reservation(source.size() + 2U, "rANS singleton encoder input");
    CountDeviceBuffer<std::byte> input(static_cast<std::uint32_t>(source.size()));
    input.reset(source);
    GpuTelemetry telemetry{GpuObservationMode::CountersOnly};
    RansEncoder encoder({input.data(), source.size()}, &telemetry);
    const auto expected = rans_singleton_payload(513U, 16U, 0U, 8U, 255U);
    const auto payload = encoder.encode(16U, 0U, 8U, source.size());
    REQUIRE_TRUE(payload.has_value());
    REQUIRE_EQ(*payload, expected);
    const auto retained = *payload;
    REQUIRE_TRUE(!encoder.encode(16U, 0U, 8U, payload->size()).has_value());
    REQUIRE_EQ(*payload, retained);
    const auto greater_budget = encoder.encode(16U, 0U, 8U, payload->size() + 1U);
    REQUIRE_TRUE(greater_budget.has_value());
    REQUIRE_EQ(*greater_budget, expected);
    const auto before = snapshot_gpu_telemetry(telemetry);
    const auto wider_segments = encoder.encode(16U, 0U, 12U, source.size());
    REQUIRE_TRUE(wider_segments.has_value());
    REQUIRE_EQ(*wider_segments, rans_singleton_payload(513U, 16U, 0U, 12U, 255U));
    // One encode and one compaction, with no count/preparation/normalization/CDF work.
    REQUIRE_EQ(snapshot_gpu_telemetry(telemetry).kernel_launches - before.kernel_launches, 2U);
    require_encoder_readback(*wider_segments, source, true);
    encoder.close_checked();
    input.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_encoder_invalid_options_preserve_caches_and_minimum_bound_avoids_work) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    const std::vector<std::byte> source(513U, std::byte{0xFF});
    HipDeviceMemoryReservation input_reservation(source.size() + 2U, "rANS invalid encoder input");
    CountDeviceBuffer<std::byte> input(static_cast<std::uint32_t>(source.size()));
    input.reset(source);
    GpuTelemetry telemetry{GpuObservationMode::CountersOnly};
    const auto before = encoder_reserved_bytes();
    require_encoder_error<ArchiveError>([&] { RansEncoder invalid({}, &telemetry); });
    REQUIRE_EQ(encoder_reserved_bytes(), before);
    RansEncoder encoder({input.data(), source.size()}, &telemetry);
    REQUIRE_TRUE(encoder.encode(16U, 0U, 8U, source.size()).has_value());
    const auto cached = snapshot_gpu_telemetry(telemetry);
    for (const auto options : std::array{std::array{7U, 0U, 8U}, std::array{17U, 0U, 8U}, std::array{16U, 9U, 8U},
                                         std::array{16U, 0U, 7U}, std::array{16U, 0U, 13U}}) {
        require_encoder_error<ArchiveError>(
            [&] { (void)encoder.encode(options[0], options[1], options[2], source.size()); });
    }
    require_encoder_error<ArchiveError>([&] { (void)encoder.encode(16U, 0U, 8U, source.size() + 1U); });
    REQUIRE_TRUE(!encoder.encode(16U, 0U, 8U, 0U).has_value());
    REQUIRE_TRUE(!encoder.encode(16U, 0U, 8U, 35U).has_value());
    REQUIRE_EQ(snapshot_gpu_telemetry(telemetry).kernel_launches, cached.kernel_launches);
    REQUIRE_EQ(snapshot_gpu_telemetry(telemetry).h2d_bytes, cached.h2d_bytes);
    REQUIRE_TRUE(encoder.encode(16U, 0U, 8U, source.size()).has_value());
    REQUIRE_EQ(snapshot_gpu_telemetry(telemetry).kernel_launches - cached.kernel_launches, 2U);
    encoder.close_checked();
    encoder.close_checked();
    REQUIRE_EQ(encoder_reserved_bytes(), before);
    require_encoder_error<GpuError>([&] { (void)encoder.encode(16U, 0U, 8U, source.size()); });
    input.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_encoder_losing_stream_bound_stops_before_later_batches_and_reuses_models) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> source(33025U);
    for (std::uint32_t i = 0U; i < source.size(); ++i) {
        source[i] = static_cast<std::byte>(i & 255U);
    }
    HipDeviceMemoryReservation input_reservation(source.size() + 2U, "rANS losing encoder input");
    CountDeviceBuffer<std::byte> input(static_cast<std::uint32_t>(source.size()));
    input.reset(source);
    GpuTelemetry telemetry{GpuObservationMode::CountersOnly};
    RansEncoder encoder({input.data(), source.size()}, &telemetry);
    REQUIRE_TRUE(!encoder.encode(8U, 0U, 8U, source.size()).has_value());
    const auto before = snapshot_gpu_telemetry(telemetry);
    REQUIRE_TRUE(!encoder.encode(8U, 0U, 8U, source.size()).has_value());
    // The first 128 complete streams plus remaining minimum states already
    // lose. Do not encode the last two segments or launch dense compaction.
    REQUIRE_EQ(snapshot_gpu_telemetry(telemetry).kernel_launches - before.kernel_launches, 1U);
    const auto contextual = encoder.encode(8U, 8U, 8U, source.size());
    REQUIRE_TRUE(contextual.has_value());
    require_encoder_readback(*contextual, source, true);
    REQUIRE_EQ(input.read(static_cast<std::uint32_t>(source.size())), source);
    encoder.close_checked();
    input.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_encoder_completed_checkpoints_cancel_discard_and_recover) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    const auto source = encoder_source(33025U);
    HipDeviceMemoryReservation input_reservation(source.size() + 2U, "rANS cancellation encoder input");
    CountDeviceBuffer<std::byte> input(static_cast<std::uint32_t>(source.size()));
    input.reset(source);
    const auto before = encoder_reserved_bytes();
    std::uint32_t boundaries = 0U;
    {
        RansEncoder control({input.data(), source.size()}, nullptr);
        REQUIRE_TRUE(control
                         .encode(16U, 8U, 8U, source.size(),
                                 [&] {
                                     REQUIRE_EQ(hipStreamQuery(hipStreamPerThread), hipSuccess);
                                     ++boundaries;
                                 })
                         .has_value());
        control.close_checked();
    }
    REQUIRE_TRUE(boundaries > 5U && boundaries < 64U);
    for (std::uint32_t stop = 1U; stop <= boundaries; ++stop) {
        RansEncoder encoder({input.data(), source.size()}, nullptr);
        std::uint32_t observed = 0U;
        require_encoder_error<GpuError>([&] {
            (void)encoder.encode(16U, 8U, 8U, source.size(), [&] {
                REQUIRE_EQ(hipStreamQuery(hipStreamPerThread), hipSuccess);
                if (++observed == stop) {
                    throw GpuError("controlled rANS encoder cancellation");
                }
            });
        });
        REQUIRE_EQ(observed, stop);
        const auto recovered = encoder.encode(16U, 8U, 8U, source.size());
        REQUIRE_TRUE(recovered.has_value());
        require_encoder_readback(*recovered, source, false);
        encoder.close_checked();
        REQUIRE_EQ(encoder_reserved_bytes(), before);
    }
    REQUIRE_EQ(input.read(static_cast<std::uint32_t>(source.size())), source);
    input.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_encoder_maximum_source_and_odd_alignment_preserve_independent_readback) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    for (const auto bytes : std::array{257U, 4096U, 4097U, 4098U, 4099U, kRansCountMaxBytes}) {
        const std::vector<std::byte> source(bytes, std::byte{0xFF});
        HipDeviceMemoryReservation input_reservation(source.size() + 2U, "rANS maximum encoder input");
        CountDeviceBuffer<std::byte> input(bytes);
        input.reset(source);
        const auto before = encoder_reserved_bytes();
        RansEncoder encoder({input.data(), source.size()}, nullptr);
        const auto payload = encoder.encode(16U, 8U, 12U, source.size());
        REQUIRE_TRUE(payload.has_value());
        require_encoder_readback(*payload, source, true);
        encoder.close_checked();
        REQUIRE_EQ(encoder_reserved_bytes(), before);
        REQUIRE_EQ(input.read(bytes), source);
        input.release_checked();
    }
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}
