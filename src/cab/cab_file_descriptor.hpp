#pragma once

#ifdef _WIN32
#include <io.h>
#include <utility>

namespace flitzzip::detail {

// Owns one Windows CRT descriptor, including during container insertion and callback failure.
class CabFileDescriptor {
  public:
    // Purpose: Adopt one CRT descriptor without allocating memory.
    // Inputs: An owned nonnegative descriptor or -1. Outputs: A single owner responsible for closing it.
    explicit CabFileDescriptor(int descriptor = -1) noexcept : descriptor_(descriptor) {}

    CabFileDescriptor(const CabFileDescriptor&) = delete;
    CabFileDescriptor& operator=(const CabFileDescriptor&) = delete;

    // Purpose: Transfer descriptor ownership without extending its lifetime through a borrowed handle.
    // Inputs: Another owner. Outputs: Adopts its descriptor and leaves that owner empty.
    CabFileDescriptor(CabFileDescriptor&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}

    // Purpose: Replace an owned descriptor while releasing the preceding resource.
    // Inputs: Another owner, possibly this object. Outputs: One live owner; self-assignment preserves ownership.
    CabFileDescriptor& operator=(CabFileDescriptor&& other) noexcept {
        if (this != &other) {
            (void)close();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    // Purpose: Close an abandoned descriptor during normal exit or exception unwinding.
    // Inputs: None. Outputs: Releases the owned CRT resource without throwing.
    ~CabFileDescriptor() {
        (void)close();
    }

    // Purpose: Borrow the descriptor for an FDI operation without transferring ownership.
    // Inputs: None. Outputs: The live descriptor, or -1 after close or move.
    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

    // Purpose: Close at most once, including when the operating system reports a close failure.
    // Inputs: None. Outputs: CRT close status, or zero for an empty owner; ownership is always cleared first.
    int close() noexcept {
        const int descriptor = std::exchange(descriptor_, -1);
        return descriptor < 0 ? 0 : _close(descriptor);
    }

  private:
    int descriptor_ = -1;
};

}  // namespace flitzzip::detail
#endif
