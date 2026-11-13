#include "test_util.hpp"
#include "rans_fixtures.hpp"

#include "core/rans_block.hpp"
#include "gpu/hip_device.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#if FLITZZIP_ENABLE_HIP
#include "gpu/hip_kernel_api.hpp"

namespace {
using namespace flitzzip;
using namespace flitzzip::hip_detail;
constexpr std::byte kGuard{0xA5};
constexpr auto kStatusGuard = static_cast<RansDecodeStatus>(0xA5A5A5A5U);

struct RansKernelFixture {
    std::vector<std::byte> input;
    std::vector<std::uint32_t> models;
    std::vector<RansDecodeSegment> plans;
    std::uint32_t decoded;
    std::uint32_t precision;
    std::uint32_t contexts;
};

// Use the production admission boundary, then pass its exact immutable CDF and
// stream views to the device. Expected decoded bytes come from independent
// literal fixtures or the singleton state identity, never the CPU decoder.
RansKernelFixture make_rans_kernel_fixture(std::span<const std::byte> payload, std::uint32_t decoded) {
    const GpuRansBlockView view(payload, decoded);
    RansKernelFixture fixture{{}, {}, {}, decoded, view.precision(), view.context_bits()};
    fixture.input.resize(payload.size() + 64U, kGuard);
    std::ranges::copy(payload, fixture.input.begin() + 32U);
    fixture.models.resize(view.model_workspace_words() + 2U, 0xA5A5A5A5U);
    view.prepare_models(std::span(fixture.models).subspan(1U, view.model_workspace_words()));
    for (std::uint32_t segment = 0U; segment < view.segment_count(); ++segment) {
        const auto stream = view.stream(segment);
        const auto offset = segment << view.segment_shift();
        fixture.plans.push_back({static_cast<std::uint32_t>(stream.data() - payload.data()) + 32U,
                                 static_cast<std::uint32_t>(stream.size()), 32U + offset,
                                 std::min(1U << view.segment_shift(), decoded - offset)});
    }
    return fixture;
}

struct RansKernelResult {
    std::vector<std::byte> output;
    std::vector<RansDecodeStatus> statuses;
};

// Own initialized allocations through synchronous completion and full readback.
// Overlaunch exercises inactive lanes. Source/model/plan/status/output guards
// are checked across their complete extents, including rejected-segment cases.
RansKernelResult run_rans_kernel(const RansKernelFixture& fixture, std::uint32_t active_count) {
    REQUIRE_TRUE(active_count <= fixture.plans.size());
    const auto& api = hip_kernel_api();
    RansKernelResult result{std::vector<std::byte>(fixture.decoded + 64U, kGuard),
                            std::vector<RansDecodeStatus>(fixture.plans.size() + 2U, kStatusGuard)};
    auto input_bytes = static_cast<std::uint32_t>(fixture.input.size());
    const auto model_bytes = fixture.models.size() * sizeof(std::uint32_t);
    const auto plan_bytes = fixture.plans.size() * sizeof(RansDecodeSegment);
    const auto status_bytes = result.statuses.size() * sizeof(RansDecodeStatus);
    HipDeviceMemoryReservation reservation(input_bytes + model_bytes + plan_bytes + status_bytes + result.output.size(),
                                           "rANS kernel test");
    HipDeviceBuffer<std::byte> input(input_bytes, "rANS test input");
    HipDeviceBuffer<std::uint32_t> models(model_bytes, "rANS test models");
    HipDeviceBuffer<RansDecodeSegment> plans(plan_bytes, "rANS test plans");
    HipDeviceBuffer<std::byte> output(result.output.size(), "rANS test output");
    HipDeviceBuffer<RansDecodeStatus> statuses(status_bytes, "rANS test statuses");
    REQUIRE_EQ(hipMemcpy(input.get(), fixture.input.data(), input_bytes, hipMemcpyHostToDevice), hipSuccess);
    REQUIRE_EQ(hipMemcpy(models.get(), fixture.models.data(), model_bytes, hipMemcpyHostToDevice), hipSuccess);
    REQUIRE_EQ(hipMemcpy(plans.get(), fixture.plans.data(), plan_bytes, hipMemcpyHostToDevice), hipSuccess);
    REQUIRE_EQ(hipMemcpy(output.get(), result.output.data(), result.output.size(), hipMemcpyHostToDevice), hipSuccess);
    REQUIRE_EQ(hipMemcpy(statuses.get(), result.statuses.data(), status_bytes, hipMemcpyHostToDevice), hipSuccess);
    const auto* from = input.get();
    const auto* cdf = models.get() + 1U;
    auto words = static_cast<std::uint32_t>(fixture.models.size() - 2U);
    const auto* descriptors = plans.get();
    auto* to = output.get();
    auto output_bytes = static_cast<std::uint32_t>(result.output.size());
    auto* status = statuses.get() + 1U;
    auto precision = fixture.precision;
    auto contexts = fixture.contexts;
    void* arguments[]{&from,      &input_bytes, &cdf, &words,        &descriptors, &active_count,
                      &precision, &contexts,    &to,  &output_bytes, &status};
    REQUIRE_EQ(hipLaunchKernel(reinterpret_cast<const void*>(api.decode_rans_segments), dim3(active_count / 64U + 1U),
                               dim3(64U), arguments, 0, nullptr),
               hipSuccess);
    REQUIRE_EQ(hipDeviceSynchronize(), hipSuccess);
    REQUIRE_EQ(hipMemcpy(result.output.data(), output.get(), result.output.size(), hipMemcpyDeviceToHost), hipSuccess);
    REQUIRE_EQ(hipMemcpy(result.statuses.data(), statuses.get(), status_bytes, hipMemcpyDeviceToHost), hipSuccess);
    std::vector<std::byte> actual_input(fixture.input.size());
    std::vector<std::uint32_t> actual_models(fixture.models.size());
    std::vector<RansDecodeSegment> actual_plans(fixture.plans.size());
    REQUIRE_EQ(hipMemcpy(actual_input.data(), input.get(), input_bytes, hipMemcpyDeviceToHost), hipSuccess);
    REQUIRE_EQ(hipMemcpy(actual_models.data(), models.get(), model_bytes, hipMemcpyDeviceToHost), hipSuccess);
    REQUIRE_EQ(hipMemcpy(actual_plans.data(), plans.get(), plan_bytes, hipMemcpyDeviceToHost), hipSuccess);
    REQUIRE_EQ(actual_input, fixture.input);
    REQUIRE_EQ(actual_models, fixture.models);
    REQUIRE_TRUE(std::ranges::equal(std::as_bytes(std::span(actual_plans)), std::as_bytes(std::span(fixture.plans))));
    REQUIRE_EQ(result.statuses.front(), kStatusGuard);
    REQUIRE_EQ(result.statuses.back(), kStatusGuard);
    REQUIRE_TRUE(std::ranges::all_of(std::span(result.output).first(32U), [](auto b) { return b == kGuard; }));
    REQUIRE_TRUE(std::ranges::all_of(std::span(result.output).last(32U), [](auto b) { return b == kGuard; }));
    statuses.reset_checked("release rANS test statuses");
    output.reset_checked("release rANS test output");
    plans.reset_checked("release rANS test plans");
    models.reset_checked("release rANS test models");
    input.reset_checked("release rANS test input");
    return result;
}

void require_rans_success(const RansKernelFixture& fixture, unsigned int pattern) {
    const auto result = run_rans_kernel(fixture, static_cast<std::uint32_t>(fixture.plans.size()));
    for (std::size_t i = 1U; i <= fixture.plans.size(); ++i) {
        REQUIRE_EQ(result.statuses[i], RansDecodeStatus::success);
    }
    for (std::uint32_t i = 0U; i < fixture.decoded; ++i) {
        const auto expected = pattern == 2U || (pattern == 1U && i % 2U != 0U) ? std::byte{255} : std::byte{0};
        REQUIRE_EQ(result.output[32U + i], expected);
    }
}
}  // namespace
#endif

