#include "gpu/hip_device.hpp"

#include "core/result.hpp"
#include "core/resource_limits.hpp"
#include "gpu/pinned_host_budget.hpp"

#include <algorithm>
#include <array>
#include <bit>
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
#include <winver.h>
#include <hip/hip_runtime.h>
#endif

namespace superzip {

namespace {
PinnedHostBudget pinned_host_budget;
std::atomic<std::uint64_t> pinned_host_release_failures{0};
}  // namespace

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

// Purpose: Read bounded numeric version metadata while the trusted runtime file is held against replacement.
// Inputs: Absolute runtime path under the loader's open file lock.
// Outputs: Returns its fixed four-component file version or unavailable metadata without guessing SDK identity.
std::optional<HipRuntimeVersion> read_locked_runtime_version(const std::filesystem::path& path) {
    DWORD ignored = 0;
    const auto size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    constexpr DWORD maximum_metadata_bytes = 64U * 1024U;
    if (size == 0U || size > maximum_metadata_bytes) {
        return std::nullopt;
    }
    std::vector<std::byte> metadata(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, metadata.data())) {
        return std::nullopt;
    }
    void* value = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(metadata.data(), L"\\", &value, &length) || value == nullptr ||
        length < sizeof(VS_FIXEDFILEINFO)) {
        return std::nullopt;
    }
    // Treat the API-returned pointer as an untrusted byte location until it is contained in the owned buffer.
    const auto begin = reinterpret_cast<std::uintptr_t>(metadata.data());
    const auto address = reinterpret_cast<std::uintptr_t>(value);
    if (address < begin || address - begin > metadata.size() ||
        sizeof(VS_FIXEDFILEINFO) > metadata.size() - (address - begin)) {
        return std::nullopt;
    }
    std::array<std::byte, sizeof(VS_FIXEDFILEINFO)> fixed_bytes{};
    std::copy_n(metadata.begin() + static_cast<std::ptrdiff_t>(address - begin), fixed_bytes.size(),
                fixed_bytes.begin());
    const auto fixed = std::bit_cast<VS_FIXEDFILEINFO>(fixed_bytes);
    if (fixed.dwSignature != VS_FFI_SIGNATURE) {
        return std::nullopt;
    }
    return HipRuntimeVersion{static_cast<std::uint16_t>(HIWORD(fixed.dwFileVersionMS)),
                             static_cast<std::uint16_t>(LOWORD(fixed.dwFileVersionMS)),
                             static_cast<std::uint16_t>(HIWORD(fixed.dwFileVersionLS)),
                             static_cast<std::uint16_t>(LOWORD(fixed.dwFileVersionLS))};
}

// Purpose: Load one exact trusted HIP runtime while holding its file identity against replacement.
// Inputs: `path` is an absolute System32 or Program Files candidate and `runtime` is the required basename.
// Outputs: Returns the module with locked file-version metadata, or null after a loader/trust failure.
HMODULE load_trusted_hip_candidate(const std::filesystem::path& path, const std::wstring& runtime,
                                   std::optional<HipRuntimeVersion>& version) {
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
    if (!exact_module) {
        CloseHandle(locked_file);
        FreeLibrary(module);
        return nullptr;
    }
    try {
        version = read_locked_runtime_version(path);
    } catch (...) {
        version = std::nullopt;
    }
    CloseHandle(locked_file);
    return module;
}

struct LoadedHipRuntime {
    HMODULE module = nullptr;
    std::optional<HipRuntimeVersion> version;
};

// Purpose: Load the AMD HIP runtime before touching delay-loaded HIP imports.
// Inputs: None; uses the compile-time major-version DLL name recorded by CMake.
// Outputs: Returns the process-lifetime trusted module and the exact version observed under its load lock.
const LoadedHipRuntime& loaded_hip_runtime() {
    static const LoadedHipRuntime loaded = [] {
        LoadedHipRuntime result;
        const auto runtime = widen_ascii(SUPERZIP_HIP_RUNTIME_DLL_NAME);
        for (const auto& candidate : trusted_hip_runtime_candidates(runtime)) {
            result.module = load_trusted_hip_candidate(candidate, runtime, result.version);
            if (result.module != nullptr) {
                return result;
            }
        }
        return result;
    }();
    return loaded;
}

