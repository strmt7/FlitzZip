#include "gpu/hip_rans_statistics.hpp"

#include <algorithm>

namespace flitzzip::hip_detail {
namespace {

constexpr std::size_t kStatisticsBytes =
    3U * kRansCountWords * sizeof(std::uint32_t) + kRansProjectionBatch * sizeof(RansCountStatus);
constexpr unsigned int kStatisticsThreads = 256U;

std::span<const std::byte> admit_statistics_source(std::span<const std::byte> source) {
    if (source.empty() || source.data() == nullptr || source.size() > kRansCountMaxBytes) {
        throw ArchiveError("rANS statistics source extent is invalid");
    }
    require_hip_kernel_device_support();
    return source;
}

void statistics_checkpoint(const std::function<void()>& checkpoint) {
    if (checkpoint) {
        checkpoint();
    }
}

// Zero on the same stream as counting. Completion precedes any callback or
// cache admission, including the one-segment case that launches no correction.
void clear_statistics(std::uint32_t* table) {
    check_hip(hipMemsetAsync(table, 0, kRansCountWords * sizeof(std::uint32_t), hipStreamPerThread),
              "zero rANS statistics");
    check_hip(hipStreamSynchronize(hipStreamPerThread), "complete rANS statistics zeroing");
}

}  // namespace

RansStatisticsWorkspaceRequirements rans_statistics_workspace_requirements() noexcept {
    return {sizeof(RansStatistics), kStatisticsBytes};
}

RansStatistics::RansStatistics(std::span<const std::byte> device_source, GpuTelemetry* telemetry,
                               const std::function<void()>& checkpoint)
    : source_(admit_statistics_source(device_source)), telemetry_(telemetry),
      reservation_(kStatisticsBytes, "rANS statistics scratch"),
      storage_(kStatisticsBytes, "allocate rANS statistics scratch"), base_(storage_.get()),
      removed_(base_ + kRansCountWords), projected_(removed_ + kRansCountWords),
      device_statuses_(reinterpret_cast<RansCountStatus*>(projected_ + kRansCountWords)) {
    static_assert(alignof(RansCountStatus) <= alignof(std::uint32_t));
    check_hip(hipGetDevice(&device_), "get rANS statistics device");
    record_gpu_device_allocation_bytes(telemetry_, kStatisticsBytes);
    try {
        statistics_checkpoint(checkpoint);
        clear_statistics(base_);
        count_source(checkpoint);
    } catch (...) {
        (void)hipStreamSynchronize(hipStreamPerThread);
        throw;
    }
}

void RansStatistics::require_current_device() const {
    int current = -1;
    check_hip(hipGetDevice(&current), "get current rANS statistics device");
    if (!reusable_ || current != device_) {
        throw GpuError("rANS statistics operation is closed or has a different current device");
    }
}

void RansStatistics::initialize_statuses(std::uint32_t count) {
    auto batch = std::span(statuses_).first(count);
    std::ranges::fill(batch, RansCountStatus::invalid_plan);
    check_hip(copy_on_codec_stream(device_statuses_, batch.data(), batch.size_bytes(), hipMemcpyHostToDevice),
              "initialize rANS statistics statuses");
    record_gpu_h2d_bytes(telemetry_, batch.size_bytes());
}

void RansStatistics::require_statuses(std::uint32_t count) {
    auto batch = std::span(statuses_).first(count);
    check_hip(copy_on_codec_stream(batch.data(), device_statuses_, batch.size_bytes(), hipMemcpyDeviceToHost),
              "read rANS statistics statuses");
    record_gpu_d2h_bytes(telemetry_, batch.size_bytes());
    if (std::ranges::any_of(batch, [](auto status) { return status != RansCountStatus::success; })) {
        throw ArchiveError("rANS statistics failed GPU validation");
    }
}

void RansStatistics::count_source(const std::function<void()>& checkpoint) {
    const auto bytes = static_cast<std::uint32_t>(source_.size());
    const auto tiles = (bytes - 1U) / kRansCountTileBytes + 1U;
    for (std::uint32_t first = 0U; first < tiles;) {
        const auto count = std::min(kRansCountTilesPerDispatch, tiles - first);
        const auto statuses = count * kRansCountBucketCount;
        initialize_statuses(statuses);
        dispatch_codec_kernel(telemetry_, hip_kernel_api().count_rans_transitions, statuses, kStatisticsThreads,
                              "count rANS source", source_.data(), bytes, first, count, base_, kRansCountWords,
                              device_statuses_, statuses);
        require_statuses(statuses);
        statistics_checkpoint(checkpoint);
        first += count;
    }
}

void RansStatistics::count_boundaries(std::uint32_t shift, const std::function<void()>& checkpoint) {
    clear_statistics(removed_);
    const auto bytes = static_cast<std::uint32_t>(source_.size());
    const auto last = (bytes - 1U) >> shift;
    for (std::uint32_t first = 1U; first <= last;) {
        const auto count = std::min(kRansCorrectionBatch, last - first + 1U);
        initialize_statuses(count);
        dispatch_codec_kernel(telemetry_, hip_kernel_api().count_rans_boundaries,
                              (count + kStatisticsThreads - 1U) / kStatisticsThreads, kStatisticsThreads,
                              "count rANS segment boundaries", source_.data(), bytes, shift, first, count, removed_,
                              kRansCountWords, device_statuses_, count);
        require_statuses(count);
        statistics_checkpoint(checkpoint);
        first += count;
    }
    boundary_shift_ = shift;
}

void RansStatistics::project_counts(std::uint32_t bits, const std::function<void()>& checkpoint) {
    const auto entries = 256U << bits;
    for (std::uint32_t first = 0U; first < entries;) {
        const auto count = std::min(kRansProjectionBatch, entries - first);
        initialize_statuses(count);
        dispatch_codec_kernel(telemetry_, hip_kernel_api().project_rans_counts,
                              (count + kStatisticsThreads - 1U) / kStatisticsThreads, kStatisticsThreads,
                              "project rANS context counts", static_cast<const std::uint32_t*>(base_), kRansCountWords,
                              static_cast<const std::uint32_t*>(removed_), kRansCountWords,
                              static_cast<std::uint32_t>(source_.size()), bits, first, count, projected_, entries,
                              device_statuses_, count);
        require_statuses(count);
        statistics_checkpoint(checkpoint);
        first += count;
    }
    projected_bits_ = bits;
}

std::span<const std::uint32_t> RansStatistics::project(std::uint32_t context_bits, std::uint32_t segment_shift,
                                                       const std::function<void()>& checkpoint) {
    require_current_device();
    if (context_bits > 8U || segment_shift < 8U || segment_shift > 12U) {
        throw ArchiveError("rANS statistics projection options are invalid");
    }
    try {
        statistics_checkpoint(checkpoint);
        if (boundary_shift_ != segment_shift) {
            projected_bits_ = 9U;
            boundary_shift_ = 0U;
            count_boundaries(segment_shift, checkpoint);
        }
        if (projected_bits_ != context_bits) {
            projected_bits_ = 9U;
            project_counts(context_bits, checkpoint);
        }
    } catch (...) {
        boundary_shift_ = 0U;
        projected_bits_ = 9U;
        reusable_ = hipStreamSynchronize(hipStreamPerThread) == hipSuccess;
        throw;
    }
    return {projected_, std::size_t{256U} << context_bits};
}

void RansStatistics::close_checked() {
    reusable_ = false;
    boundary_shift_ = 0U;
    projected_bits_ = 9U;
    storage_.reset_checked("free rANS statistics scratch");
    reservation_.reduce_to(0U);
}

}  // namespace flitzzip::hip_detail
