#include "gpu/gpu_codec.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#if SUPERZIP_ENABLE_HIP
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <shlobj.h>
#include <softpub.h>
#include <windows.h>
#include <wintrust.h>
#include <hip/hip_runtime.h>
#endif

namespace superzip {

#if SUPERZIP_ENABLE_HIP
namespace {

#ifndef SUPERZIP_HIP_RUNTIME_DLL_NAME
#define SUPERZIP_HIP_RUNTIME_DLL_NAME "amdhip64.dll"
#endif

// Purpose: Convert a UTF-8 runtime DLL name to a Windows UTF-16 string for loader APIs.
// Inputs: `text` is an ASCII-compatible DLL name from the build definition.
// Outputs: Returns a UTF-16 string with one wchar_t per byte.
std::wstring widen_ascii(const char* text) {
    std::wstring out;
    while (*text) {
        out.push_back(static_cast<unsigned char>(*text));
        ++text;
    }
    return out;
}

// Purpose: Return one Windows known-folder path without consulting mutable process environment variables.
// Inputs: `folder_id` names the system-owned folder registration.
// Outputs: Returns the absolute folder path or an empty optional when Windows cannot resolve it.
std::optional<std::filesystem::path> known_folder_path(REFKNOWNFOLDERID folder_id) {
    PWSTR raw_path = nullptr;
    if (FAILED(SHGetKnownFolderPath(folder_id, KF_FLAG_DEFAULT, nullptr, &raw_path)) || raw_path == nullptr) {
        return std::nullopt;
    }
    std::filesystem::path path(raw_path);
    CoTaskMemFree(raw_path);
    return path;
}

// Purpose: Reject a runtime path whose existing namespace contains a reparse point or non-file leaf.
// Inputs: `path` is an absolute candidate beneath a trusted Windows installation root.
// Outputs: Returns true only when every existing component is direct and the leaf is a regular file.
bool has_direct_regular_file_chain(const std::filesystem::path& path) {
    if (!path.is_absolute()) {
        return false;
    }
    auto current = path.root_path();
    for (const auto& component : path.relative_path()) {
        current /= component;
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            return false;
        }
    }
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U;
}

// Purpose: Validate the Authenticode chain for an AMD runtime candidate without prompting or network retrieval.
// Inputs: `path` is a locked absolute file path under System32 or Program Files.
// Outputs: Returns true only when Windows trusts the embedded publisher signature.
bool has_trusted_authenticode_signature(const std::filesystem::path& path) {
    WINTRUST_FILE_INFO file_info{};
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = path.c_str();

    WINTRUST_DATA trust_data{};
    trust_data.cbStruct = sizeof(trust_data);
    trust_data.dwUIChoice = WTD_UI_NONE;
    trust_data.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust_data.dwUnionChoice = WTD_CHOICE_FILE;
    trust_data.pFile = &file_info;
    trust_data.dwStateAction = WTD_STATEACTION_VERIFY;
    trust_data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto status = WinVerifyTrust(nullptr, &action, &trust_data);
    trust_data.dwStateAction = WTD_STATEACTION_CLOSE;
    (void)WinVerifyTrust(nullptr, &action, &trust_data);
    return status == ERROR_SUCCESS;
}

// Purpose: Parse a numeric ROCm installation directory for deterministic newest-first candidate ordering.
// Inputs: `name` is one directory filename such as `7.1`.
// Outputs: Returns numeric components or an empty vector for any non-version name.
std::vector<std::uint32_t> parse_version_directory(const std::wstring& name) {
    std::vector<std::uint32_t> parts;
    std::uint64_t value = 0;
    bool have_digit = false;
    for (const auto character : name) {
        if (character >= L'0' && character <= L'9') {
            have_digit = true;
            value = (value * 10U) + static_cast<std::uint32_t>(character - L'0');
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                return {};
            }
        } else if (character == L'.' && have_digit) {
            parts.push_back(static_cast<std::uint32_t>(value));
            value = 0;
            have_digit = false;
        } else {
            return {};
        }
    }
    if (!have_digit) {
        return {};
    }
    parts.push_back(static_cast<std::uint32_t>(value));
    return parts;
}

