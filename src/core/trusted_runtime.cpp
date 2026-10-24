#include "core/trusted_runtime.hpp"
#include "core/path_text.hpp"

#include "core/integrity.hpp"
#include "core/result.hpp"
#include "core/runtime_loader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace flitzzip {
#if defined(_WIN32)
// Purpose: Read a native module path; inputs: module and bounded buffer; outputs: exact Win32 result/error.
std::uint32_t runtime_detail::NativeRuntimeLoader::module_file_name(void* module, wchar_t* buffer,
                                                                    std::uint32_t capacity) const noexcept {
    return GetModuleFileNameW(static_cast<HMODULE>(module), buffer, capacity);
}

// Purpose: Load a pinned runtime; inputs: absolute path and fixed search flags; outputs: owned module or null.
void* runtime_detail::NativeRuntimeLoader::load_library(const wchar_t* path, std::uint32_t flags) const noexcept {
    return LoadLibraryExW(path, nullptr, flags);
}

// Purpose: Release rejected module ownership; inputs: live native reference; outputs: one native release call.
void runtime_detail::NativeRuntimeLoader::free_library(void* module) const noexcept {
    static_cast<void>(FreeLibrary(static_cast<HMODULE>(module)));
}
#endif

namespace {

constexpr std::size_t kWindowsExtendedPathUnits = 32768U;

#if defined(_WIN32)
// Purpose: Format one Windows loader error without leaking unrelated process state.
// Inputs: `code` is the value captured from `GetLastError`.
// Outputs: Returns a compact deterministic diagnostic string.
std::string windows_error_text(DWORD code) {
    std::ostringstream out;
    out << "Windows error " << code;
    return out.str();
}

// Purpose: Resolve the directory containing the running executable without consulting the process current directory.
// Inputs: The explicit loader boundary; null module queries the current executable.
// Outputs: Returns the absolute executable directory or throws on truncation/API failure.
std::filesystem::path executable_directory(const runtime_detail::RuntimeLoader& loader) {
    std::array<wchar_t, kWindowsExtendedPathUnits> buffer{};
    const auto length = loader.module_file_name(nullptr, buffer.data(), static_cast<std::uint32_t>(buffer.size()));
    if (length == 0U) {
        throw ArchiveError("cannot locate FlitzZip executable directory: " + windows_error_text(GetLastError()));
    }
    if (length >= buffer.size()) {
        throw ArchiveError("FlitzZip executable path exceeds the Windows extended-path limit");
    }
    return std::filesystem::path(std::wstring_view(buffer.data(), length)).parent_path();
}

// Purpose: Resolve the path Windows associated with a loaded module for same-object verification.
// Inputs: `module` is a non-null native handle; `loader` reads the path Windows associates with that reference.
// Outputs: Returns the module path or throws after a loader API failure/truncation.
std::filesystem::path loaded_module_path(void* module, const runtime_detail::RuntimeLoader& loader) {
    std::array<wchar_t, kWindowsExtendedPathUnits> buffer{};
    const auto length = loader.module_file_name(module, buffer.data(), static_cast<std::uint32_t>(buffer.size()));
    if (length == 0U) {
        throw ArchiveError("cannot resolve loaded runtime path: " + windows_error_text(GetLastError()));
    }
    if (length >= buffer.size()) {
        throw ArchiveError("loaded runtime path exceeds the Windows extended-path limit");
    }
    return std::filesystem::path(std::wstring_view(buffer.data(), length));
}
#endif

// Purpose: Normalize and validate one pinned SHA-256 digest before comparing runtime bytes.
// Inputs: `digest` must contain exactly 64 hexadecimal ASCII characters.
// Outputs: Returns lowercase hexadecimal text or throws on malformed build metadata.
std::string normalized_sha256(std::string_view digest) {
    if (digest.size() != 64U ||
        !std::ranges::all_of(digest, [](unsigned char value) { return std::isxdigit(value) != 0; })) {
        throw ArchiveError("bundled runtime has an invalid pinned SHA-256 value");
    }
    std::string normalized(digest);
    std::ranges::transform(normalized, normalized.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return normalized;
}

// Purpose: Reject path-bearing runtime names so all resolution stays anchored beside the executable.
// Inputs: `dll_name` is caller-supplied compile-time runtime metadata, never a stream or device path.
// Outputs: Returns a filename path or throws before I/O for embedded NUL, stream syntax, roots or dot components.
std::filesystem::path checked_runtime_name(std::wstring_view dll_name) {
    const std::filesystem::path name(dll_name);
    if (dll_name.find(L'\0') != std::wstring_view::npos || dll_name.find(L':') != std::wstring_view::npos ||
        name.empty() || name.has_root_path() || name.has_parent_path() || name.filename() != name || name == L"." ||
        name == L"..") {
        throw ArchiveError("bundled runtime name must be a single filename");
    }
    return name;
}

}  // namespace

// Purpose: Transfer a verified module and its source lock without duplicating the module reference.
// Inputs: `other` is an expiring owner whose native module handle is cleared.
// Outputs: Constructs the sole remaining owner and leaves `other` safely destructible.
TrustedRuntimeModule::TrustedRuntimeModule(TrustedRuntimeModule&& other) noexcept
    : source_(std::move(other.source_)), module_(std::exchange(other.module_, nullptr)) {}

// Purpose: Unload the verified module before releasing its pinned source object and parent chain.
// Inputs: None.
// Outputs: Releases owned operating-system state without throwing.
TrustedRuntimeModule::~TrustedRuntimeModule() {
#if defined(_WIN32)
    if (module_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
#endif
}

// Purpose: Validate one pinned module through the explicit loader, retaining source ownership through rejection.
// Inputs: A single filename, trusted digest and synchronous loader whose nonnull loads are real OS references.
// Outputs: Returns an owning verified module or releases the loaded reference before propagating validation errors.
TrustedRuntimeModule runtime_detail::load_trusted_runtime(std::wstring_view dll_name, std::string_view expected_sha256,
                                                          const runtime_detail::RuntimeLoader& loader) {
#if defined(_WIN32)
    const auto expected = normalized_sha256(expected_sha256);
    const auto dll_path = executable_directory(loader) / checked_runtime_name(dll_name);
    auto source = pin_source_file(dll_path);
    const auto digest = hash_file(source.path(), IntegrityMode::Sha256);
    if (!digest.attempted || digest.hex_digest != expected) {
        throw SecurityError("bundled runtime SHA-256 verification failed: " +
                            path_diagnostic_utf8(source.path().filename()));
    }

    const auto module =
        loader.load_library(source.path().c_str(), LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
        const auto error = GetLastError();
        throw ArchiveError("cannot load verified bundled runtime " + path_diagnostic_utf8(source.path().filename()) +
                           ": " + windows_error_text(error));
    }
    try {
        std::error_code error;
        const auto loaded_path = loaded_module_path(module, loader);
        if (!std::filesystem::equivalent(source.path(), loaded_path, error) || error) {
            throw SecurityError("Windows loaded a different object for bundled runtime: " +
                                path_diagnostic_utf8(source.path().filename()));
        }
        return TrustedRuntimeModule(std::move(source), module);
    } catch (...) {
        loader.free_library(module);
        throw;
    }
#else
    (void)dll_name;
    (void)expected_sha256;
    (void)loader;
    throw ArchiveError("app-local runtime loading is available only in Windows builds");
#endif
}

// Purpose: Load one app-local runtime with the fixed production Windows implementation.
// Inputs: One filename and its trusted build digest; callers cannot supply a loader through this product interface.
// Outputs: Returns a pinned verified owner, or propagates a native admission error without retry or substitution.
TrustedRuntimeModule load_trusted_app_local_runtime(std::wstring_view dll_name, std::string_view expected_sha256) {
#if defined(_WIN32)
    const runtime_detail::NativeRuntimeLoader loader;
    return runtime_detail::load_trusted_runtime(dll_name, expected_sha256, loader);
#else
    (void)dll_name;
    (void)expected_sha256;
    throw ArchiveError("app-local runtime loading is available only in Windows builds");
#endif
}

}  // namespace flitzzip
