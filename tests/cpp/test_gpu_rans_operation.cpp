#include "test_util.hpp"
#include "rans_fixtures.hpp"

#include "core/rans_block.hpp"
#include "core/result.hpp"
#include "gpu/hip_device.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#if FLITZZIP_ENABLE_HIP
#include "gpu/hip_crc.hpp"
#include "gpu/hip_rans.hpp"

namespace {
using namespace flitzzip;
using namespace flitzzip::hip_detail;
constexpr std::size_t kGuardBytes = 32U;
constexpr std::byte kGuard{0xA5};
constexpr std::uint32_t kModelGuard = 0xA5A5A5A5U;

// Independent bitwise IEEE CRC, rather than the production host/device tables.
std::uint32_t rans_fixture_crc(std::span<const std::byte> bytes) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        crc ^= static_cast<std::uint32_t>(byte);
        for (unsigned int bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xEDB88320U : 0U);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

// Own distinct logical device windows with initialized guards. The host payload
// precedes its borrowed parser view; every HIP owner precedes its reservation's
// destruction. Readback and checked release finish before this fixture ends.
class ResidentRansCase {
  public:
    ResidentRansCase(std::span<const std::byte> bytes, std::uint32_t decoded)
        : payload(bytes.begin(), bytes.end()), block(payload, decoded),
          model_storage_(block.model_workspace_words() + 2U, kModelGuard),
          reservation_(payload.size() + decoded + 4U * kGuardBytes, "rANS operation fixture"),
          input_(payload.size() + 2U * kGuardBytes, "rANS operation input"),
          output_(decoded + 2U * kGuardBytes, "rANS operation output") {
        REQUIRE_EQ(hipGetDevice(&device_), hipSuccess);
        auto complete_input = guarded(payload);
        REQUIRE_EQ(
            copy_on_codec_stream(input_.get(), complete_input.data(), complete_input.size(), hipMemcpyHostToDevice),
            hipSuccess);
        reset_output();
    }

    std::span<const std::byte> input_window() const {
        return {input_.get() + kGuardBytes, payload.size()};
    }
    std::span<std::byte> output_window() const {
        return {output_.get() + kGuardBytes, block.decoded_size()};
    }
    std::span<std::uint32_t> models() {
        return std::span(model_storage_).subspan(1U, block.model_workspace_words());
    }
    std::size_t live_reserved_bytes() const {
        return hip_device_reservation_state().reserved_bytes(device_);
    }
    void reset_output() {
        std::vector<std::byte> bytes(block.decoded_size() + 2U * kGuardBytes, kGuard);
        REQUIRE_EQ(copy_on_codec_stream(output_.get(), bytes.data(), bytes.size(), hipMemcpyHostToDevice), hipSuccess);
    }
    void require_output(std::span<const std::byte> expected) const {
        REQUIRE_EQ(expected.size(), block.decoded_size());
        auto complete = guarded(expected);
        std::vector<std::byte> actual(complete.size());
        REQUIRE_EQ(copy_on_codec_stream(actual.data(), output_.get(), actual.size(), hipMemcpyDeviceToHost),
                   hipSuccess);
        REQUIRE_EQ(actual, complete);
    }
    void require_input_and_model_guards() const {
        auto expected = guarded(payload);
        std::vector<std::byte> actual(expected.size());
        REQUIRE_EQ(copy_on_codec_stream(actual.data(), input_.get(), actual.size(), hipMemcpyDeviceToHost), hipSuccess);
        REQUIRE_EQ(actual, expected);
        REQUIRE_EQ(model_storage_.front(), kModelGuard);
        REQUIRE_EQ(model_storage_.back(), kModelGuard);
    }
    std::span<std::byte> overlapping_output() const {
        REQUIRE_TRUE(block.decoded_size() <= payload.size() + kGuardBytes);
        return {input_.get() + kGuardBytes, block.decoded_size()};
    }
    void release_checked() {
        output_.reset_checked("release rANS operation output");
        input_.reset_checked("release rANS operation input");
        reservation_.reduce_to(0U);
    }

    const std::vector<std::byte> payload;
    const GpuRansBlockView block;

  private:
    static std::vector<std::byte> guarded(std::span<const std::byte> inner) {
        std::vector<std::byte> result(inner.size() + 2U * kGuardBytes, kGuard);
        std::ranges::copy(inner, result.begin() + kGuardBytes);
        return result;
    }

