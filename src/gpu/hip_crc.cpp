#include "gpu/hip_crc.hpp"
#include "gpu/hip_kernel_api.hpp"
#include "core/checksum.hpp"

#include <numeric>

namespace superzip::hip_detail {
namespace {

// Purpose: Select the measured small- or large-chunk CRC geometry without changing checksum semantics.
// Inputs: `bytes` is the total source or decoded byte count for one operation.
// Outputs: Returns a bounded 8 KiB or 32 KiB segment size used consistently by host and HIP code.
std::uint32_t crc_segment_bytes_for_size(std::uint64_t bytes) {
    return bytes <= kSmallCrcInputLimitBytes ? kSmallCrcSegmentBytes : kLargeCrcSegmentBytes;
}

// Purpose: Return the number of selected-size CRC segments needed for a device buffer.
// Inputs: `bytes` is the buffer length and `segment_bytes` is the operation's selected geometry.
// Outputs: Returns zero for empty buffers or a bounded segment count for nonempty buffers.
std::uint32_t crc_segment_count(std::uint64_t bytes, std::uint32_t segment_bytes) {
    if (bytes == 0) {
        return 0;
    }
    const auto segments = (bytes + segment_bytes - 1U) / segment_bytes;
    if (segments > std::numeric_limits<std::uint32_t>::max()) {
        throw GpuError("CRC segment count exceeds HIP launch limits");
    }
    return static_cast<std::uint32_t>(segments);
}

// Purpose: Combine ordered CRC segments into one ZIP-compatible finalized CRC-32.
// Inputs: `segments` contains finalized per-segment CRCs and byte lengths in archive order.
// Outputs: Returns the finalized CRC for the concatenated byte stream.
std::uint32_t combine_crc_segments(std::span<const DeviceCrcSegment> segments) {
    std::uint32_t combined = 0;
    for (const auto& segment : segments) {
        combined = crc32_combine(combined, segment.crc32, segment.length);
    }
    return combined;
}

}  // namespace

// Purpose: Launch the HIP CRC segment kernel and return the combined CRC.
// Inputs: `device_input` is an allocated device buffer, `input_len` is its byte length, and `telemetry` records HIP
// activity. Outputs: Returns the finalized CRC-32 while copying only compact segment metadata back to host memory.
std::uint32_t compute_crc32_device(const std::byte* device_input, std::uint64_t input_len, GpuTelemetry* telemetry,
                                   const char* action) {
    if (input_len == 0) {
        return 0;
    }
    const auto crc_segment_bytes = crc_segment_bytes_for_size(input_len);
    const auto segments = crc_segment_count(input_len, crc_segment_bytes);
    const auto segment_bytes = checked_multiply_bytes(segments, sizeof(DeviceCrcSegment), action);
    HipDeviceMemoryReservation reservation(segment_bytes, action);
    HipDeviceBuffer<DeviceCrcSegment> device_segments(segment_bytes, "hipMalloc CRC segments");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(segment_bytes));
    constexpr auto threads = kCrcSegmentThreads;
    const auto grid = segments;
    auto events = make_hip_event_pair("create crc32_segments_kernel events");
    launch_measured_kernel(hip_kernel_api().crc32_segments_kernel, grid, threads, 0, hipStreamPerThread, events,
                           "launch crc32_segments_kernel", device_input, static_cast<std::size_t>(input_len),
                           device_segments.get(), segments, crc_segment_bytes);
    finish_measured_kernel(telemetry, events, "synchronize crc32_segments_kernel");

    std::vector<DeviceCrcSegment> host_segments(segments);
    check_hip(copy_on_codec_stream(host_segments.data(), device_segments.get(), segment_bytes, hipMemcpyDeviceToHost),
              "hipMemcpy CRC segments");
    record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(segment_bytes));
    device_segments.reset_checked("hipFree CRC segments");
    return combine_crc_segments(host_segments);
}

