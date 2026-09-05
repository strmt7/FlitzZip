#pragma once

#include "core/file_manifest.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

namespace superzip {

// Purpose: Keep a checksum-verified app-local DLL and its parent chain pinned for the complete module lifetime.
// Inputs: Constructed only by `load_trusted_app_local_runtime` from a verified regular file and loaded module handle.
// Outputs: Releases the module before releasing the source lock; exposes only the native handle needed for ABI lookup.
class TrustedRuntimeModule final {
  public:
    TrustedRuntimeModule(const TrustedRuntimeModule&) = delete;
    TrustedRuntimeModule& operator=(const TrustedRuntimeModule&) = delete;
    TrustedRuntimeModule(TrustedRuntimeModule&& other) noexcept;
    TrustedRuntimeModule& operator=(TrustedRuntimeModule&&) = delete;

    // Purpose: Unload the runtime while its exact source file and parent chain remain pinned.
    // Inputs: None.
    // Outputs: Releases the operating-system module reference and source locks without throwing.
    ~TrustedRuntimeModule();

    // Purpose: Return the verified operating-system module handle for exact export lookup.
    // Inputs: None.
    // Outputs: Returns a non-null native module handle while this object is alive.
    [[nodiscard]] void* native_handle() const noexcept {
        return module_;
    }

    // Purpose: Return the pinned absolute path used to load the verified module.
    // Inputs: None.
    // Outputs: Returns a stable path reference without transferring its source lock.
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return source_->path();
    }

  private:
    friend TrustedRuntimeModule load_trusted_app_local_runtime(std::wstring_view dll_name,
                                                               std::string_view expected_sha256);

    // Purpose: Bind one verified source lock to the module loaded from that exact path.
    // Inputs: `source` pins the DLL and `module` is its non-null operating-system module handle.
    // Outputs: Creates an owning module session.
    TrustedRuntimeModule(PinnedSourceFile source, void* module) : source_(std::move(source)), module_(module) {}

    std::optional<PinnedSourceFile> source_;
    void* module_ = nullptr;
};

// Purpose: Load one pinned, checksum-verified DLL from the executable directory with a restricted dependency search.
// Inputs: `dll_name` is a filename without path syntax and `expected_sha256` is the pinned lowercase digest.
// Outputs: Returns an owning verified module or throws before any untrusted export can execute.
[[nodiscard]] TrustedRuntimeModule load_trusted_app_local_runtime(std::wstring_view dll_name,
                                                                  std::string_view expected_sha256);

}  // namespace superzip