TEST_CASE(gpu_rans_kernel_independent_frames_and_full_width_models) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    REQUIRE_EQ(hip_kernel_api().abi_version, HipKernelApi{}.abi_version);
    REQUIRE_EQ(hip_kernel_api().abi_bytes, sizeof(HipKernelApi));
    require_rans_success(make_rans_kernel_fixture(flitzzip_test::kRansZero16, 32U), 0U);
    require_rans_success(make_rans_kernel_fixture(flitzzip_test::kRansBinary8, 128U), 1U);
    require_rans_success(make_rans_kernel_fixture(flitzzip_test::kRansSegmented12, 513U), 1U);
    const auto fixture = make_rans_kernel_fixture(flitzzip_test::kRansSingleton8, 513U);
    require_rans_success(fixture, 2U);
    const auto inactive = run_rans_kernel(fixture, 0U);
    REQUIRE_TRUE(std::ranges::all_of(inactive.output, [](auto b) { return b == kGuard; }));
    REQUIRE_TRUE(std::ranges::all_of(inactive.statuses, [](auto s) { return s == kStatusGuard; }));
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

// A singleton leaves state L unchanged at every precision. One complete
// segment exercises all option widths and the maximum bounded per-lane loop.
TEST_CASE(gpu_rans_kernel_all_options_and_maximum_segment_work) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    for (unsigned int precision = 8U; precision <= 16U; ++precision) {
        for (unsigned int contexts = 0U; contexts <= 8U; ++contexts) {
            for (unsigned int shift = 8U; shift <= 12U; ++shift) {
                auto payload = flitzzip_test::kRansZero16;
                payload[0] = static_cast<std::byte>(precision);
                payload[1] = static_cast<std::byte>(contexts);
                payload[2] = static_cast<std::byte>(shift);
                require_rans_success(make_rans_kernel_fixture(payload, 1U << shift), 0U);
            }
        }
    }
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_kernel_rejects_uncontained_and_unbounded_plans) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    const auto valid = make_rans_kernel_fixture(flitzzip_test::kRansSingleton8, 513U);
    const auto max = std::numeric_limits<std::uint32_t>::max();
    const std::array bad_plans{
        RansDecodeSegment{max, 4U, 32U, 256U}, RansDecodeSegment{0U, max, 32U, 256U},
        RansDecodeSegment{32U, 3U, 32U, 256U}, RansDecodeSegment{0U, 7U, 32U, 1U},
        RansDecodeSegment{32U, 4U, max, 256U}, RansDecodeSegment{32U, 4U, 32U, max},
        RansDecodeSegment{32U, 4U, 32U, 0U},   RansDecodeSegment{32U, 4U, 32U, 4097U},
    };
    for (const auto plan : bad_plans) {
        auto fixture = valid;
        fixture.plans[0] = plan;
        const auto result = run_rans_kernel(fixture, 3U);
        REQUIRE_EQ(result.statuses[1], RansDecodeStatus::invalid_plan);
        REQUIRE_EQ(result.statuses[2], RansDecodeStatus::success);
        REQUIRE_EQ(result.statuses[3], RansDecodeStatus::success);
        REQUIRE_TRUE(
            std::ranges::all_of(std::span(result.output).subspan(32U, 256U), [](auto b) { return b == kGuard; }));
    }
    for (const auto precision : {0U, 7U, 17U, max}) {
        auto fixture = valid;
        fixture.precision = precision;
        REQUIRE_EQ(run_rans_kernel(fixture, 1U).statuses[1], RansDecodeStatus::invalid_plan);
    }
    for (const auto contexts : {9U, 32U, max}) {
        auto fixture = valid;
        fixture.contexts = contexts;
        REQUIRE_EQ(run_rans_kernel(fixture, 1U).statuses[1], RansDecodeStatus::invalid_plan);
    }
    require_rans_success(valid, 2U);
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_kernel_rejects_model_state_and_stream_faults_then_recovers) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    const auto valid = make_rans_kernel_fixture(flitzzip_test::kRansSingleton8, 513U);
    for (const auto row : {0U, 255U, 771U, std::numeric_limits<std::uint32_t>::max()}) {
        auto fixture = valid;
        fixture.models[1] = row;
        REQUIRE_EQ(run_rans_kernel(fixture, 1U).statuses[1], RansDecodeStatus::missing_model);
    }
    for (const auto state : {0U, (1U << 23U) - 1U, 1U << 31U, std::numeric_limits<std::uint32_t>::max()}) {
        auto fixture = valid;
        for (unsigned int i = 0U; i < 4U; ++i) {
            fixture.input[fixture.plans[0].stream_offset + i] = static_cast<std::byte>(state >> (8U * i));
        }
        const auto result = run_rans_kernel(fixture, 3U);
        REQUIRE_EQ(result.statuses[1], RansDecodeStatus::invalid_state);
        REQUIRE_EQ(result.statuses[2], RansDecodeStatus::success);
        REQUIRE_EQ(result.statuses[3], RansDecodeStatus::success);
    }
    auto final_state = valid;
    final_state.input[final_state.plans[0].stream_offset] = std::byte{1};
    REQUIRE_EQ(run_rans_kernel(final_state, 1U).statuses[1], RansDecodeStatus::invalid_final_state);
    auto trailing = valid;
    ++trailing.plans[0].stream_bytes;
    REQUIRE_EQ(run_rans_kernel(trailing, 1U).statuses[1], RansDecodeStatus::invalid_final_state);
    auto truncated = make_rans_kernel_fixture(flitzzip_test::kRansBinary8, 128U);
    truncated.plans[0].stream_bytes = 4U;
    REQUIRE_EQ(run_rans_kernel(truncated, 1U).statuses[1], RansDecodeStatus::truncated_stream);
    auto missing = valid;
    missing.models[1U + 255U] = 0U;
    REQUIRE_EQ(run_rans_kernel(missing, 1U).statuses[1], RansDecodeStatus::missing_model);
    auto bad_mass = valid;
    bad_mass.models[1U + bad_mass.models[1U] + 256U] = 255U;
    REQUIRE_EQ(run_rans_kernel(bad_mass, 1U).statuses[1], RansDecodeStatus::missing_model);
    require_rans_success(valid, 2U);
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}
