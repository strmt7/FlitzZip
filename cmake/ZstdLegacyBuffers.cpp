#include "ZstdLegacyBuffers.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <span>
#include <utility>

#ifdef SUPERZIP_LEGACY_BUFFER_FAULT_ALLOCATIONS
#include "fault_allocator.h"
#endif

namespace {
struct BufferDelete {
    ZBUFF_bufferAllocator allocator;

    // Purpose: Release an array using the allocator that acquired it.
    // Inputs: One live array. Outputs: Matching release with no exception.
    void operator()(char* data) const noexcept {
        if (allocator.release != nullptr) {
            allocator.release(allocator.opaque, data);
        } else {
#ifdef SUPERZIP_LEGACY_BUFFER_FAULT_ALLOCATIONS
            sz_fault_free(data);
#else
            delete[] data;
#endif
        }
    }
};
using BufferOwner = std::unique_ptr<char[], BufferDelete>;

// Purpose: Acquire one scoped array under the selected allocation contract.
// Inputs: A valid allocator and bounded nonzero size. Outputs: One owner or an empty owner on failure.
BufferOwner acquire_buffer(ZBUFF_bufferAllocator allocator, std::size_t size) noexcept {
    char* data;
    if (allocator.allocate != nullptr) {
        data = static_cast<char*>(allocator.allocate(allocator.opaque, size));
    } else {
#ifdef SUPERZIP_LEGACY_BUFFER_FAULT_ALLOCATIONS
        data = static_cast<char*>(sz_fault_malloc(size));
#else
        data = new (std::nothrow) char[size];
#endif
    }
    return BufferOwner(data, BufferDelete{allocator});
}
}  // namespace

struct ZBUFF_ownedBuffers_s {
    ZBUFF_bufferAllocator allocator;
    BufferOwner input;
    std::size_t inputCapacity{};
    BufferOwner output;
    std::size_t outputCapacity{};

    // Purpose: Initialize an empty ownership tree with matching deleters.
    // Inputs: Validated allocator. Outputs: Empty owners and zero capacities.
    explicit ZBUFF_ownedBuffers_s(ZBUFF_bufferAllocator value) noexcept
        : allocator(value), input(nullptr, BufferDelete{value}), output(nullptr, BufferDelete{value}) {}
};

// Purpose: Create an empty owner without transferring partial construction to C.
// Inputs: A complete allocator pair or the default array allocator. Outputs: Owned state or null.
extern "C" ZBUFF_ownedBuffers* ZBUFF_createOwnedBuffers(ZBUFF_bufferAllocator allocator) {
    if ((allocator.allocate == nullptr) != (allocator.release == nullptr)) {
        return nullptr;
    }
    if (allocator.allocate != nullptr) {
        auto* storage = allocator.allocate(allocator.opaque, sizeof(ZBUFF_ownedBuffers));
        return storage == nullptr ? nullptr : new (storage) ZBUFF_ownedBuffers(allocator);
    }
#ifdef SUPERZIP_LEGACY_BUFFER_FAULT_ALLOCATIONS
    auto* storage = sz_fault_malloc(sizeof(ZBUFF_ownedBuffers));
    return storage == nullptr ? nullptr : new (storage) ZBUFF_ownedBuffers(allocator);
#else
    return new (std::nothrow) ZBUFF_ownedBuffers(allocator);
#endif
}

