#pragma once

#include "core/file_manifest.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

namespace flitzzip {

class TrustedRuntimeModule;
namespace runtime_detail {
class RuntimeLoader;

// Purpose: Validate one app-local runtime through an explicit Windows loader boundary.
// Inputs: A filename, trusted digest and loader that outlives this synchronous factory; successful loads are real.
// Outputs: Transfers a verified OS module to its owner, or releases the rejected module before source pins unwind.
TrustedRuntimeModule load_trusted_runtime(std::wstring_view dll_name, std::string_view expected_sha256,
                                          const RuntimeLoader& loader);
}  // namespace runtime_detail

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
    // Outputs: Returns the owned native module handle, or null after ownership has moved elsewhere.
    [[nodiscard]] void* native_handle() const noexcept {
        return module_;
    }

    // Purpose: Return the pinned absolute path used to load the verified module.
    // Inputs: This object still owns its module; do not query the path of a moved-from owner.
    // Outputs: Returns a stable path reference without transferring its source lock.
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return source_->path();
    }

  private:
    friend TrustedRuntimeModule runtime_detail::load_trusted_runtime(std::wstring_view dll_name,
                                                                     std::string_view expected_sha256,
                                                                     const runtime_detail::RuntimeLoader& loader);

    // Purpose: Bind one verified source lock to the module loaded from that exact path.
    // Inputs: `source` pins the DLL and `module` is its non-null operating-system module handle.
    // Outputs: Creates an owning module session.
    TrustedRuntimeModule(PinnedSourceFile source, void* module) : source_(std::move(source)), module_(module) {}

    std::optional<PinnedSourceFile> source_;
    void* module_ = nullptr;
};

// Purpose: Load one pinned, checksum-verified DLL from the executable directory with a restricted dependency search.
// Inputs: `dll_name` is one NUL-free filename without path or alternate-stream syntax;
// `expected_sha256` is the pinned hexadecimal digest, accepted in either ASCII letter case.
// Outputs: Returns an owning verified module or throws before any untrusted export can execute.
[[nodiscard]] TrustedRuntimeModule load_trusted_app_local_runtime(std::wstring_view dll_name,
                                                                  std::string_view expected_sha256);

}  // namespace flitzzip
