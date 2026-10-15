#pragma once

#ifdef _WIN32
#include <windows.h>
#include <combaseapi.h>

namespace flitzzip {

// Owns a Windows shell string allocated by the COM task allocator, including
// failed API outputs and exceptions while converting the returned text.
class CoTaskMemWideString final {
  public:
    CoTaskMemWideString(const CoTaskMemWideString&) = delete;
    CoTaskMemWideString& operator=(const CoTaskMemWideString&) = delete;

    // Purpose: Create an empty task-allocator string owner. Inputs: None. Outputs: Null owned pointer.
    CoTaskMemWideString() = default;

    // Purpose: Release the owned shell string. Inputs: Current pointer. Outputs: Frees once; never throws.
    ~CoTaskMemWideString() noexcept {
        CoTaskMemFree(value_);
    }

    // Purpose: Prepare an API output parameter without leaking its preceding allocation.
    // Inputs: Current owner. Outputs: Releases old storage and returns a null-initialized pointer slot.
    PWSTR* put() noexcept {
        CoTaskMemFree(value_);
        value_ = nullptr;
        return &value_;
    }

    // Purpose: Borrow the shell string without transferring ownership.
    // Inputs: Current owner. Outputs: UTF-16 pointer, or null before successful allocation.
    [[nodiscard]] PWSTR get() const noexcept {
        return value_;
    }

  private:
    PWSTR value_ = nullptr;
};

}  // namespace flitzzip
#endif