// Purpose: Enumerate only System32 and administrator-owned ROCm installation paths for the required runtime.
// Inputs: `runtime` is the exact build-bound major-version DLL filename.
// Outputs: Returns absolute candidates in trusted-preference and newest-version order.
std::vector<std::filesystem::path> trusted_hip_runtime_candidates(const std::wstring& runtime) {
    std::vector<std::filesystem::path> candidates;
    std::array<wchar_t, 32768> system_directory{};
    const auto system_length = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
    if (system_length > 0U && system_length < system_directory.size()) {
        candidates.emplace_back(std::filesystem::path(system_directory.data()) / runtime);
    }

    const auto program_files = known_folder_path(FOLDERID_ProgramFiles);
    if (!program_files) {
        return candidates;
    }
    const auto rocm_root = *program_files / L"AMD" / L"ROCm";
    std::error_code error;
    std::vector<std::pair<std::vector<std::uint32_t>, std::filesystem::path>> versions;
    for (std::filesystem::directory_iterator
             iterator(rocm_root, std::filesystem::directory_options::skip_permission_denied, error),
         end;
         !error && iterator != end; iterator.increment(error)) {
        const auto version = parse_version_directory(iterator->path().filename().wstring());
        if (!version.empty() && iterator->is_directory(error) && !error) {
            versions.emplace_back(version, iterator->path());
        }
    }
    std::ranges::sort(versions, [](const auto& left, const auto& right) { return left.first > right.first; });
    for (const auto& [version, path] : versions) {
        (void)version;
        candidates.emplace_back(path / L"bin" / runtime);
    }
    return candidates;
}

// Purpose: Load one exact trusted HIP runtime while holding its file identity against replacement.
// Inputs: `path` is an absolute System32 or Program Files candidate and `runtime` is the required basename.
// Outputs: Returns the loaded module or null after any path, signature, identity, or loader failure.
HMODULE load_trusted_hip_candidate(const std::filesystem::path& path, const std::wstring& runtime) {
    if (path.filename().wstring() != runtime || !has_direct_regular_file_chain(path)) {
        return nullptr;
    }
    const HANDLE locked_file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (locked_file == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    if (!has_trusted_authenticode_signature(path)) {
        CloseHandle(locked_file);
        return nullptr;
    }
    HMODULE module =
        LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
        CloseHandle(locked_file);
        return nullptr;
    }
    std::array<wchar_t, 32768> loaded_path{};
    const auto loaded_length = GetModuleFileNameW(module, loaded_path.data(), static_cast<DWORD>(loaded_path.size()));
    const bool exact_module = loaded_length > 0U && loaded_length < loaded_path.size() &&
                              CompareStringOrdinal(path.c_str(), -1, loaded_path.data(), -1, TRUE) == CSTR_EQUAL;
    CloseHandle(locked_file);
    if (!exact_module) {
        FreeLibrary(module);
        return nullptr;
    }
    return module;
}

// Purpose: Load the AMD HIP runtime before touching delay-loaded HIP imports.
// Inputs: None; uses the compile-time major-version DLL name recorded by CMake.
// Outputs: Returns true only when a signed runtime loads from an absolute System32 or Program Files path.
bool load_hip_runtime() {
    static const bool loaded = [] {
        const auto runtime = widen_ascii(SUPERZIP_HIP_RUNTIME_DLL_NAME);
        for (const auto& candidate : trusted_hip_runtime_candidates(runtime)) {
            if (load_trusted_hip_candidate(candidate, runtime) != nullptr) {
                return true;
            }
        }
        return false;
    }();
    return loaded;
}

}  // namespace
#endif

// Purpose: Query AMD HIP availability and selected device metadata.
// Inputs: None.
// Outputs: Returns device status; absent or unreadable devices are reported in-band rather than thrown.
GpuInfo query_hip_gpu_info() {
    GpuInfo info;
#if SUPERZIP_ENABLE_HIP
    info.hip_compiled = true;
    info.runtime_name = SUPERZIP_HIP_RUNTIME_DLL_NAME;
    if (!load_hip_runtime()) {
        info.status = "AMD HIP runtime is not loadable. Install or update the AMD GPU driver that provides " +
                      info.runtime_name + ".";
        return info;
    }
    info.hip_runtime_loadable = true;
    int count = 0;
    const auto count_status = hipGetDeviceCount(&count);
    info.device_count = count;
    if (count_status != hipSuccess || count <= 0) {
        info.status = "No AMD HIP device is available";
        return info;
    }
    int selected = 0;
    (void)hipGetDevice(&selected);
    hipDeviceProp_t props{};
    const auto props_status = hipGetDeviceProperties(&props, selected);
    if (props_status != hipSuccess) {
        info.status = "Unable to read AMD HIP device properties";
        return info;
    }
    std::size_t free_bytes = 0;
    std::size_t total_bytes = 0;
    const auto memory_status = hipMemGetInfo(&free_bytes, &total_bytes);
    if (memory_status == hipSuccess) {
        info.vram_free_bytes = static_cast<std::uint64_t>(free_bytes);
        info.vram_total_bytes = static_cast<std::uint64_t>(total_bytes);
    }
    info.available = true;
    info.selected_device = selected;
    info.device_name = props.name;
    info.gcn_arch = props.gcnArchName;
    std::ostringstream status;
    status << "AMD HIP ready: " << info.device_name << " (" << info.gcn_arch << ")";
    info.status = status.str();
#else
    info.hip_compiled = false;
    info.status = "Built without HIP acceleration";
#endif
    return info;
}

}  // namespace superzip