// Purpose: Calculate a GPU source CRC for each independent block in a validated bounded batch.
// Inputs: device_input contains the concatenated blocks; lengths define exact boundaries; telemetry records costs.
// Outputs: Returns ordered block CRCs after one checksum launch; copies back metadata only and releases all scratch.
std::vector<std::uint32_t> compute_block_crc32_device(const std::byte* device_input,
                                                      std::span<const std::uint32_t> lengths, GpuTelemetry* telemetry) {
    std::vector<CrcInputRange> ranges;
    std::size_t offset = 0;
    const auto total_input_bytes = std::accumulate(lengths.begin(), lengths.end(), std::uint64_t{0});
    const auto crc_segment_bytes = crc_segment_bytes_for_size(total_input_bytes);
    for (const auto length : lengths) {
        for (std::uint32_t pos = 0; pos < length;) {
            const auto count = std::min(crc_segment_bytes, length - pos);
            ranges.push_back(CrcInputRange{.offset = offset + pos, .length = count});
            pos += count;
        }
        offset += length;
    }
    if (ranges.empty()) {
        return {};
    }
    const auto range_bytes = checked_multiply_bytes(ranges.size(), sizeof(CrcInputRange), "batch CRC ranges");
    const auto segment_bytes = checked_multiply_bytes(ranges.size(), sizeof(DeviceCrcSegment), "batch CRC results");
    const auto total_bytes = checked_add_bytes(range_bytes, segment_bytes, "batch CRC scratch");
    HipDeviceMemoryReservation reservation(total_bytes, "batch CRC scratch");
    HipDeviceBuffer<CrcInputRange> device_ranges(range_bytes, "hipMalloc batch CRC ranges");
    HipDeviceBuffer<DeviceCrcSegment> device_segments(segment_bytes, "hipMalloc batch CRC results");
    record_gpu_device_allocation_bytes(telemetry, total_bytes);
    check_hip(copy_on_codec_stream(device_ranges.get(), ranges.data(), range_bytes, hipMemcpyHostToDevice),
              "hipMemcpy batch CRC ranges");
    record_gpu_h2d_bytes(telemetry, range_bytes);
    const auto count = static_cast<std::uint32_t>(ranges.size());
    constexpr auto threads = kCrcSegmentThreads;
    const auto grid = count;
    auto events = make_hip_event_pair("create independent CRC events");
    launch_measured_kernel(hip_kernel_api().crc32_independent_ranges_kernel, grid, threads, 0, hipStreamPerThread,
                           events, "launch independent CRC kernel", device_input, device_ranges.get(),
                           device_segments.get(), count);
    finish_measured_kernel(telemetry, events, "synchronize independent CRC kernel");
    std::vector<DeviceCrcSegment> segments(count);
    check_hip(copy_on_codec_stream(segments.data(), device_segments.get(), segment_bytes, hipMemcpyDeviceToHost),
              "hipMemcpy independent CRC results");
    record_gpu_d2h_bytes(telemetry, segment_bytes);
    device_ranges.reset_checked("hipFree batch CRC ranges");
    device_segments.reset_checked("hipFree batch CRC results");
    std::vector<std::uint32_t> checksums;
    checksums.reserve(lengths.size());
    std::size_t first = 0;
    for (const auto length : lengths) {
        const auto segment_count = crc_segment_count(length, crc_segment_bytes);
        checksums.push_back(
            combine_crc_segments(std::span<const DeviceCrcSegment>(segments).subspan(first, segment_count)));
        first += segment_count;
    }
    return checksums;
}

// Purpose: Launch the HIP decoded-stream CRC kernel without materializing decoded output.
// Inputs: `device_payload`/`device_blocks` are allocated device buffers, `block_count` and `output_len` bound the
// decoded layout, and `telemetry` records HIP activity. Outputs: Returns the finalized CRC-32 while copying only
// compact segment metadata back to host memory.
std::uint32_t compute_decoded_crc32_device(const std::byte* device_payload, const DeviceBlock* device_blocks,
                                           std::uint32_t block_count, std::uint64_t output_len, GpuTelemetry* telemetry,
                                           const char* action) {
    if (output_len == 0) {
        return 0;
    }
    const auto crc_segment_bytes = crc_segment_bytes_for_size(output_len);
    const auto segments = crc_segment_count(output_len, crc_segment_bytes);
    const auto segment_bytes = checked_multiply_bytes(segments, sizeof(DeviceCrcSegment), action);
    HipDeviceMemoryReservation reservation(segment_bytes, action);
    HipDeviceBuffer<DeviceCrcSegment> device_segments(segment_bytes, "hipMalloc decoded CRC segments");
    record_gpu_device_allocation_bytes(telemetry, static_cast<std::uint64_t>(segment_bytes));
    constexpr auto threads = kCrcSegmentThreads;
    const auto grid = segments;
    auto events = make_hip_event_pair("create decoded_crc32_segments_kernel events");
    launch_measured_kernel(hip_kernel_api().decoded_crc32_segments_kernel, grid, threads, 0, hipStreamPerThread, events,
                           "launch decoded_crc32_segments_kernel", device_payload, device_blocks, block_count,
                           static_cast<std::size_t>(output_len), device_segments.get(), segments, crc_segment_bytes);
    finish_measured_kernel(telemetry, events, "synchronize decoded_crc32_segments_kernel");

    std::vector<DeviceCrcSegment> host_segments(segments);
    check_hip(copy_on_codec_stream(host_segments.data(), device_segments.get(), segment_bytes, hipMemcpyDeviceToHost),
              "hipMemcpy decoded CRC segments");
    record_gpu_d2h_bytes(telemetry, static_cast<std::uint64_t>(segment_bytes));
    device_segments.reset_checked("hipFree decoded CRC segments");
    return combine_crc_segments(host_segments);
}

}  // namespace superzip::hip_detail
