#include "ZstdLegacyBuffers.h"

#include <algorithm>
#include <cstddef>
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

// Purpose: Construct an ownership record under its original allocation contract.
// Inputs: A complete aligned, nonthrowing allocator pair or the default allocator.
// Outputs: A complete record or null, with no partial ownership transfer.
template <typename Owner> Owner* create_record(ZBUFF_bufferAllocator allocator) noexcept {
    if ((allocator.allocate == nullptr) != (allocator.release == nullptr)) {
        return nullptr;
    }
    if (allocator.allocate != nullptr) {
        auto* storage = allocator.allocate(allocator.opaque, sizeof(Owner));
        if (storage != nullptr && reinterpret_cast<std::uintptr_t>(storage) % alignof(Owner) != 0) {
            allocator.release(allocator.opaque, storage);
            return nullptr;
        }
        return storage == nullptr ? nullptr : new (storage) Owner(allocator);
    }
#ifdef SUPERZIP_LEGACY_BUFFER_FAULT_ALLOCATIONS
    auto* storage = sz_fault_malloc(sizeof(Owner));
    return storage == nullptr ? nullptr : new (storage) Owner(allocator);
#else
    return new (std::nothrow) Owner(allocator);
#endif
}

// Purpose: Destroy owned arrays before releasing their ownership record.
// Inputs: One record or null. Outputs: Exactly one release through the matching allocator.
template <typename Owner> void release_record(Owner* owner) noexcept {
    if (owner == nullptr) {
        return;
    }
    const auto allocator = owner->allocator;
    if (allocator.release != nullptr) {
        owner->~Owner();
        allocator.release(allocator.opaque, owner);
    } else {
#ifdef SUPERZIP_LEGACY_BUFFER_FAULT_ALLOCATIONS
        owner->~Owner();
        sz_fault_free(owner);
#else
        delete owner;
#endif
    }
}

