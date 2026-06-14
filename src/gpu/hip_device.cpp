#include "gpu/gpu_codec.hpp"

#include <sstream>
#include <string>

#if SUPERZIP_ENABLE_HIP
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
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

// Purpose: Load the AMD HIP runtime before touching delay-loaded HIP imports.
// Inputs: None; uses the compile-time runtime DLL name recorded by CMake.
// Outputs: Returns true when Windows can load the runtime from trusted default DLL locations.
bool load_hip_runtime() {
    static const bool loaded = [] {
        const auto runtime = widen_ascii(SUPERZIP_HIP_RUNTIME_DLL_NAME);
        HMODULE existing = GetModuleHandleW(runtime.c_str());
        if (existing) {
            return true;
        }
        HMODULE module = LoadLibraryExW(runtime.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module) {
            return true;
        }
        wchar_t hip_path[MAX_PATH]{};
        constexpr DWORD hip_path_capacity = static_cast<DWORD>(sizeof(hip_path) / sizeof(hip_path[0]));
        const DWORD length = GetEnvironmentVariableW(L"HIP_PATH", hip_path, hip_path_capacity);
        if (length == 0 || length >= hip_path_capacity) {
            return false;
        }
        std::wstring explicit_runtime = hip_path;
        if (!explicit_runtime.empty() && explicit_runtime.back() != L'\\' && explicit_runtime.back() != L'/') {
            explicit_runtime.push_back(L'\\');
        }
        explicit_runtime += L"bin\\";
        explicit_runtime += runtime;
        module = LoadLibraryExW(explicit_runtime.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module != nullptr;
    }();
    return loaded;
}

}  // namespace
#endif

// Purpose: Query AMD HIP availability and selected device metadata.
// Inputs: None.
// Outputs: Returns device status; absent or unreadable devices are reported in-band rather than thrown.
GpuInfo query_gpu_info_cpu_only() {
    GpuInfo info;
#if SUPERZIP_ENABLE_HIP
    info.hip_compiled = true;
    info.runtime_name = SUPERZIP_HIP_RUNTIME_DLL_NAME;
    if (!load_hip_runtime()) {
        info.status = "AMD HIP runtime is not loadable. Install or update the AMD GPU driver that provides " + info.runtime_name + ".";
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
