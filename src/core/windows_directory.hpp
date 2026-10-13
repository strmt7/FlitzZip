#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <string_view>

namespace flitzzip::windows_detail {

enum class DirectoryOpenMode { Existing, OpenOrCreate, CreateNew };

// Purpose: Open or create one directory relative to a pinned parent without reparsing a mutable path.
// Inputs: An owned parent handle, one filename component, open mode and optional creation security descriptor.
// Outputs: Returns a caller-owned handle; only CreateNew name collisions return INVALID_HANDLE_VALUE. Other
// failures and reparse points throw before any caller can write an archive payload through the result.
[[nodiscard]] HANDLE open_directory_relative(HANDLE parent, std::wstring_view component, DirectoryOpenMode mode,
                                             PSECURITY_DESCRIPTOR security_descriptor = nullptr);

}  // namespace flitzzip::windows_detail
#endif
