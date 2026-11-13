#pragma once

#include "gpu/hip_rans_statistics.hpp"

#include <array>
#include <optional>
#include <vector>

namespace flitzzip::hip_detail {

struct RansEncodeWorkspaceRequirements {
    std::size_t host_bytes;    // Operation, requested vector storage and one returned payload up to N bytes.
    std::size_t device_bytes;  // Statistics and all encoder scratch; excludes the borrowed source and other winners.
    std::size_t scratch_bytes;
    std::uint32_t max_segments;
    std::uint32_t slot_bytes;
};

// Allocation-free logical storage plan for a 1..16 MiB source. Charge host_bytes
// before constructing an encoder; separately charge resident source, retained
// winners, workers, callback captures and allocator bookkeeping. The operation
// reserves its device scratch through the existing aggregate HIP budget.
[[nodiscard]] RansEncodeWorkspaceRequirements rans_encode_workspace_requirements(std::size_t source_bytes);

// One thread-affine encoder borrows immutable resident source bytes and live
// telemetry on the calling thread's current device. Preceding writes must be
// available on hipStreamPerThread. Borrowed owners remain live and immutable
// until successful close_checked() or destruction, including after failed close.
// All counting, normalization and symbol encoding execute on HIP; host work is
// bounded metadata admission, prefix offsets and sparse framing only.
//
// encode() returns a complete private payload only when strictly smaller than
// exclusive_limit (0..source.size()); losses and ties return nullopt. Returned
// bytes own their storage and survive the next trial. Trial views are never
// exposed. Counts and sorted positive symbols are reused across precision trials;
// unconditional models are independent of segment shift. A caller may retain
// only one returned payload within host_bytes and must separately charge others.
//
// Checkpoints run before work and after actual completion, including losing and
// cached trials. They may throw, but must not reenter, change devices or mutate
// borrowed storage. Failed admitted work invalidates prepared models; failed
// drain in this owner or its statistics child prohibits reuse. Invalid options
// do not invalidate completed caches.
// Checked close reports release failure and retains ownership for retry;
// destruction keeps the existing best-effort HIP buffer cleanup contract.
// Runtime descriptors are copied; borrowed callback state must outlive this owner.
// Default descriptors retain native HIP execution and hardware admission.
// This component does not activate a public archive kind or replace normal
// archive version admission, decoded CRC verification or adoption qualification.
class RansEncoder {
  public:
    explicit RansEncoder(std::span<const std::byte> device_source, GpuTelemetry* telemetry,
                         const std::function<void()>& checkpoint = {}, HipRuntimeCalls runtime = {});
    RansEncoder(const RansEncoder&) = delete;
    RansEncoder& operator=(const RansEncoder&) = delete;

    [[nodiscard]] std::optional<std::vector<std::byte>> encode(std::uint32_t precision, std::uint32_t context_bits,
                                                               std::uint32_t segment_shift, std::size_t exclusive_limit,
                                                               const std::function<void()>& checkpoint = {});
    void close_checked();

  private:
    void bind_storage();
    void require_current_device() const;
    void prepare_rows(std::uint32_t bits, std::uint32_t shift, const std::function<void()>& checkpoint);
    void prepare_model(std::uint32_t precision, const std::function<void()>& checkpoint);
    void initialize_statuses(std::uint32_t count);
    void require_statuses(std::uint32_t count);
    [[nodiscard]] bool encode_streams(std::uint32_t shift, std::size_t frame_header_bytes, std::size_t exclusive_limit,
                                      const std::function<void()>& checkpoint);
    void admit_stream_batch(std::uint32_t first, std::uint32_t count, std::uint32_t shift);
    void compact_streams(std::uint32_t shift, const std::function<void()>& checkpoint);
    [[nodiscard]] std::vector<std::byte> serialize(std::uint32_t shift, std::size_t frame_header_bytes);

    const std::span<const std::byte> source_;
    GpuTelemetry* const telemetry_;
    const HipRuntimeCalls runtime_;
    const RansEncodeWorkspaceRequirements required_;
    HipDeviceMemoryReservation reservation_;
    RansStatistics statistics_;
    HipDeviceBuffer<std::byte> storage_;
    std::vector<std::uint32_t> frequencies_;
    std::vector<std::uint32_t> offsets_;
    std::array<RansModelRow, 256U> rows_{};
    std::array<RansModelStatus, 256U> statuses_{};
    std::array<RansEncodeResult, kRansEncodeBatchSegments> results_{};
    RansModelRow* device_rows_ = nullptr;
    std::uint8_t* order_ = nullptr;
    std::uint32_t* device_frequencies_ = nullptr;
    std::uint32_t* models_ = nullptr;
    std::uint32_t* reciprocals_ = nullptr;
    RansModelStatus* device_statuses_ = nullptr;
    std::byte* slots_ = nullptr;
    RansEncodeResult* device_results_ = nullptr;
    std::uint32_t* device_offsets_ = nullptr;
    std::byte* output_ = nullptr;
    std::span<const std::uint32_t> counts_;
    std::size_t model_bytes_ = 0U;
    std::uint32_t bits_ = 9U;
    std::uint32_t shift_ = 0U;
    std::uint32_t precision_ = 0U;
    int device_ = -1;
    bool reusable_ = true;
};

}  // namespace flitzzip::hip_detail
