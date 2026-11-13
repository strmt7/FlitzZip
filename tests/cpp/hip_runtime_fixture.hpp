#pragma once

#include "gpu/hip_runtime_calls.hpp"

#include <array>
#include <cstdint>

namespace flitzzip_test {

enum class HipCall : std::size_t { allocate, release, copy, clear, launch, synchronize, count };
inline constexpr auto kHipCallKinds = static_cast<std::size_t>(HipCall::count);

// Per-instance one-shot failures before real calls. Every successful acquisition
// is a real HIP allocation. Inline bookkeeping cannot allocate/throw in callbacks.
// Tests assert zero live allocations before teardown; emergency cleanup only
// protects the host after a failed assertion, never makes a leaking case pass.
class HipFaultRuntime {
  public:
    HipFaultRuntime() = default;
    HipFaultRuntime(const HipFaultRuntime&) = delete;
    HipFaultRuntime& operator=(const HipFaultRuntime&) = delete;

    ~HipFaultRuntime() {
        for (auto& allocation : live_) {
            if (allocation.pointer != nullptr) {
                (void)flitzzip::hip_detail::native_hip_release(nullptr, allocation.pointer);
            }
        }
    }

    [[nodiscard]] flitzzip::hip_detail::HipRuntimeCalls calls() noexcept {
        return {{this, allocate, release}, this, copy, clear, launch, synchronize};
    }
    void fail(HipCall kind, std::size_t relative_ordinal) noexcept {
        const auto index = static_cast<std::size_t>(kind);
        failures_[index] = counts[index] + relative_ordinal;
    }
    [[nodiscard]] std::size_t live_count() const noexcept {
        std::size_t count = 0U;
        for (const auto& allocation : live_) {
            count += allocation.pointer != nullptr ? 1U : 0U;
        }
        return count;
    }

    std::array<std::size_t, kHipCallKinds> counts{};
    std::size_t injected = 0U;
    bool fail_drain_after_primary = false;
    bool valid = true;

  private:
    struct Allocation {
        void* pointer = nullptr;
        std::size_t bytes = 0U;
    };
    std::array<Allocation, 4U> live_{};
    std::array<std::size_t, kHipCallKinds> failures_{};
    bool drain_failure_pending_ = false;

    bool reject(HipCall kind) noexcept {
        const auto index = static_cast<std::size_t>(kind);
        ++counts[index];
        if (counts[index] == failures_[index]) {
            failures_[index] = 0U;
            ++injected;
            drain_failure_pending_ = fail_drain_after_primary;
            return true;
        }
        return false;
    }
    bool contains(const void* pointer, std::size_t bytes) const noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        for (const auto& allocation : live_) {
            const auto begin = reinterpret_cast<std::uintptr_t>(allocation.pointer);
            if (allocation.pointer != nullptr && address >= begin && bytes <= allocation.bytes &&
                address - begin <= allocation.bytes - bytes) {
                return true;
            }
        }
        return false;
    }
    static hipError_t allocate(void* context, void** pointer, std::size_t bytes) noexcept {
        auto& self = *static_cast<HipFaultRuntime*>(context);
        if (self.reject(HipCall::allocate)) {
            return hipErrorOutOfMemory;
        }
        const auto status = flitzzip::hip_detail::native_hip_allocate(nullptr, pointer, bytes);
        if (status == hipSuccess && bytes != 0U) {
            for (auto& allocation : self.live_) {
                if (allocation.pointer == nullptr) {
                    allocation = {*pointer, bytes};
                    return status;
                }
            }
            self.valid = false;
            (void)flitzzip::hip_detail::native_hip_release(nullptr, *pointer);
            *pointer = nullptr;
            return hipErrorOutOfMemory;
        }
        return status;
    }
    static hipError_t release(void* context, void* pointer) noexcept {
        auto& self = *static_cast<HipFaultRuntime*>(context);
        if (self.reject(HipCall::release)) {
            return hipErrorUnknown;
        }
        for (auto& allocation : self.live_) {
            if (allocation.pointer == pointer && pointer != nullptr) {
                const auto status = flitzzip::hip_detail::native_hip_release(nullptr, pointer);
                if (status == hipSuccess) {
                    allocation = {};
                }
                return status;
            }
        }
        self.valid = false;
        return hipErrorInvalidDevicePointer;
    }
    static hipError_t copy(void* context, void* destination, const void* source, std::size_t bytes,
                           hipMemcpyKind direction, hipStream_t stream) noexcept {
        auto& self = *static_cast<HipFaultRuntime*>(context);
        if (self.reject(HipCall::copy)) {
            return hipErrorUnknown;
        }
        const bool extent_valid = direction == hipMemcpyHostToDevice   ? self.contains(destination, bytes)
                                  : direction == hipMemcpyDeviceToHost ? self.contains(source, bytes)
                                                                       : false;
        if (!extent_valid || stream != hipStreamPerThread) {
            self.valid = false;
            return hipErrorInvalidValue;
        }
        return flitzzip::hip_detail::native_hip_copy(nullptr, destination, source, bytes, direction, stream);
    }
    static hipError_t clear(void* context, void* destination, int value, std::size_t bytes,
                            hipStream_t stream) noexcept {
        auto& self = *static_cast<HipFaultRuntime*>(context);
        if (self.reject(HipCall::clear)) {
            return hipErrorUnknown;
        }
        if (!self.contains(destination, bytes) || stream != hipStreamPerThread) {
            self.valid = false;
            return hipErrorInvalidValue;
        }
        return flitzzip::hip_detail::native_hip_clear(nullptr, destination, value, bytes, stream);
    }
    static hipError_t launch(void* context, const void* kernel, dim3 grid, dim3 block, void** arguments,
                             std::size_t shared_bytes, hipStream_t stream, hipEvent_t start, hipEvent_t stop) noexcept {
        auto& self = *static_cast<HipFaultRuntime*>(context);
        if (self.reject(HipCall::launch)) {
            return hipErrorUnknown;
        }
        if (stream != hipStreamPerThread) {
            self.valid = false;
            return hipErrorInvalidValue;
        }
        return flitzzip::hip_detail::native_hip_launch(nullptr, kernel, grid, block, arguments, shared_bytes, stream,
                                                       start, stop);
    }
    static hipError_t synchronize(void* context, hipStream_t stream) noexcept {
        auto& self = *static_cast<HipFaultRuntime*>(context);
        if (self.reject(HipCall::synchronize)) {
            return hipErrorUnknown;
        }
        if (self.drain_failure_pending_) {
            self.drain_failure_pending_ = false;
            ++self.injected;
            return hipErrorInvalidValue;
        }
        if (stream != hipStreamPerThread) {
            self.valid = false;
            return hipErrorInvalidValue;
        }
        return flitzzip::hip_detail::native_hip_synchronize(nullptr, stream);
    }
};

}  // namespace flitzzip_test
