#include "gpu/hip_rans.hpp"

#include "core/byte_region.hpp"
#include "gpu/hip_kernel_api.hpp"

#include <algorithm>
#include <array>

namespace flitzzip::hip_detail {
namespace {

constexpr unsigned int kDecodeThreads = 64U;
constexpr std::size_t kHostBatchBytes =
    kRansDecodeBatchSegments * (sizeof(RansDecodeSegment) + sizeof(RansDecodeStatus));

// One charged allocation gives models and reusable batch metadata the same
// completion lifetime. Each byte offset preserves the following POD alignment.
struct RansDecodeScratch {
    HipDeviceMemoryReservation reservation;
    HipDeviceBuffer<std::byte> storage;
    std::uint32_t* models;
    RansDecodeSegment* plans;
    RansDecodeStatus* statuses;

    RansDecodeScratch(const RansDecodeWorkspaceRequirements& required, GpuTelemetry* telemetry)
        : reservation(required.device_bytes, "rANS decode scratch"),
          storage(required.device_bytes, "allocate rANS decode scratch") {
        static_assert(alignof(RansDecodeSegment) <= alignof(std::uint32_t));
        static_assert(alignof(RansDecodeStatus) <= alignof(std::uint32_t));
        auto* next = storage.get();
        models = reinterpret_cast<std::uint32_t*>(next);
        next += required.model_words * sizeof(std::uint32_t);
        plans = reinterpret_cast<RansDecodeSegment*>(next);
        next += required.batch_segments * sizeof(RansDecodeSegment);
        statuses = reinterpret_cast<RansDecodeStatus*>(next);
        record_gpu_device_allocation_bytes(telemetry, required.device_bytes);
    }
};

// The parser already proved dense streams and bounded decoded segments. Build
// only this batch's exact plans; no table proportional to the whole block is kept.
void prepare_rans_batch(const GpuRansBlockView& block, std::uint32_t first, std::span<RansDecodeSegment> plans) {
    for (std::uint32_t i = 0U; i < plans.size(); ++i) {
        const auto segment = first + i;
        const auto stream = block.stream(segment);
        const auto output_offset = segment << block.segment_shift();
        plans[i] = {static_cast<std::uint32_t>(stream.data() - block.payload().data()),
                    static_cast<std::uint32_t>(stream.size()), output_offset,
                    std::min(1U << block.segment_shift(), block.decoded_size() - output_offset)};
    }
}

void run_rans_batch(const GpuRansBlockView& block, std::span<const std::byte> device_payload,
                    std::span<std::byte> device_output, std::span<const RansDecodeSegment> plans,
                    std::span<RansDecodeStatus> statuses, RansDecodeScratch& scratch, GpuTelemetry* telemetry) {
    const auto plan_bytes = plans.size_bytes();
    const auto status_bytes = statuses.size_bytes();
    // A fresh non-success status for every reuse prevents old success metadata
    // from admitting an omitted or incomplete later dispatch.
    std::ranges::fill(statuses, RansDecodeStatus::invalid_plan);
    check_hip(copy_on_codec_stream(scratch.plans, plans.data(), plan_bytes, hipMemcpyHostToDevice),
              "upload rANS decode plans");
    check_hip(copy_on_codec_stream(scratch.statuses, statuses.data(), status_bytes, hipMemcpyHostToDevice),
              "initialize rANS decode statuses");
    record_gpu_h2d_bytes(telemetry, plan_bytes + status_bytes);
    const auto count = static_cast<std::uint32_t>(plans.size());
    dispatch_codec_kernel(
        telemetry, hip_kernel_api().decode_rans_segments, (count + kDecodeThreads - 1U) / kDecodeThreads,
        kDecodeThreads, "rANS segment decode", device_payload.data(), static_cast<std::uint32_t>(device_payload.size()),
        static_cast<const std::uint32_t*>(scratch.models), static_cast<std::uint32_t>(block.model_workspace_words()),
        static_cast<const RansDecodeSegment*>(scratch.plans), count, static_cast<std::uint32_t>(block.precision()),
        static_cast<std::uint32_t>(block.context_bits()), device_output.data(),
        static_cast<std::uint32_t>(device_output.size()), scratch.statuses);
    check_hip(copy_on_codec_stream(statuses.data(), scratch.statuses, status_bytes, hipMemcpyDeviceToHost),
              "download rANS decode statuses");
    record_gpu_d2h_bytes(telemetry, status_bytes);
    if (std::ranges::any_of(statuses, [](auto status) { return status != RansDecodeStatus::success; })) {
        throw ArchiveError("rANS block failed GPU decoding validation");
    }
}

}  // namespace

RansDecodeWorkspaceRequirements rans_decode_workspace_requirements(const GpuRansBlockView& block) noexcept {
    const auto words = block.model_workspace_words();
    const auto batch = std::min(block.segment_count(), kRansDecodeBatchSegments);
    const auto model_bytes = words * sizeof(std::uint32_t);
    return {words, batch, model_bytes + kHostBatchBytes,
            model_bytes + batch * (sizeof(RansDecodeSegment) + sizeof(RansDecodeStatus))};
}

void decode_rans_block_device(const GpuRansBlockView& block, std::span<const std::byte> device_payload,
                              std::span<std::byte> device_output, std::span<std::uint32_t> host_models,
                              GpuTelemetry* telemetry, const std::function<void()>& checkpoint) {
    if (device_payload.size() != block.payload().size() || device_output.size() != block.decoded_size() ||
        byte_regions_overlap(device_payload, device_output)) {
        throw ArchiveError("rANS device decode extent or ownership is invalid");
    }
    if (checkpoint) {
        checkpoint();
    }
    block.prepare_models(host_models);
    require_hip_kernel_device_support();
    const auto required = rans_decode_workspace_requirements(block);
    RansDecodeScratch scratch(required, telemetry);
    std::array<RansDecodeSegment, kRansDecodeBatchSegments> plans{};
    std::array<RansDecodeStatus, kRansDecodeBatchSegments> statuses{};
    try {
        const auto model_bytes = required.model_words * sizeof(std::uint32_t);
        check_hip(copy_on_codec_stream(scratch.models, host_models.data(), model_bytes, hipMemcpyHostToDevice),
                  "upload rANS decode models");
        record_gpu_h2d_bytes(telemetry, model_bytes);
        for (std::uint32_t first = 0U; first < block.segment_count();) {
            if (checkpoint) {
                checkpoint();
            }
            const auto count = std::min(required.batch_segments, block.segment_count() - first);
            auto batch_plans = std::span(plans).first(count);
            prepare_rans_batch(block, first, batch_plans);
            run_rans_batch(block, device_payload, device_output, batch_plans, std::span(statuses).first(count), scratch,
                           telemetry);
            first += count;
        }
        if (checkpoint) {
            checkpoint();
        }
    } catch (...) {
        // Preserve the first error. A failed drain is never a successful decode
        // or permission to reuse scratch; owners still unwind on this thread.
        (void)hipStreamSynchronize(hipStreamPerThread);
        throw;
    }
    scratch.storage.reset_checked("free rANS decode scratch");
}

}  // namespace flitzzip::hip_detail
