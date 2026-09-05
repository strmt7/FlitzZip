#include "core/trusted_runtime.hpp"

#include "core/integrity.hpp"
#include "core/result.hpp"

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

namespace superzip {
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
// Inputs: None.
// Outputs: Returns the absolute executable directory or throws on truncation/API failure.
std::filesystem::path executable_directory() {
    std::array<wchar_t, kWindowsExtendedPathUnits> buffer{};
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0U) {
        throw ArchiveError("cannot locate SuperZip executable directory: " + windows_error_text(GetLastError()));
    }
    if (length >= buffer.size()) {
        throw ArchiveError("SuperZip executable path exceeds the Windows extended-path limit");
    }
    return std::filesystem::path(std::wstring_view(buffer.data(), length)).parent_path();
}

// Purpose: Resolve the path Windows associated with a loaded module for same-object verification.
// Inputs: `module` is a non-null module handle returned by `LoadLibraryExW`.
// Outputs: Returns the module path or throws after a loader API failure/truncation.
std::filesystem::path loaded_module_path(HMODULE module) {
    std::array<wchar_t, kWindowsExtendedPathUnits> buffer{};
    const auto length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
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
// Inputs: `dll_name` is caller-supplied compile-time runtime metadata.
// Outputs: Returns a filename path or throws when separators, roots, or dot components are present.
std::filesystem::path checked_runtime_name(std::wstring_view dll_name) {
    const std::filesystem::path name(dll_name);
    if (name.empty() || name.has_root_path() || name.has_parent_path() || name.filename() != name || name == L"." ||
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

// Purpose: Load one app-local runtime only after pinning its path and matching its build-pinned SHA-256 digest.
// Inputs: `dll_name` is a single filename and `expected_sha256` is trusted build metadata.
// Outputs: Returns an owning verified module or throws before untrusted code can execute.
TrustedRuntimeModule load_trusted_app_local_runtime(std::wstring_view dll_name, std::string_view expected_sha256) {
#if defined(_WIN32)
    const auto expected = normalized_sha256(expected_sha256);
    const auto dll_path = executable_directory() / checked_runtime_name(dll_name);
    auto source = pin_source_file(dll_path);
    const auto digest = hash_file(source.path(), IntegrityMode::Sha256);
    if (!digest.attempted || digest.hex_digest != expected) {
        throw SecurityError("bundled runtime SHA-256 verification failed: " + source.path().filename().string());
    }

    const auto module =
        LoadLibraryExW(source.path().c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
        throw ArchiveError("cannot load verified bundled runtime " + source.path().filename().string() + ": " +
                           windows_error_text(GetLastError()));
    }
    try {
        std::error_code error;
        const auto loaded_path = loaded_module_path(module);
        if (!std::filesystem::equivalent(source.path(), loaded_path, error) || error) {
            throw SecurityError("Windows loaded a different object for bundled runtime: " +
                                source.path().filename().string());
        }
        return TrustedRuntimeModule(std::move(source), module);
    } catch (...) {
        FreeLibrary(module);
        throw;
    }
#else
    (void)dll_name;
    (void)expected_sha256;
    throw ArchiveError("app-local runtime loading is available only in Windows builds");
#endif
}

}  // namespace superzip