// Purpose: Grow actual owner capacities atomically with respect to allocation failure.
// Inputs: A live exclusive owner and requested extents. Outputs: Ordinary status, with strong rollback on failure.
extern "C" ZBUFF_bufferResult ZBUFF_reserveOwnedBuffers(ZBUFF_ownedBuffers* owner, std::size_t inputSize,
                                                        std::size_t outputSize) {
    if (owner == nullptr || inputSize > static_cast<std::size_t>(PTRDIFF_MAX) ||
        outputSize > static_cast<std::size_t>(PTRDIFF_MAX)) {
        return ZBUFF_buffer_invalid;
    }
    BufferOwner input(nullptr, BufferDelete{owner->allocator});
    BufferOwner output(nullptr, BufferDelete{owner->allocator});
    if (inputSize > owner->inputCapacity) {
        input = acquire_buffer(owner->allocator, inputSize);
        if (!input) {
            return ZBUFF_buffer_allocation_failure;
        }
    }
    if (outputSize > owner->outputCapacity) {
        output = acquire_buffer(owner->allocator, outputSize);
        if (!output) {
            return ZBUFF_buffer_allocation_failure;
        }
    }
    if (input) {
        owner->input = std::move(input);
        owner->inputCapacity = inputSize;
    }
    if (output) {
        owner->output = std::move(output);
        owner->outputCapacity = outputSize;
    }
    return ZBUFF_buffer_ok;
}

// Purpose: Expose one coherent borrowed view of actual allocated storage.
// Inputs: A live owner or null. Outputs: Pointers/capacities with no independent C-owned size fields.
extern "C" ZBUFF_bufferView ZBUFF_viewOwnedBuffers(const ZBUFF_ownedBuffers* owner) {
    return owner == nullptr
               ? ZBUFF_bufferView{}
               : ZBUFF_bufferView{owner->input.get(), owner->inputCapacity, owner->output.get(), owner->outputCapacity};
}

// Purpose: Destroy arrays before releasing their shared owner record.
// Inputs: One owned state or null. Outputs: Matching release of the complete ownership tree.
extern "C" void ZBUFF_releaseOwnedBuffers(ZBUFF_ownedBuffers* owner) {
    if (owner == nullptr) {
        return;
    }
    const auto allocator = owner->allocator;
    if (allocator.release != nullptr) {
        owner->~ZBUFF_ownedBuffers_s();
        allocator.release(allocator.opaque, owner);
    } else {
#ifdef SUPERZIP_LEGACY_BUFFER_FAULT_ALLOCATIONS
        owner->~ZBUFF_ownedBuffers_s();
        sz_fault_free(owner);
#else
        delete owner;
#endif
    }
}

// Purpose: Enforce complete byte extents before overlap-safe copying.
// Inputs: Actual live source/destination capacities, offsets and count.
// Outputs: Status; invalid requests make no access or pointer arithmetic and preserve destination bytes.
extern "C" ZBUFF_bufferResult ZBUFF_copyBytes(void* destination, std::size_t destinationCapacity,
                                              std::size_t destinationOffset, const void* source,
                                              std::size_t sourceCapacity, std::size_t sourceOffset, std::size_t count) {
    if (destinationOffset > destinationCapacity || sourceOffset > sourceCapacity ||
        count > destinationCapacity - destinationOffset || count > sourceCapacity - sourceOffset ||
        destinationCapacity > static_cast<std::size_t>(PTRDIFF_MAX) ||
        sourceCapacity > static_cast<std::size_t>(PTRDIFF_MAX) ||
        (destination == nullptr && destinationCapacity != 0) || (source == nullptr && sourceCapacity != 0)) {
        return ZBUFF_buffer_invalid;
    }
    if (count == 0) {
        return ZBUFF_buffer_ok;
    }
    const auto input = std::span(static_cast<const char*>(source), sourceCapacity).subspan(sourceOffset, count);
    const auto output =
        std::span(static_cast<char*>(destination), destinationCapacity).subspan(destinationOffset, count);
    if (input.data() == output.data()) {
        return ZBUFF_buffer_ok;
    }
    const std::less<const char*> before;
    if (before(input.data(), output.data()) && before(output.data(), input.data() + count)) {
        std::copy_backward(input.begin(), input.end(), output.end());
    } else {
        std::copy(input.begin(), input.end(), output.begin());
    }
    return ZBUFF_buffer_ok;
}
