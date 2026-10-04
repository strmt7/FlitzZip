#include "ZstdCoverWorkGroup.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <utility>

extern "C" {
#include "common/error_private.h"
}

#ifdef SUPERZIP_COVER_FAULT_ALLOCATIONS
#include "fault_allocator.h"
#endif

namespace {
struct ContextDelete {
    // Purpose: Match the allocator used for private C context storage.
    // Inputs: Owned array or null. Outputs: Releases storage without invoking C context cleanup.
    void operator()(std::byte* storage) const noexcept {
#ifdef SUPERZIP_COVER_FAULT_ALLOCATIONS
        sz_fault_free(storage);
#else
        delete[] storage;
#endif
    }
};
using ContextOwner = std::unique_ptr<std::byte[], ContextDelete>;

// Purpose: Acquire fundamentally aligned storage for an implicit-lifetime private C context.
// Inputs: Validated positive extent. Outputs: Exclusive array owner or empty owner on failure.
ContextOwner acquire_context(std::size_t bytes) noexcept {
#ifdef SUPERZIP_COVER_FAULT_ALLOCATIONS
    return ContextOwner(static_cast<std::byte*>(sz_fault_malloc(bytes)));
#else
    return ContextOwner(new (std::nothrow) std::byte[bytes]);
#endif
}
}  // namespace

struct COVER_workGroup_s {
    COVER_best_t best{};
    std::unique_ptr<POOL_ctx, decltype(&POOL_free)> pool{nullptr, POOL_free};
    ContextOwner context;
    COVER_contextDestructor destroyContext = nullptr;
    bool committed = false;

    // Purpose: Initialize synchronized heap state before any worker can borrow it.
    // Inputs: None. Outputs: An empty group with initialized result synchronization.
    COVER_workGroup_s() noexcept {
        COVER_best_init(&best);
    }

    // Purpose: Join actual worker completion before context or synchronization is released.
    // Inputs: This exclusive owner. Outputs: Releases the entire ownership tree in dependency order.
    ~COVER_workGroup_s() noexcept {
        finish_context();
        COVER_best_destroy(&best);
    }

    // Purpose: Complete all worker accesses before destroying their shared context.
    // Inputs: This live group. Outputs: Empty context; pool and best record remain reusable.
    void finish_context() noexcept {
        if (pool) {
            POOL_joinJobs(pool.get());
        }
        COVER_best_wait(&best);
        if (committed && destroyContext != nullptr) {
            destroyContext(context.get());
        }
        committed = false;
        destroyContext = nullptr;
        context.reset();
    }
};

// Purpose: Construct optimizer state and its optional pool with complete failure rollback.
// Inputs: Requested worker count. Outputs: Exclusive heap owner or null; no borrowed stack state.
extern "C" COVER_workGroup* COVER_createWorkGroup(unsigned threads) {
#ifdef SUPERZIP_COVER_FAULT_ALLOCATIONS
    auto* storage = sz_fault_malloc(sizeof(COVER_workGroup));
    auto* group = storage == nullptr ? nullptr : new (storage) COVER_workGroup;
#else
    auto* group = new (std::nothrow) COVER_workGroup;
#endif
    if (group != nullptr && threads > 1) {
        group->pool.reset(POOL_create(threads, 1));
        if (!group->pool) {
            COVER_releaseWorkGroup(group);
            return nullptr;
        }
    }
    return group;
}

// Purpose: Borrow the result record from its owner; null input returns null.
// Inputs: Live group or null. Outputs: No ownership transfer.
extern "C" COVER_best_t* COVER_workGroupBest(COVER_workGroup* group) {
    return group == nullptr ? nullptr : &group->best;
}

// Purpose: Borrow the group's pool, or return null for inline execution.
// Inputs: Live group or null. Outputs: No ownership transfer.
extern "C" POOL_ctx* COVER_workGroupPool(COVER_workGroup* group) {
    return group == nullptr ? nullptr : group->pool.get();
}

// Purpose: Publish a completed dictionary only within the caller's advertised output allocation.
// Inputs: Joined group whose best record owns dictSize initialized bytes, and a live output extent.
// Outputs: Exact copied size or parameter error before copying; retains group ownership.
extern "C" std::size_t COVER_copyWorkDictionary(COVER_workGroup* group, void* destination, std::size_t capacity) {
    if (group == nullptr || capacity > static_cast<std::size_t>(PTRDIFF_MAX) || group->best.dictSize > capacity ||
        (group->best.dictSize != 0 && (destination == nullptr || group->best.dict == nullptr))) {
        return ERROR(parameter_outOfBound);
    }
    const std::span<const std::byte> input(static_cast<const std::byte*>(group->best.dict), group->best.dictSize);
    const std::span<std::byte> output(static_cast<std::byte*>(destination), group->best.dictSize);
    std::copy(input.begin(), input.end(), output.begin());
    return input.size();
}

// Purpose: Prepare one private C context without abandoning a live predecessor on allocation failure.
// Inputs: Live group, extent within PTRDIFF_MAX, supported alignment and matching cleanup callback.
// Outputs: Borrowed storage after joining the predecessor, or null with its ownership preserved.
extern "C" void* COVER_prepareWorkContext(COVER_workGroup* group, std::size_t bytes, std::size_t alignment,
                                          COVER_contextDestructor destroy) {
    if (group == nullptr || bytes == 0 || bytes > static_cast<std::size_t>(PTRDIFF_MAX) || alignment == 0 ||
        alignment > alignof(std::max_align_t) || (alignment & (alignment - 1)) != 0 || destroy == nullptr) {
        return nullptr;
    }
    auto next = acquire_context(bytes);
    if (!next) {
        return nullptr;
    }
    group->finish_context();
    group->context = std::move(next);
    group->destroyContext = destroy;
    return group->context.get();
}

// Purpose: Register successful C initialization for matching context destruction.
// Inputs: Live group with prepared storage. Outputs: Marks the owned context committed.
extern "C" void COVER_commitWorkContext(COVER_workGroup* group) {
    if (group != nullptr && group->context) {
        group->committed = true;
    }
}

// Purpose: Join all actual worker calls before invoking committed C context destruction.
// Inputs: Live group or null. Outputs: Releases its context and retains synchronized result state.
extern "C" void COVER_finishWorkContext(COVER_workGroup* group) {
    if (group != nullptr) {
        group->finish_context();
    }
}

// Purpose: Release the optimizer ownership tree through its matching allocator.
// Inputs: Exclusive owner or null. Outputs: Workers return before any borrowed state is destroyed.
extern "C" void COVER_releaseWorkGroup(COVER_workGroup* group) {
#ifdef SUPERZIP_COVER_FAULT_ALLOCATIONS
    if (group != nullptr) {
        group->~COVER_workGroup();
        sz_fault_free(group);
    }
#else
    delete group;
#endif
}