// Purpose: Preserve the existing boolean admission contract over the trusted runtime's cached identity.
// Inputs: None; performs trusted loading only once per process.
// Outputs: Returns true only when the exact signed module is resident.
bool load_hip_runtime() {
    return loaded_hip_runtime().module != nullptr;
}

struct HipDeviceIdentity {
    int count;
    int selected;
};

// Purpose: Validate live device enumeration and the calling thread's selection without querying properties or VRAM.
// Inputs: The trusted HIP runtime is loaded; allow_absent permits only a successful zero-device enumeration.
// Outputs: Returns the current identity, or {0, -1} for admitted absence; other failures throw GpuError.
HipDeviceIdentity checked_hip_device_identity(bool allow_absent = false) {
    int count = 0;
    const auto count_status = hipGetDeviceCount(&count);
    if (count_status != hipSuccess) {
        throw GpuError(std::string("Unable to enumerate AMD HIP devices: ") + hipGetErrorString(count_status));
    }
    if (count == 0 && allow_absent) {
        return HipDeviceIdentity{0, -1};
    }
    if (count <= 0) {
        throw GpuError("No AMD HIP device is available");
    }
    int selected = -1;
    const auto device_status = hipGetDevice(&selected);
    if (device_status != hipSuccess) {
        throw GpuError(std::string("Unable to read the current AMD HIP device: ") + hipGetErrorString(device_status));
    }
    if (selected < 0 || selected >= count) {
        throw GpuError("The current AMD HIP device is outside the enumerated device range");
    }
    return HipDeviceIdentity{count, selected};
}

}  // namespace
#endif

// Purpose: Validate runtime and thread-local device readiness without collecting unused diagnostic metadata.
// Inputs: None; neither device selection nor allocation admission is changed.
// Outputs: Returns on readiness or throws GpuError; every subsequent HIP operation still checks its own result.
void require_hip_device_ready() {
#if SUPERZIP_ENABLE_HIP
    if (!load_hip_runtime()) {
        throw GpuError(
            std::string("AMD HIP runtime is not loadable. Install or update the AMD GPU driver that provides ") +
            SUPERZIP_HIP_RUNTIME_DLL_NAME + ".");
    }
    (void)checked_hip_device_identity();
#else
    throw GpuError("Built without HIP acceleration");
#endif
}

// Purpose: Distinguish expected HIP absence from a failure on a present runtime/device.
// Inputs: None; preserves the calling thread's selection and performs no allocation or property queries.
// Outputs: Returns false only for a missing runtime/build or zero devices; unexpected HIP errors propagate.
bool hip_device_available() {
#if SUPERZIP_ENABLE_HIP
    return load_hip_runtime() && checked_hip_device_identity(true).selected >= 0;
#else
    return false;
#endif
}

// Purpose: Admit a pinned decode owner against current host RAM and the process-wide outstanding reservation cap.
// Inputs: bytes is bounded native output; the caller's selected HIP device remains unchanged.
// Outputs: Returns pinned bytes or nullptr on unavailable resources; allocation failure returns its reservation.
std::byte* try_allocate_hip_host_output(std::size_t bytes) {
#if SUPERZIP_ENABLE_HIP
    if (bytes < kMinArchiveBlockBytes || bytes > kMaxArchiveChunkBytes || !load_hip_runtime()) {
        return nullptr;
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory) || !pinned_host_budget.try_reserve(bytes, memory.ullAvailPhys)) {
        return nullptr;
    }
    void* pointer = nullptr;
    const auto status = hipHostMalloc(&pointer, bytes, hipHostMallocPortable);
    if (status != hipSuccess) {
        pinned_host_budget.release(bytes);
        return nullptr;
    }
    return static_cast<std::byte*>(pointer);