// Purpose: Reject impossible complete byte geometry before pointer formation.
// Inputs: A pointer, its actual extent and a selected initialized range.
// Outputs: True only for a representable range and a non-null nonempty extent.
bool valid_extent(const void* data, std::size_t capacity, std::size_t offset, std::size_t count) noexcept {
    return offset <= capacity && count <= capacity - offset && capacity <= static_cast<std::size_t>(PTRDIFF_MAX) &&
           (data != nullptr || capacity == 0);
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

struct ZBUFF_ownedHistory_s {
    ZBUFF_bufferAllocator allocator;
    BufferOwner bytes;
    std::size_t capacity{};
    std::size_t initialized{};
    std::size_t next{};
    std::size_t limit{};

    // Purpose: Initialize an empty independent history record.
    // Inputs: Validated allocator. Outputs: Matching empty array ownership and zero geometry.
    explicit ZBUFF_ownedHistory_s(ZBUFF_bufferAllocator value) noexcept
        : allocator(value), bytes(nullptr, BufferDelete{value}) {}
};

struct ZBUFF_decoderOwner_s {
    ZBUFF_bufferAllocator allocator;
    BufferOwner storage;
    std::size_t storageBytes{};
    std::unique_ptr<ZBUFF_ownedHistory, decltype(&ZBUFF_releaseOwnedHistory)> history;

    // Purpose: Initialize an empty decoder ownership tree with matching child releases.
    // Inputs: Validated allocator. Outputs: An empty tree that can roll back partial acquisition.
    explicit ZBUFF_decoderOwner_s(ZBUFF_bufferAllocator value) noexcept
        : allocator(value), storage(nullptr, BufferDelete{value}), history(nullptr, ZBUFF_releaseOwnedHistory) {}
};

namespace {
// Purpose: Copy an initialized logical suffix across at most one ring boundary.
// Inputs: Nonempty owned history, count <= distance <= initialized and a complete distinct writable output range.
// Outputs: Exactly count bytes in logical order; retains no output pointer.
void copy_history_range(const ZBUFF_ownedHistory& owner, std::span<char> output, std::size_t distance) noexcept {
    const auto begin = distance <= owner.next ? owner.next - distance : owner.capacity - (distance - owner.next);
    const auto first = std::min(output.size(), owner.capacity - begin);
    const auto input = std::span(owner.bytes.get(), owner.capacity);
    std::copy_n(input.begin() + begin, first, output.begin());
    std::copy_n(input.begin(), output.size() - first, output.begin() + first);
}

// Purpose: Grow a history array transactionally without losing initialized logical order.
// Inputs: Required bytes fit the owner's limit and are greater than current capacity.
// Outputs: New owned storage or allocation failure; failure changes no history metadata or bytes.
ZBUFF_bufferResult grow_history(ZBUFF_ownedHistory& owner, std::size_t required) noexcept {
    const auto doubled = owner.capacity > owner.limit / 2 ? owner.limit : owner.capacity * 2;
    const auto capacity = std::max(required, doubled);
    auto bytes = acquire_buffer(owner.allocator, capacity);
    if (!bytes) {
        return ZBUFF_buffer_allocation_failure;
    }
    if (owner.initialized != 0) {
        copy_history_range(owner, std::span(bytes.get(), capacity).first(owner.initialized), owner.initialized);
    }
    owner.bytes = std::move(bytes);
    owner.capacity = capacity;
    owner.next = owner.initialized == capacity ? 0 : owner.initialized;
    return ZBUFF_buffer_ok;
}
}  // namespace

// Purpose: Create an empty owner without transferring partial construction to C.
// Inputs: A complete allocator pair or the default array allocator. Outputs: Owned state or null.
extern "C" ZBUFF_ownedBuffers* ZBUFF_createOwnedBuffers(ZBUFF_bufferAllocator allocator) {
    return create_record<ZBUFF_ownedBuffers>(allocator);
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
    release_record(owner);
}

// Purpose: Enforce complete byte extents before overlap-safe copying.
// Inputs: Actual live source/destination capacities, offsets and count.
// Outputs: Status; invalid requests make no access or pointer arithmetic and preserve destination bytes.
extern "C" ZBUFF_bufferResult ZBUFF_copyBytes(void* destination, std::size_t destinationCapacity,
                                              std::size_t destinationOffset, const void* source,
                                              std::size_t sourceCapacity, std::size_t sourceOffset, std::size_t count) {
    if (!valid_extent(destination, destinationCapacity, destinationOffset, count) ||
        !valid_extent(source, sourceCapacity, sourceOffset, count)) {
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

// Purpose: Create bounded independent history. Inputs: Complete allocator pair or null callbacks.
// Outputs: A complete empty owner or null; no caller storage is borrowed.
extern "C" ZBUFF_ownedHistory* ZBUFF_createOwnedHistory(ZBUFF_bufferAllocator allocator) {
    return create_record<ZBUFF_ownedHistory>(allocator);
}

// Purpose: Reset initialized history for a new decoding session.
// Inputs: A live owner and representable byte limit. Outputs: Status; success clears only logical history.
extern "C" ZBUFF_bufferResult ZBUFF_resetOwnedHistory(ZBUFF_ownedHistory* owner, std::size_t limit) {
    if (owner == nullptr || limit > static_cast<std::size_t>(PTRDIFF_MAX)) {
        return ZBUFF_buffer_invalid;
    }
    owner->limit = limit;
    owner->initialized = 0;
    owner->next = 0;
    return ZBUFF_buffer_ok;
}

// Purpose: Retain only history permitted by the decoded frame window.
// Inputs: A live owner and representable limit. Outputs: Status; newer initialized bytes remain in order.
extern "C" ZBUFF_bufferResult ZBUFF_limitOwnedHistory(ZBUFF_ownedHistory* owner, std::size_t limit) {
    if (owner == nullptr || limit > static_cast<std::size_t>(PTRDIFF_MAX)) {
        return ZBUFF_buffer_invalid;
    }
    owner->limit = limit;
    owner->initialized = std::min(owner->initialized, limit);
    return ZBUFF_buffer_ok;
}

// Purpose: Capture caller bytes in independent history before their lifetime ends.
// Inputs: Full initialized source geometry, distinct from private history storage.
// Outputs: Status; invalid geometry or allocation failure preserves all previous history.
extern "C" ZBUFF_bufferResult ZBUFF_appendOwnedHistory(ZBUFF_ownedHistory* owner, const void* source,
                                                       std::size_t capacity, std::size_t offset, std::size_t count) {
    if (owner == nullptr || !valid_extent(source, capacity, offset, count)) {
        return ZBUFF_buffer_invalid;
    }
    if (count == 0 || owner->limit == 0) {
        return ZBUFF_buffer_ok;
    }
    const auto retained = std::min(count, owner->limit);
    const auto required = owner->initialized >= owner->limit - retained ? owner->limit : owner->initialized + retained;
    if (required > owner->capacity) {
        const auto result = grow_history(*owner, required);
        if (result != ZBUFF_buffer_ok) {
            return result;
        }
    }
    const auto input =
        std::span(static_cast<const char*>(source), capacity).subspan(offset + count - retained, retained);
    const auto output = std::span(owner->bytes.get(), owner->capacity);
    const auto first = std::min(retained, owner->capacity - owner->next);
    std::copy_n(input.begin(), first, output.begin() + owner->next);
    std::copy(input.begin() + first, input.end(), output.begin());
    owner->next = retained < owner->capacity - owner->next ? owner->next + retained : retained - first;
    owner->initialized = required;
    return ZBUFF_buffer_ok;
}

// Purpose: Copy actual initialized history into a bounded caller destination.
// Inputs: Complete writable geometry and count <= distance <= initialized history.
// Outputs: Status; invalid requests perform no write and never retain caller pointers.
extern "C" ZBUFF_bufferResult ZBUFF_copyOwnedHistory(const ZBUFF_ownedHistory* owner, void* destination,
                                                     std::size_t capacity, std::size_t offset, std::size_t distance,
                                                     std::size_t count) {
    if (owner == nullptr || !valid_extent(destination, capacity, offset, count) || count > distance ||
        distance > owner->initialized) {
        return ZBUFF_buffer_invalid;
    }
    if (count != 0) {
        copy_history_range(*owner, std::span(static_cast<char*>(destination), capacity).subspan(offset, count),
                           distance);
    }
    return ZBUFF_buffer_ok;
}

// Purpose: Clone history under the destination allocator without sharing borrowed storage.
// Inputs: Two live owners. Outputs: Status; allocation failures retain original destination state.
extern "C" ZBUFF_bufferResult ZBUFF_cloneOwnedHistory(ZBUFF_ownedHistory* destination,
                                                      const ZBUFF_ownedHistory* source) {
    if (destination == nullptr || source == nullptr) {
        return ZBUFF_buffer_invalid;
    }
    if (destination == source) {
        return ZBUFF_buffer_ok;
    }
    BufferOwner bytes(nullptr, BufferDelete{destination->allocator});
    if (source->initialized != 0) {
        bytes = acquire_buffer(destination->allocator, source->initialized);
        if (!bytes) {
            return ZBUFF_buffer_allocation_failure;
        }
        copy_history_range(*source, std::span(bytes.get(), source->initialized), source->initialized);
    }
    destination->bytes = std::move(bytes);
    destination->capacity = source->initialized;
    destination->initialized = source->initialized;
    destination->next = 0;
    destination->limit = source->limit;
    return ZBUFF_buffer_ok;
}

// Purpose: Report actual initialized history. Inputs: A live owner or null. Outputs: Its retained byte count.
extern "C" std::size_t ZBUFF_ownedHistorySize(const ZBUFF_ownedHistory* owner) {
    return owner == nullptr ? 0 : owner->initialized;
}

// Purpose: Release one complete history tree. Inputs: An owner or null. Outputs: Matching array/record destruction.
extern "C" void ZBUFF_releaseOwnedHistory(ZBUFF_ownedHistory* owner) {
    release_record(owner);
}

// Purpose: Construct all decoder-owned resources before exposing a C context.
// Inputs: Representable nonzero bytes, fundamental power-of-two alignment and matching allocator callbacks.
// Outputs: Complete ownership or null; every failed child acquisition rolls back through its original allocator.
extern "C" ZBUFF_decoderOwner* ZBUFF_createDecoderOwner(ZBUFF_bufferAllocator allocator, std::size_t bytes,
                                                        std::size_t alignment) {
    if (bytes == 0 || bytes > static_cast<std::size_t>(PTRDIFF_MAX) || alignment == 0 ||
        alignment > alignof(std::max_align_t) || (alignment & (alignment - 1)) != 0) {
        return nullptr;
    }
    std::unique_ptr<ZBUFF_decoderOwner, decltype(&ZBUFF_releaseDecoderOwner)> owner(
        create_record<ZBUFF_decoderOwner>(allocator), ZBUFF_releaseDecoderOwner);
    if (!owner) {
        return nullptr;
    }
    owner->storage = acquire_buffer(allocator, bytes);
    if (!owner->storage || reinterpret_cast<std::uintptr_t>(owner->storage.get()) % alignment != 0) {
        return nullptr;
    }
    owner->storageBytes = bytes;
    owner->history.reset(ZBUFF_createOwnedHistory(allocator));
    return owner->history ? owner.release() : nullptr;
}

// Purpose: Expose only the actual owned decoder allocation.
// Inputs: A complete owner or null. Outputs: Aligned live storage or null, with no ownership transfer.
extern "C" void* ZBUFF_decoderStorage(const ZBUFF_decoderOwner* owner) {
    return owner == nullptr ? nullptr : owner->storage.get();
}

// Purpose: Expose the decoder tree's independently owned history record.
// Inputs: A complete owner or null. Outputs: Live history or null; caller storage is never retained.
extern "C" ZBUFF_ownedHistory* ZBUFF_decoderHistory(const ZBUFF_decoderOwner* owner) {
    return owner == nullptr ? nullptr : owner->history.get();
}

// Purpose: Account for complete initial decoder ownership without allocation.
// Inputs: Context bytes. Outputs: Initial footprint or SIZE_MAX when addition cannot be represented.
extern "C" std::size_t ZBUFF_decoderBaseSize(std::size_t bytes) {
    constexpr auto records = sizeof(ZBUFF_decoderOwner) + sizeof(ZBUFF_ownedHistory);
    return bytes > SIZE_MAX - records ? SIZE_MAX : bytes + records;
}

// Purpose: Account for every decoder-owned allocation, including cached history capacity.
// Inputs: One complete ownership tree or null. Outputs: Its allocated byte footprint, zero for null.
extern "C" std::size_t ZBUFF_decoderOwnedSize(const ZBUFF_decoderOwner* owner) {
    if (owner == nullptr) {
        return 0;
    }
    const auto base = ZBUFF_decoderBaseSize(owner->storageBytes);
    const auto history = owner->history->capacity;
    return history > SIZE_MAX - base ? SIZE_MAX : base + history;
}

// Purpose: Destroy history before decoder bytes and release the parent under its matching allocator.
// Inputs: One owner or null. Outputs: Complete exactly-once ownership tree destruction.
extern "C" void ZBUFF_releaseDecoderOwner(ZBUFF_decoderOwner* owner) {
    release_record(owner);
}
