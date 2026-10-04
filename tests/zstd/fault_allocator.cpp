#include "fault_allocator.h"

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>

namespace {
// Each record owns its allocation. Borrowed C addresses never own or release
// storage, and an invalid release cannot discard another record.
std::array<std::unique_ptr<std::byte[]>, 32> allocations;
std::size_t allocationAttempts;
std::size_t failOnAttempt;
std::size_t invalidFrees;
}  // namespace

// Purpose: Report fixture ownership. Inputs: None. Outputs: The number of live exclusive owners.
extern "C" std::size_t sz_fault_live_allocations(void) {
    std::size_t count = 0;
    for (const auto& owner : allocations) {
        count += owner != nullptr;
    }
    return count;
}

// Purpose: Reset serial fault state without abandoning live ownership.
// Inputs: One-based failure attempt or zero. Outputs: One on reset, zero while allocations remain.
extern "C" int sz_fault_reset(std::size_t failOn) {
    if (sz_fault_live_allocations() != 0) {
        return 0;
    }
    allocationAttempts = 0;
    failOnAttempt = failOn;
    invalidFrees = 0;
    return 1;
}

// Purpose: Select a future serial failure while retaining every allocation owner.
// Inputs: Relative attempt count, or zero to disable injection. Outputs: Zero on counter overflow, one otherwise.
extern "C" int sz_fault_fail_after(std::size_t additionalAttempts) {
    if (additionalAttempts > std::numeric_limits<std::size_t>::max() - allocationAttempts) {
        return 0;
    }
    failOnAttempt = additionalAttempts == 0 ? 0 : allocationAttempts + additionalAttempts;
    return 1;
}

// Purpose: Acquire a fixture-owned byte allocation and lend its address to a C consumer.
// Inputs: Byte extent for a fundamentally aligned C object; calls are serial and do not throw.
// Outputs: Borrowed address or null on injection, bounded record exhaustion or allocation failure.
extern "C" void* sz_fault_malloc(std::size_t bytes) {
    if (allocationAttempts == std::numeric_limits<std::size_t>::max()) {
        return nullptr;
    }
    ++allocationAttempts;
    if (allocationAttempts == failOnAttempt) {
        return nullptr;
    }
    for (auto& owner : allocations) {
        if (!owner) {
            owner.reset(new (std::nothrow) std::byte[bytes]);
            return owner.get();
        }
    }
    return nullptr;
}

// Purpose: End exactly one fixture ownership record, preserving all others on invalid release.
// Inputs: Borrowed allocation address or null. Outputs: Matching array release or a diagnostic counter increment.
extern "C" void sz_fault_free(void* address) {
    if (address == nullptr) {
        return;
    }
    for (auto& owner : allocations) {
        if (owner.get() == address) {
            owner.reset();
            return;
        }
    }
    ++invalidFrees;
}

// Purpose: Report allocation ordering. Inputs: None. Outputs: Attempts since the last serial reset.
extern "C" std::size_t sz_fault_allocation_attempts(void) {
    return allocationAttempts;
}

// Purpose: Report invalid releases. Inputs: None. Outputs: Invalid-release attempts since the last serial reset.
extern "C" std::size_t sz_fault_invalid_frees(void) {
    return invalidFrees;
}