#else
    (void)bytes;
    return nullptr;
#endif
}

// Purpose: Free host storage through its allocation context and preserve conservative accounting if HIP rejects it.
// Inputs: pointer/bytes belong to one live pinned owner with no outstanding copies or borrowed views.
// Outputs: Returns reservation bytes only on successful free; a failed release is observable but never throws.
void release_hip_host_output(std::byte* pointer, std::size_t bytes) noexcept {
#if SUPERZIP_ENABLE_HIP
    if (hipHostFree(pointer) == hipSuccess) {
        pinned_host_budget.release(bytes);
    } else {
        pinned_host_release_failures.fetch_add(1U, std::memory_order_relaxed);
    }
#else
    (void)pointer;
    (void)bytes;
#endif
}

// Purpose: Expose aggregate pin admission and cleanup state independently of GPU execution telemetry.
// Inputs: None; all counters are process-wide and concurrently updated.
// Outputs: Returns outstanding reservation bytes and cumulative failed frees, not an OS memory-use estimate.
HipPinnedHostStats snapshot_hip_pinned_host_stats() noexcept {
    return {pinned_host_budget.reserved_bytes(), pinned_host_release_failures.load(std::memory_order_relaxed)};
}

// Purpose: Size decode admission from the same live host-relative allowance used by actual pin allocations.
// Inputs: None; does not query device availability or reserve storage.
// Outputs: Returns the conservative aggregate pin capacity, or zero for unavailable memory/backend information.
std::uint64_t hip_host_output_capacity_bytes() noexcept {
#if SUPERZIP_ENABLE_HIP
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    return GlobalMemoryStatusEx(&memory) ? PinnedHostBudget::allowance(memory.ullAvailPhys) : 0U;
#else
    return 0U;
#endif
}

// Purpose: Release the operation cache after shared borrowers have returned all completed output.
// Inputs: Only idle allocations remain; destruction has exclusive ownership of the pool.
// Outputs: Frees every cached allocation through its matching HIP reservation-aware release.
HipHostOutputPool::~HipHostOutputPool() {
    for (const auto& buffer : idle_) {
        release_hip_host_output(buffer.pointer, buffer.allocation_bytes);
    }
}

// Purpose: Reuse a fitting allocation without remapping host pages, evicting unusable idle extents before new
// admission. Inputs: bytes is a positive bounded native output extent; callers return each borrowed allocation exactly
// once. Outputs: Returns uniquely borrowed pinned storage or an empty result; failed pin admission never exceeds the
// cap.
HipHostOutputBuffer HipHostOutputPool::acquire(std::size_t bytes) {
    if (bytes < kMinArchiveBlockBytes || bytes > kMaxArchiveChunkBytes) {
        return {};
    }
    std::vector<HipHostOutputBuffer> retired;
    {
        const std::lock_guard lock(mutex_);
        auto best = idle_.end();
        for (auto candidate = idle_.begin(); candidate != idle_.end(); ++candidate) {
            if (candidate->allocation_bytes >= bytes &&
                (best == idle_.end() || candidate->allocation_bytes < best->allocation_bytes)) {
                best = candidate;
            }
        }
        if (best != idle_.end()) {
            auto buffer = *best;
            idle_.erase(best);
            buffer.reused = true;
            return buffer;
        }
        retired.swap(idle_);
    }
    for (const auto& buffer : retired) {
        release_hip_host_output(buffer.pointer, buffer.allocation_bytes);
    }
    auto* pointer = try_allocate_hip_host_output(bytes);
    return pointer ? HipHostOutputBuffer{pointer, bytes, false} : HipHostOutputBuffer{};
}

