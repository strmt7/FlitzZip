#pragma once

#include "7zTypes.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>

namespace superzip {

class BoundedSdkAllocator final : public ISzAlloc {
  public:
    // Purpose: Bind SDK callbacks to one stable, bounded allocation owner rather than ambient thread state.
    // Inputs: `limit_bytes` is the maximum live C-heap allocation size for this decoder/archive.
    // Outputs: Initializes an empty allocator; callers must serialize access, including thread handoff.
    explicit BoundedSdkAllocator(std::uint64_t limit_bytes)
        : ISzAlloc{allocate_callback, free_callback}, limit_bytes_(limit_bytes) {}

    BoundedSdkAllocator(const BoundedSdkAllocator&) = delete;
    BoundedSdkAllocator& operator=(const BoundedSdkAllocator&) = delete;

    // Purpose: Release any outstanding SDK allocations after ordinary or failed decoder teardown.
    // Inputs: Owned allocation registry.
    // Outputs: Frees all remaining allocations without throwing.
    ~BoundedSdkAllocator() {
        for (const auto& [address, bytes] : allocations_) {
            std::free(address);
        }
    }

    // Purpose: Report live SDK allocation accounting for bounded ownership regression checks.
    // Inputs: None; callers must serialize access.
    // Outputs: Returns current live C-heap bytes, including one byte for zero-size requests.
    [[nodiscard]] std::uint64_t current_bytes() const noexcept {
        return current_bytes_;
    }

  private:
    // Purpose: Recover the exact allocator owner from its SDK base interface and allocate within its limit.
    // Inputs: `allocator` points to this object's ISzAlloc base; `size` is the SDK byte request.
    // Outputs: Returns zero-filled memory or null; no C++ exception crosses the SDK callback boundary.
    static void* allocate_callback(ISzAllocPtr allocator, std::size_t size) noexcept {
        auto& owner = *const_cast<BoundedSdkAllocator*>(static_cast<const BoundedSdkAllocator*>(allocator));
        const auto bytes = size == 0U ? 1U : size;
        if (bytes > owner.limit_bytes_ || owner.current_bytes_ > owner.limit_bytes_ - bytes) {
            return nullptr;
        }
        void* address = std::calloc(1U, bytes);
        if (address == nullptr) {
            return nullptr;
        }
        try {
            owner.allocations_.emplace(address, bytes);
            owner.current_bytes_ += static_cast<std::uint64_t>(bytes);
        } catch (...) {
            std::free(address);
            return nullptr;
        }
        return address;
    }

    // Purpose: Release SDK memory through the same owner that allocated it, independently of the caller thread.
    // Inputs: `allocator` is this object's SDK base; `address` is null or one of its live allocations.
    // Outputs: Removes the allocation and its byte charge; null or foreign pointers leave this owner unchanged.
    static void free_callback(ISzAllocPtr allocator, void* address) noexcept {
        auto& owner = *const_cast<BoundedSdkAllocator*>(static_cast<const BoundedSdkAllocator*>(allocator));
        const auto found = owner.allocations_.find(address);
        if (found == owner.allocations_.end()) {
            return;
        }
        owner.current_bytes_ -= static_cast<std::uint64_t>(found->second);
        owner.allocations_.erase(found);
        std::free(address);
    }

    const std::uint64_t limit_bytes_;
    std::uint64_t current_bytes_ = 0;
    std::unordered_map<void*, std::size_t> allocations_;
};

}  // namespace superzip
