#pragma once

#include "core/result.hpp"

#include <cstddef>
#include <hip/hip_runtime.h>
#include <hip/hip_ext.h>

namespace flitzzip::hip_detail {

inline hipError_t native_hip_allocate(void*, void** pointer, std::size_t bytes) noexcept {
    return hipMalloc(pointer, bytes);
}

inline hipError_t native_hip_release(void*, void* pointer) noexcept {
    return hipFree(pointer);
}

// An operation snapshots this allocator/deallocator pair. Context is borrowed
// and must remain live through every owned allocation's destruction. Callbacks
// must match HIP ownership: unsuccessful acquisition transfers no allocation;
// unsuccessful checked release must not claim a successful release. Controlled
// failures reject before the real operation, never free then invent an error.
struct HipMemoryCalls {
    void* context = nullptr;
    hipError_t (*allocate)(void*, void**, std::size_t) noexcept = native_hip_allocate;
    hipError_t (*release)(void*, void*) noexcept = native_hip_release;
};

inline hipError_t native_hip_copy(void*, void* destination, const void* source, std::size_t bytes,
                                  hipMemcpyKind direction, hipStream_t stream) noexcept {
    return hipMemcpyWithStream(destination, source, bytes, direction, stream);
}

inline hipError_t native_hip_clear(void*, void* destination, int value, std::size_t bytes,
                                   hipStream_t stream) noexcept {
    return hipMemsetAsync(destination, value, bytes, stream);
}

inline hipError_t native_hip_launch(void*, const void* kernel, dim3 grid, dim3 block, void** arguments,
                                    std::size_t shared_bytes, hipStream_t stream, hipEvent_t start,
                                    hipEvent_t stop) noexcept {
    return hipExtLaunchKernel(kernel, grid, block, arguments, shared_bytes, stream, start, stop, 0U);
}

inline hipError_t native_hip_synchronize(void*, hipStream_t stream) noexcept {
    return hipStreamSynchronize(stream);
}

// Private per-instance SDK boundary, not a backend selector or global hook.
// Descriptors are copied; borrowed callback state stays live and thread-affine
// through owner destruction. Calls preserve native HIP stream/copy semantics,
// do not throw or retain host argument arrays, and may leave preceding device
// work pending after failure. Successful calls perform the real requested work.
// Hardware/module admission, device selection and aggregate budgets stay real.
struct HipRuntimeCalls {
    HipMemoryCalls memory;
    void* context = nullptr;
    hipError_t (*copy)(void*, void*, const void*, std::size_t, hipMemcpyKind, hipStream_t) noexcept = native_hip_copy;
    hipError_t (*clear)(void*, void*, int, std::size_t, hipStream_t) noexcept = native_hip_clear;
    hipError_t (*launch)(void*, const void*, dim3, dim3, void**, std::size_t, hipStream_t, hipEvent_t,
                         hipEvent_t) noexcept = native_hip_launch;
    hipError_t (*synchronize)(void*, hipStream_t) noexcept = native_hip_synchronize;
};

[[nodiscard]] inline HipRuntimeCalls checked_hip_runtime_calls(HipRuntimeCalls calls) {
    if (calls.memory.allocate == nullptr || calls.memory.release == nullptr || calls.copy == nullptr ||
        calls.clear == nullptr || calls.launch == nullptr || calls.synchronize == nullptr) {
        throw GpuError("HIP runtime call interface is incomplete");
    }
    return calls;
}

}  // namespace flitzzip::hip_detail