// Purpose: Return completed output to its bounded operation cache without throwing from ownership cleanup.
// Inputs: pointer/bytes are one exclusively borrowed pinned allocation with no outstanding transfers or views.
// Outputs: Caches at most the native maximum queue depth; cache failure immediately uses normal HIP release.
void HipHostOutputPool::release(std::byte* pointer, std::size_t bytes) noexcept {
    try {
        const std::lock_guard lock(mutex_);
        if (idle_.size() < kMaxInflightArchiveChunks) {
            idle_.push_back({pointer, bytes, false});
            return;
        }
    } catch (...) {
        // Cache bookkeeping must not prevent reservation-aware release during stack unwinding.
    }
    release_hip_host_output(pointer, bytes);
}

// Purpose: Admit the current device's zero-retention stream allocator after exact runtime-version validation.
// Inputs: The calling thread's selected device and its current pool; neither is reconfigured.
// Outputs: Returns false for unsupported features/policies or throws GpuError for other runtime failures.
bool hip_stream_ordered_allocator_supported() {
#if SUPERZIP_ENABLE_HIP
    if (!load_hip_runtime() || !hip_runtime_allows_stream_allocations(loaded_hip_runtime().version)) {
        return false;
    }
    int device = -1;
    if (hipGetDevice(&device) != hipSuccess) {
        throw GpuError("Unable to query device for HIP stream allocation");
    }
    struct Capability {
        int device = -1;
        bool pools = false;
    };
    thread_local Capability cached;
    if (cached.device != device) {
        int supported = 0;
        const auto status = hipDeviceGetAttribute(&supported, hipDeviceAttributeMemoryPoolsSupported, device);
        if (status != hipSuccess && status != hipErrorNotSupported) {
            throw GpuError(std::string("Unable to query HIP memory pools: ") + hipGetErrorString(status));
        }
        cached = Capability{device, status == hipSuccess && supported != 0};
    }
    if (!cached.pools) {
        return false;
    }
    hipMemPool_t pool = nullptr;
    const auto pool_status = hipDeviceGetMemPool(&pool, device);
    if (pool_status == hipErrorNotSupported) {
        return false;
    }
    if (pool_status != hipSuccess) {
        throw GpuError(std::string("Unable to query current HIP pool: ") + hipGetErrorString(pool_status));
    }
    std::uint64_t threshold = 0;
    const auto attribute_status = hipMemPoolGetAttribute(pool, hipMemPoolAttrReleaseThreshold, &threshold);
    if (attribute_status == hipErrorNotSupported) {
        return false;
    }
    if (attribute_status != hipSuccess) {
        throw GpuError(std::string("Unable to query HIP pool retention: ") + hipGetErrorString(attribute_status));
    }
    return threshold == 0U;
#else
    return false;
#endif
}

// Purpose: Query AMD HIP availability and the exact loaded runtime/device metadata.
// Inputs: None; retains the current device selection and does not change pool attributes.
// Outputs: Reports feature support and pool use in-band; absent/unreadable devices remain diagnostic failures.
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
    info.runtime_version = loaded_hip_runtime().version;
    HipDeviceIdentity identity{};
    try {
        identity = checked_hip_device_identity();
    } catch (const GpuError& error) {
        info.status = error.what();
        return info;
    }
    info.device_count = identity.count;
    const auto selected = identity.selected;
    try {
        info.stream_ordered_allocator_supported = hip_stream_ordered_allocator_supported();
    } catch (const GpuError&) {
        info.stream_ordered_allocator_supported = false;
    }
    if (info.stream_ordered_allocator_supported) {
        hipMemPool_t pool = nullptr;
        std::uint64_t used = 0;
        if (hipDeviceGetMemPool(&pool, selected) == hipSuccess &&
            hipMemPoolGetAttribute(pool, hipMemPoolAttrUsedMemCurrent, &used) == hipSuccess) {
            info.pool_used_bytes = used;
        }
    }
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
    static_assert(sizeof(std::uint64_t) == sizeof(props.luid));
    const auto adapter_luid = std::bit_cast<std::uint64_t>(std::to_array(props.luid));
    if (adapter_luid != 0U) {
        info.adapter_luid = adapter_luid;
    }
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
