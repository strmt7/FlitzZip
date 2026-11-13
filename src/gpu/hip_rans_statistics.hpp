#pragma once

#include "gpu/hip_kernel_api.hpp"

#include <array>
#include <functional>
#include <span>

namespace flitzzip::hip_detail {

struct RansStatisticsWorkspaceRequirements {
    std::size_t host_bytes;    // Includes the operation and its fixed status array.
    std::size_t device_bytes;  // Excludes the caller's immutable resident source.
};

[[nodiscard]] RansStatisticsWorkspaceRequirements rans_statistics_workspace_requirements() noexcept;

// One thread-affine operation borrows an exact, immutable resident source of
// 1..kRansCountMaxBytes bytes on the calling thread's current device. Its owner
// must remain live and immutable until successful close_checked() or destruction;
// preceding writes must be available on the codec's per-thread stream. Supplied
// telemetry must remain live for that same interval. No source upload, host
// counting or CPU fallback occurs.
// Callers charge host_bytes before constructing this object and separately
// account for the source, telemetry and callback. Device scratch is reserved here.
//
// Construction counts every source tile once. Projection reuses the immutable
// base and, for equal segment widths, completed boundary corrections. Returned
// device views are complete but expire at the next project(), close_checked()
// or destruction. They are not normalized models: the connected model consumer
// must independently admit row masses and their sum against source.size().
//
// Checkpoints execute before work and after completed batches, including cache
// hits. They may throw but must not reenter, change devices or mutate/retain
// borrowed storage. Failed admitted work or cancellation invalidates both caches;
// option rejection leaves completed caches intact. A failed drain permanently
// prohibits reuse. Normal callers check close_checked(); destructors
// retain the existing best-effort HIP buffer cleanup contract on exceptional exit.
class RansStatistics {
  public:
    explicit RansStatistics(std::span<const std::byte> device_source, GpuTelemetry* telemetry,
                            const std::function<void()>& checkpoint = {});
    RansStatistics(const RansStatistics&) = delete;
    RansStatistics& operator=(const RansStatistics&) = delete;

    [[nodiscard]] std::span<const std::uint32_t> project(std::uint32_t context_bits, std::uint32_t segment_shift,
                                                         const std::function<void()>& checkpoint = {});

    // Invalidate views before checked release. A free failure retains ownership
    // and its reservation for a cleanup retry or destructor; no further use is admitted.
    void close_checked();

  private:
    void count_source(const std::function<void()>& checkpoint);
    void count_boundaries(std::uint32_t shift, const std::function<void()>& checkpoint);
    void project_counts(std::uint32_t bits, const std::function<void()>& checkpoint);
    void initialize_statuses(std::uint32_t count);
    void require_statuses(std::uint32_t count);
    void require_current_device() const;

    const std::span<const std::byte> source_;
    GpuTelemetry* const telemetry_;
    HipDeviceMemoryReservation reservation_;
    HipDeviceBuffer<std::uint32_t> storage_;
    std::uint32_t* base_;
    std::uint32_t* removed_;
    std::uint32_t* projected_;
    RansCountStatus* device_statuses_;
    std::array<RansCountStatus, kRansProjectionBatch> statuses_{};
    int device_ = -1;
    std::uint32_t boundary_shift_ = 0U;
    std::uint32_t projected_bits_ = 9U;
    bool reusable_ = true;
};

}  // namespace flitzzip::hip_detail
