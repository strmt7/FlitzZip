#pragma once

#include <cstdint>

namespace flitzzip::runtime_detail {

// Purpose: Isolate the Windows loader operations used while validating an app-local runtime's exact identity.
// Inputs: Production uses the fixed native implementation; tests may refuse operations or vary returned paths.
// Outputs: Successful loads must return real OS module references. Ownership transfers only after identity checks.
class RuntimeLoader {
  public:
    // Purpose: Release interface storage; inputs: no owned module references; outputs: nonthrowing destruction.
    virtual ~RuntimeLoader() = default;

    // Purpose: Read an executable/module path; inputs: null selects this executable, buffer capacity is wchar_t units.
    // Outputs: Win32 length/error semantics; returns zero on failure or capacity on truncation, without throwing.
    virtual std::uint32_t module_file_name(void* module, wchar_t* buffer, std::uint32_t capacity) const noexcept = 0;

    // Purpose: Load the exact pinned file; inputs: native absolute path and restricted Windows search flags.
    // Outputs: One real owned module reference or null with GetLastError set; never substitutes another backend.
    virtual void* load_library(const wchar_t* path, std::uint32_t flags) const noexcept = 0;

    // Purpose: Release a module rejected during identity validation; inputs: the real reference just loaded.
    // Outputs: Consumes that reference exactly once without releasing its pinned source before the call returns.
    virtual void free_library(void* module) const noexcept = 0;
};

#if defined(_WIN32)
// Purpose: Provide the single Windows forwarding implementation for production and controlled boundary tests.
// Inputs: Bounded native paths, search flags and real OS module references; no mutable process-wide state.
// Outputs: Preserves native results and ownership. This internal adapter does not select another runtime backend.
class NativeRuntimeLoader final : public RuntimeLoader {
  public:
    // Purpose: Read a native module path; inputs: module and bounded buffer; outputs: exact Win32 result/error.
    std::uint32_t module_file_name(void* module, wchar_t* buffer, std::uint32_t capacity) const noexcept override;

    // Purpose: Load a pinned runtime; inputs: absolute path and fixed search flags; outputs: owned module or null.
    void* load_library(const wchar_t* path, std::uint32_t flags) const noexcept override;

    // Purpose: Release rejected module ownership; inputs: live native reference; outputs: one native release call.
    void free_library(void* module) const noexcept override;
};
#endif

}  // namespace flitzzip::runtime_detail