    std::vector<std::uint32_t> model_storage_;
    HipDeviceMemoryReservation reservation_;
    HipDeviceBuffer<std::byte> input_;
    HipDeviceBuffer<std::byte> output_;
    int device_ = -1;
};

template <typename Action> void require_rans_operation_rejected(Action action, const char* fragment) {
    bool rejected = false;
    try {
        action();
    } catch (const ArchiveError& error) {
        REQUIRE_TRUE(std::string(error.what()).find(fragment) != std::string::npos);
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

void require_rans_operation_success(ResidentRansCase& fixture, unsigned int pattern) {
    fixture.reset_output();
    GpuTelemetry telemetry{GpuObservationMode::CountersOnly};
    const auto reserved = fixture.live_reserved_bytes();
    decode_rans_block_device(fixture.block, fixture.input_window(), fixture.output_window(), fixture.models(),
                             &telemetry);
    REQUIRE_EQ(fixture.live_reserved_bytes(), reserved);
    const auto stats = snapshot_gpu_telemetry(telemetry);
    const auto segments = fixture.block.segment_count();
    const auto required = rans_decode_workspace_requirements(fixture.block);
    REQUIRE_EQ(stats.kernel_launches, (segments + 127U) / 128U);
    REQUIRE_EQ(stats.device_allocation_bytes, required.device_bytes);
    REQUIRE_EQ(stats.h2d_bytes, required.model_words * sizeof(std::uint32_t) + segments * 20U);
    REQUIRE_EQ(stats.d2h_bytes, segments * 4U);
    REQUIRE_EQ(stats.timing_status, GpuTimingStatus::NotRequested);
    REQUIRE_TRUE(std::isnan(stats.kernel_ms));
    REQUIRE_TRUE(!telemetry.timing_recorded.load());
    std::vector<std::byte> expected(fixture.block.decoded_size());
    for (std::size_t i = 0U; i < expected.size(); ++i) {
        expected[i] = pattern == 2U || (pattern == 1U && i % 2U != 0U) ? std::byte{255} : std::byte{0};
    }
    const auto crc = compute_crc32_device(fixture.output_window().data(), fixture.block.decoded_size(), &telemetry,
                                          "rANS operation decoded device CRC");
    REQUIRE_EQ(crc, rans_fixture_crc(expected));
    fixture.require_output(expected);
    fixture.require_input_and_model_guards();
    REQUIRE_EQ(fixture.live_reserved_bytes(), reserved);
}

struct RansCancellation {
    unsigned int phase;
};
}  // namespace
#endif

TEST_CASE(gpu_rans_operation_workspace_contract_is_exact_and_bounded) {
#if FLITZZIP_ENABLE_HIP
    const GpuRansBlockView small(flitzzip_test::kRansZero16, 32U);
    const auto one = rans_decode_workspace_requirements(small);
    REQUIRE_EQ(one.model_words, 258U);
    REQUIRE_EQ(one.batch_segments, 1U);
    REQUIRE_EQ(one.host_bytes, 3592U);
    REQUIRE_EQ(one.device_bytes, 1052U);
    REQUIRE_EQ(small.payload().data(), flitzzip_test::kRansZero16.data());
    REQUIRE_EQ(small.payload().size(), flitzzip_test::kRansZero16.size());
    const auto bytes = flitzzip_test::rans_singleton_payload(65536U, 16U, 8U, 8U, 255U, true);
    const GpuRansBlockView maximum(bytes, 65536U);
    const auto many = rans_decode_workspace_requirements(maximum);
    REQUIRE_EQ(many.model_words, 66048U);
    REQUIRE_EQ(many.batch_segments, 128U);
    REQUIRE_EQ(many.host_bytes, 266752U);
    REQUIRE_EQ(many.device_bytes, 266752U);
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_operation_independent_bytes_device_crc_and_dispatch_boundaries) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    struct Frame {
        std::span<const std::byte> payload;
        std::uint32_t decoded;
        unsigned int pattern;
    };
    const std::array frames{Frame{flitzzip_test::kRansZero16, 32U, 0U}, Frame{flitzzip_test::kRansBinary8, 128U, 1U},
                            Frame{flitzzip_test::kRansSegmented12, 513U, 1U},
                            Frame{flitzzip_test::kRansSingleton8, 513U, 2U}};
    for (const auto& frame : frames) {
        ResidentRansCase fixture(frame.payload, frame.decoded);
        require_rans_operation_success(fixture, frame.pattern);
        fixture.release_checked();
    }
    for (const std::uint32_t decoded : {32U, 255U, 256U, 257U, 16384U, 16385U, 32768U, 32769U, 65537U}) {
        const auto bytes = flitzzip_test::rans_singleton_payload(decoded, 16U, 8U, 8U);
        ResidentRansCase fixture(bytes, decoded);
        require_rans_operation_success(fixture, 0U);
        fixture.release_checked();
    }
    // Context255 reaches the final CDF word; singleton mass65536 must remain full-width there.
    const auto bytes = flitzzip_test::rans_singleton_payload(65536U, 16U, 8U, 8U, 255U, true);
    ResidentRansCase maximum(bytes, 65536U);
    require_rans_operation_success(maximum, 2U);
    maximum.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_operation_checkpoints_see_completed_windows_and_recover) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    constexpr std::uint32_t decoded = 33025U;
    const auto payload = flitzzip_test::rans_singleton_payload(decoded, 16U, 0U, 8U);
    ResidentRansCase fixture(payload, decoded);
    const auto reserved = fixture.live_reserved_bytes();
    for (const unsigned int cancel_phase : {1U, 2U, 3U, 4U}) {
        fixture.reset_output();
        GpuTelemetry telemetry{GpuObservationMode::CountersOnly};
        unsigned int calls = 0U;
        bool cancelled = false;
        try {
            decode_rans_block_device(fixture.block, fixture.input_window(), fixture.output_window(), fixture.models(),
                                     &telemetry, [&] {
                                         ++calls;
                                         const auto completed = calls <= 2U ? 0U : calls == 3U ? 32768U : decoded;
                                         std::vector<std::byte> expected(decoded, kGuard);
                                         std::fill_n(expected.begin(), completed, std::byte{0});
                                         fixture.require_output(expected);
                                         if (calls == cancel_phase) {
                                             throw RansCancellation{calls};
                                         }
                                     });
        } catch (const RansCancellation& error) {
            REQUIRE_EQ(error.phase, cancel_phase);
            cancelled = true;
        }
        REQUIRE_TRUE(cancelled);
        REQUIRE_EQ(calls, cancel_phase);
        REQUIRE_EQ(fixture.live_reserved_bytes(), reserved);
        REQUIRE_EQ(snapshot_gpu_telemetry(telemetry).kernel_launches, cancel_phase <= 2U   ? 0U
                                                                      : cancel_phase == 3U ? 1U
                                                                                           : 2U);
        fixture.require_input_and_model_guards();
        require_rans_operation_success(fixture, 0U);
    }
    fixture.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_operation_rejects_later_corruption_without_publishing_or_retaining_scratch) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    constexpr std::uint32_t decoded = 65537U;
    auto payload = flitzzip_test::rans_singleton_payload(decoded, 16U, 0U, 8U);
    // The temporary view expires before mutation; the operation gets a fresh
    // admitted immutable view over the deliberately invalid coded state.
    const auto offset =
        static_cast<std::size_t>(GpuRansBlockView(payload, decoded).stream(128U).data() - payload.data());
    std::fill_n(payload.begin() + offset, 4U, std::byte{0});
    ResidentRansCase fixture(payload, decoded);
    GpuTelemetry telemetry{GpuObservationMode::CountersOnly};
    const auto reserved = fixture.live_reserved_bytes();
    require_rans_operation_rejected(
        [&] {
            decode_rans_block_device(fixture.block, fixture.input_window(), fixture.output_window(), fixture.models(),
                                     &telemetry);
        },
        "failed GPU decoding validation");
    REQUIRE_EQ(snapshot_gpu_telemetry(telemetry).kernel_launches, 2U);
    REQUIRE_EQ(fixture.live_reserved_bytes(), reserved);
    std::vector<std::byte> expected(decoded, kGuard);
    for (std::uint32_t segment = 0U; segment < 256U; ++segment) {
        if (segment != 128U) {
            std::fill_n(expected.begin() + segment * 256U, 256U, std::byte{0});
        }
    }
    fixture.require_output(expected);
    fixture.require_input_and_model_guards();
    fixture.release_checked();
    ResidentRansCase recovered(flitzzip_test::kRansSegmented12, 513U);
    require_rans_operation_success(recovered, 1U);
    recovered.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_operation_rejects_extent_alias_and_scratch_errors_before_device_work) {
#if FLITZZIP_ENABLE_HIP
    if (!query_gpu_info().available) {
        return;
    }
    ResidentRansCase fixture(flitzzip_test::kRansZero16, 32U);
    GpuTelemetry telemetry{GpuObservationMode::CountersOnly};
    const auto reserved = fixture.live_reserved_bytes();
    const std::array<std::function<void()>, 5> rejected{
        [&] { decode_rans_block_device(fixture.block, {}, fixture.output_window(), fixture.models(), &telemetry); },
        [&] { decode_rans_block_device(fixture.block, fixture.input_window(), {}, fixture.models(), &telemetry); },
        [&] {
            decode_rans_block_device(fixture.block, fixture.input_window().first(18U), fixture.output_window(),
                                     fixture.models(), &telemetry);
        },
        [&] {
            decode_rans_block_device(fixture.block, fixture.input_window(), fixture.output_window().first(31U),
                                     fixture.models(), &telemetry);
        },
        [&] {
            decode_rans_block_device(fixture.block, fixture.input_window(), fixture.overlapping_output(),
                                     fixture.models(), &telemetry);
        },
    };
    for (const auto& action : rejected) {
        require_rans_operation_rejected(action, "extent or ownership");
    }
    require_rans_operation_rejected(
        [&] {
            decode_rans_block_device(fixture.block, fixture.input_window(), fixture.output_window(),
                                     fixture.models().first(1U), &telemetry);
        },
        "workspace");
    const auto stats = snapshot_gpu_telemetry(telemetry);
    REQUIRE_EQ(stats.kernel_launches, 0U);
    REQUIRE_EQ(stats.device_allocation_bytes, 0U);
    REQUIRE_EQ(stats.h2d_bytes, 0U);
    REQUIRE_EQ(stats.d2h_bytes, 0U);
    REQUIRE_EQ(fixture.live_reserved_bytes(), reserved);
    fixture.require_output(std::vector<std::byte>(32U, kGuard));
    fixture.require_input_and_model_guards();
    require_rans_operation_success(fixture, 0U);
    fixture.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}
