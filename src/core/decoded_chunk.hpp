#pragma once

#include "gpu/gpu_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace superzip {

// Output ownership pairs heap or HIP allocation with its exact matching non-throwing release operation.
struct DecodedStorageDeleter {
    std::size_t size = 0;
    void (*release)(std::byte*, std::size_t) noexcept = nullptr;
    std::shared_ptr<HipHostOutputPool> pool;
    bool new_pinned_allocation = true;

    // Purpose: Release one output allocation using its matching allocator, including moved ownership.
    // Inputs: pointer is the unique owner's allocation; size/release were established at allocation time.
    // Outputs: Frees heap storage or invokes the bounded HIP release; never throws from destruction.
    void operator()(std::byte* pointer) const noexcept;
};

using DecodedStorage = std::unique_ptr<std::byte[], DecodedStorageDeleter>;

// Purpose: Bound required-HIP owned-output concurrency by the aggregate pin capacity, keeping CPU admission unchanged.
// Inputs: requested is admitted pipeline depth; output_bytes is its maximum decoded window; options select backend.
// Outputs: Returns at least one and no more than requested tasks; no device selection or allocation is changed.
std::uint32_t resolve_owned_decode_inflight(std::uint32_t requested, std::size_t output_bytes,
                                            const GpuCodecOptions& options) noexcept;

// Purpose: Create an operation-local reusable pinned-output cache only for required-HIP owned decoding.
// Inputs: options select the normal backend; CPU/automatic mode does not acquire a retained pin cache.
// Outputs: Returns shared cache ownership or nullptr; no pinned buffers are allocated until decoding starts.
std::shared_ptr<HipHostOutputPool> make_owned_decode_pool(const GpuCodecOptions& options);

// Fully decoded, operation-owned bytes; empty and moved-from owners expose an empty view.
struct DecodedChunk {
    DecodedStorage storage;
    std::size_t decoded_size = 0;
    std::uint32_t crc32 = 0;
    bool gpu_used = false;

    // Purpose: Borrow completed decoded bytes without exposing uninitialized or moved-from storage.
    // Inputs: None; the returned view must not outlive this owner.
    // Outputs: Returns a read-only span or an empty span for an empty/moved-from owner.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return storage ? std::span<const std::byte>(storage.get(), decoded_size) : std::span<const std::byte>{};
    }
};

// Purpose: Decode one bounded native chunk into fully overwritten owned storage and compute its CRC.
// Inputs: Borrowed payload/blocks, exact decoded byte count, and the normal production CPU/HIP policy.
// Outputs: Returns completed bytes, CRC, and actual backend use; throws before returning partial decoded storage.
DecodedChunk decode_owned_chunk(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                                std::size_t decoded_size, const GpuCodecOptions& options);

}  // namespace superzip
