#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <memory>

namespace flitzzip::windows_detail {

struct HandleCloser {
    // Purpose: Release a transferred Windows kernel handle without allocating or throwing.
    // Inputs: An owned handle, nullptr or INVALID_HANDLE_VALUE; borrowed handles must not be passed.
    // Outputs: Closes each valid owned handle once when its unique owner is destroyed or reset.
    void operator()(void* handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};

// Construct the owner before a potentially allocating container call, then move the owner into the container.
// Passing a raw acquired handle to emplace_back would leave it unowned if the container allocation throws.
using HandleOwner = std::unique_ptr<void, HandleCloser>;

}  // namespace flitzzip::windows_detail
#endif
