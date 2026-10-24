#pragma once

#include "test_util.hpp"

namespace flitzzip_test {

class ScopedEnvironment final {
  public:
    // Purpose: Preserve one process environment value while a GUI-startup contract mutates it.
    // Inputs: `name` is a stable variable owned by the test process.
    // Outputs: Captures the prior value and restores it at destruction.
    explicit ScopedEnvironment(std::wstring name) : name_(std::move(name)) {
        wchar_t buffer[32768]{};
        const DWORD length = GetEnvironmentVariableW(name_.c_str(), buffer, 32768U);
        if (length > 0U && length < 32768U) {
            previous_.assign(buffer, length);
            existed_ = true;
        }
    }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

    // Purpose: Restore the captured environment value without leaking test state.
    // Inputs: Captured construction state.
    // Outputs: Restores or removes the variable.
    ~ScopedEnvironment() {
        SetEnvironmentVariableW(name_.c_str(), existed_ ? previous_.c_str() : nullptr);
    }

    // Purpose: Set or remove the scoped environment variable.
    // Inputs: Nullable UTF-16 value.
    // Outputs: Fails the test if Windows rejects the mutation.
    void set(const wchar_t* value) const {
        REQUIRE_TRUE(SetEnvironmentVariableW(name_.c_str(), value) != 0);
    }

  private:
    std::wstring name_;
    std::wstring previous_;
    bool existed_ = false;
};

}  // namespace flitzzip_test
