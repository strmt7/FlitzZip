#pragma once

#include "core/byte_plane_block.hpp"
#include "gpu/hip_kernel_api.hpp"

namespace superzip::hip_detail {

// Purpose: Execute the reversible transform exclusively on HIP with operation-owned admitted buffers.
// Inputs: Equal bounded host spans, admitted width/direction, caller checkpoints and actual device telemetry.
// Outputs: Returns exact transformed bytes after synchronization; errors and cancellation propagate.
inline void transform_byte_planes_hip(std::span<const std::byte> source, std::span<std::byte> destination,
                                      std::uint8_t width, bool inverse, const GpuCodecOptions& options) {
    if (!is_gpu_byte_plane_width(width) || source.size() < width || source.size() > kMaxArchiveBlockBytes ||
        destination.size() != source.size()) {
        throw GpuError("GPU byte-plane transform geometry is invalid");
    }
    if (options.encode_checkpoint) {
        options.encode_checkpoint();
    }
    require_hip_device_ready();
    auto* telemetry = options.telemetry.get();
    const auto allocation_bytes = checked_multiply_bytes(source.size(), 2U, "byte-plane device memory");
    HipDeviceMemoryReservation reservation(allocation_bytes, "byte-plane transform");
    HipDeviceBuffer<std::byte> input(source.size(), "hipMalloc byte-plane input");
    HipDeviceBuffer<std::byte> output(source.size(), "hipMalloc byte-plane output");
    record_gpu_device_allocation_bytes(telemetry, allocation_bytes);
    check_hip(copy_on_codec_stream(input.get(), source.data(), source.size(), hipMemcpyHostToDevice),
              "hipMemcpy byte-plane input");
    record_gpu_h2d_bytes(telemetry, source.size());
    const auto events = make_hip_event_pair("create byte-plane timing events");
    const auto grid = static_cast<unsigned int>((source.size() + 255U) / 256U);
    launch_measured_kernel(hip_kernel_api().byte_plane_transform, grid, 256, 0, hipStreamPerThread, events,
                           "launch byte_plane_transform_kernel", input.get(), output.get(),
                           static_cast<std::uint32_t>(source.size()), static_cast<std::uint32_t>(width), inverse);
    finish_measured_kernel(telemetry, events, "synchronize byte_plane_transform_kernel");
    check_hip(copy_on_codec_stream(destination.data(), output.get(), destination.size(), hipMemcpyDeviceToHost),
              "hipMemcpy byte-plane output");
    record_gpu_d2h_bytes(telemetry, destination.size());
    input.reset_checked("hipFree byte-plane input");
    output.reset_checked("hipFree byte-plane output");
    if (options.encode_checkpoint) {
        options.encode_checkpoint();
    }
}

}  // namespace superzip::hip_detail
