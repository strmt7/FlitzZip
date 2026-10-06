#pragma once

#include "gpu/gpu_codec.hpp"

#include <mutex>
#include <vector>

namespace superzip {

// Purpose: Validate the trusted HIP runtime and the calling thread's current device without gathering diagnostics.
// Inputs: None; preserves the thread-local device selection and does not cache device availability or free memory.
// Outputs: Returns when a current device is admitted; throws GpuError on missing HIP or runtime/device failure.
void require_hip_device_ready();

// Purpose: Select optional HIP without treating runtime/device errors as ordinary absence.
// Inputs: None; preserves device selection and does not cache runtime/device availability.
// Outputs: Returns false for absent HIP only; unexpected enumeration/selection failures throw GpuError.
bool hip_device_available();

// Outstanding host pin reservations, not VRAM; release failures remain reserved conservatively.
struct HipPinnedHostStats {
    std::uint64_t reserved_bytes = 0;
    std::uint64_t release_failures = 0;
};

// Purpose: Try a bounded, operation-owned pinned output allocation on the calling thread's current HIP device.
// Inputs: bytes is an exact output extent; does not change device selection or retain an allocation pool.
// Outputs: Returns owned storage or nullptr for unavailable HIP, pin failure, or host-budget denial.
std::byte* try_allocate_hip_host_output(std::size_t bytes);

// Purpose: Release a completed pinned owner without throwing from a destructor or changing the current device.
// Inputs: pointer/bytes describe exactly one allocation returned by try_allocate_hip_host_output.
// Outputs: Frees storage and releases its reservation; failed frees retain the reservation and increment diagnostics.
void release_hip_host_output(std::byte* pointer, std::size_t bytes) noexcept;

// Purpose: Inspect aggregate pin admission and cleanup diagnostics without allocating or querying HIP.
// Inputs: None; snapshots may change concurrently.
// Outputs: Returns live reservation bytes and cumulative release failures, never identities or private paths.
HipPinnedHostStats snapshot_hip_pinned_host_stats() noexcept;

// Purpose: Derive a conservative pinned-output capacity without reserving memory or changing HIP device selection.
// Inputs: None; uses current available physical RAM on HIP-enabled Windows builds.
// Outputs: Returns the host-relative aggregate pin ceiling, or zero when the snapshot/backend is unavailable.
std::uint64_t hip_host_output_capacity_bytes() noexcept;

// A borrowed pool buffer records its actual allocation extent and whether it was reused.
struct HipHostOutputBuffer {
    std::byte* pointer = nullptr;
    std::size_t allocation_bytes = 0;
    bool reused = false;
};

// Operation-local cache; borrowed owners retain shared pool lifetime until their bytes are no longer used.
class HipHostOutputPool {
  public:
    // Purpose: Free all idle pinned buffers after the last operation/borrower releases shared ownership.
    // Inputs: No borrowers or concurrent member calls remain at destruction.
    // Outputs: Returns every idle reservation; failed HIP frees remain visible in aggregate diagnostics.
    ~HipHostOutputPool();

    // Purpose: Borrow the smallest fitting idle buffer or try a bounded fresh pin allocation.
    // Inputs: bytes is the desired decoded extent, bounded by the normal native chunk contract.
    // Outputs: Returns private storage with its allocation size, or an empty buffer for heap fallback.
    HipHostOutputBuffer acquire(std::size_t bytes);

    // Purpose: Cache a completed borrowed output, or free it when cache admission/allocation fails.
    // Inputs: pointer/bytes belong to one returned buffer; GPU work and all borrowed views have completed.
    // Outputs: Keeps at most 64 idle buffers under the global pin cap; never throws during owner destruction.
    void release(std::byte* pointer, std::size_t bytes) noexcept;

  private:
    std::mutex mutex_;
    std::vector<HipHostOutputBuffer> idle_;
};

// Purpose: Admit stream-ordered allocation only for an eligible loaded runtime and the current HIP device/pool.
// Inputs: Preserves device selection; caches immutable device capability per calling thread/device.
// Outputs: Returns false for unavailable features or retained-pool policies; throws on other HIP failures.
bool hip_stream_ordered_allocator_supported();

// Purpose: Query HIP availability, current device metadata, and a diagnostic memory snapshot.
// Inputs: None; preserves the calling thread's current device selection.
// Outputs: Returns diagnostic status in-band; memory values are not an allocation reservation.
GpuInfo query_hip_gpu_info();

}  // namespace superzip
