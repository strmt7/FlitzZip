#include "gpu/hip_kernel_api.hpp"

#include "core/trusted_runtime.hpp"
#include "superzip_hip_kernel_identity.hpp"

#include <algorithm>
#include <tuple>
#include <utility>
#include <windows.h>

namespace superzip {
namespace {

struct KernelModule {
    TrustedRuntimeModule owner;
    const HipKernelApi* api;
};

// Purpose: Reject absent device code before the HIP DLL can register incompatible fat binaries.
// Inputs: Live thread-local selection and targets embedded alongside this build's exact module checksum.
// Outputs: Returns the admitted device ordinal; failed runtime/property queries or unsupported targets throw.
int require_compiled_kernel_target() {
    int selected = -1;
    const auto selected_status = hipGetDevice(&selected);
    if (selected_status != hipSuccess || selected < 0) {
        throw GpuError("Unable to select a device for AMD HIP kernel compatibility: " +
                       std::string(hipGetErrorString(selected_status)));
    }
    thread_local int admitted_device = -1;
    if (admitted_device == selected) {
        return selected;
    }
    hipDeviceProp_t properties{};
    const auto properties_status = hipGetDeviceProperties(&properties, selected);
    if (properties_status != hipSuccess) {
        throw GpuError("Unable to read AMD HIP kernel target: " + std::string(hipGetErrorString(properties_status)));
    }
    const auto* end = std::find(std::begin(properties.gcnArchName), std::end(properties.gcnArchName), '\0');
    const std::string_view reported(properties.gcnArchName, static_cast<std::size_t>(end - properties.gcnArchName));
    const auto architecture = reported.substr(0U, reported.find(':'));
    if (std::ranges::find(kHipKernelArchitectures, architecture) == kHipKernelArchitectures.end()) {
        throw GpuError("AMD HIP device target " + std::string(architecture) +
                       " is unavailable on the selected device in this kernel build");
    }
    admitted_device = selected;
    return selected;
}

// Purpose: Enumerate device entrypoints once for ABI completeness and native device-code admission.
// Inputs: A checksum-verified, version-compatible dispatch table. Outputs: Typed device function pointers.
auto device_kernel_entries(const HipKernelApi& api) {
    const auto entries = std::tuple{api.materialize_prefix_segments_kernel,
                                    api.verify_analysis_candidates_kernel,
                                    api.materialize_blocks_kernel,
                                    api.materialize_segments_kernel,
                                    api.apply_sparse_patches_kernel,
                                    api.crc32_segments_kernel,
                                    api.crc32_independent_ranges_kernel,
                                    api.decoded_crc32_segments_kernel,
                                    api.diagnostic_compute_kernel,
                                    api.diagnostic_checksum_kernel,
                                    api.prefix_segment_lengths_batch_kernel,
                                    api.prefix_pack_segments_batch_kernel,
                                    api.entropy_lengths_2,
                                    api.entropy_lengths_4,
                                    api.entropy_lengths_8,
                                    api.entropy_lengths_16,
                                    api.adaptive_prefix_pack_segments_batch_kernel,
                                    api.decode_dictionary_segments,
                                    api.build_dictionary_keys,
                                    api.link_dictionary_predecessors,
                                    api.search_dictionary_matches,
                                    api.search_neutron_matches,
                                    api.encode_dictionary_deferred,
                                    api.encode_dictionary_cached,
                                    api.encode_dictionary_periodic,
                                    api.compact_dictionary_segments,
                                    api.count_sparse_positions_kernel,
                                    api.gather_sparse_positions_kernel,
                                    api.neutron_bound_match_graphs,
                                    api.neutron_initialize,
                                    api.neutron_classify_match_graphs,
                                    api.neutron_initialize_noncompetitive_graphs,
                                    api.neutron_parse_tile,
                                    api.neutron_initialize_writers,
                                    api.neutron_emit_tile,
                                    api.byte_plane_transform};
    static_assert(std::tuple_size_v<decltype(entries)> == 36U);
    return entries;
}

// Purpose: Reject an incomplete or incompatible private kernel ABI before retaining any dispatch entrypoint.
// Inputs: The table returned by the exact checksum-verified module. Outputs: Returns on completeness or throws.
void validate_kernel_api(const HipKernelApi* api) {
    if (api == nullptr || api->abi_version != 1U || api->abi_bytes != sizeof(HipKernelApi)) {
        throw GpuError("AMD HIP kernel module ABI does not match this application");
    }
    if (api->sort_dictionary_keys == nullptr ||
        !std::apply([](auto... entry) { return ((entry != nullptr) && ...); }, device_kernel_entries(*api))) {
        throw GpuError("AMD HIP kernel module ABI is incomplete");
    }
}

// Purpose: Keep the exact kernel DLL and its source locks alive only after trusted device admission.
// Inputs: This build's embedded DLL digest and fixed export name. Outputs: A process-lifetime verified owner/table.
const KernelModule& admitted_kernel_module() {
    static const KernelModule loaded = [] {
        require_hip_device_ready();
        (void)require_compiled_kernel_target();
        auto owner = [] {
            try {
                return load_trusted_app_local_runtime(L"superzip_hip_kernels.dll", kHipKernelModuleSha256);
            } catch (const Error& error) {
                throw GpuError(std::string("AMD HIP kernel admission failed: ") + error.what());
            }
        }();
        const auto entry = GetProcAddress(static_cast<HMODULE>(owner.native_handle()), "superzip_hip_kernel_api_v1");
        if (entry == nullptr) {
            throw GpuError("AMD HIP kernel module is missing its required ABI export");
        }
        using ReadApi = const HipKernelApi* (*)() noexcept;
        const auto* api = reinterpret_cast<ReadApi>(entry)();
        validate_kernel_api(api);
        return KernelModule{std::move(owner), api};
    }();
    return loaded;
}

}  // namespace

// Purpose: Return this build's admitted kernel entrypoints without reloading the module or changing ownership.
// Inputs: None; first use verifies driver/device readiness, source bytes and the private ABI.
// Outputs: Returns an immutable process-lifetime table; failed admission propagates without a CPU retry.
const HipKernelApi& hip_kernel_api() {
    const auto& api = *admitted_kernel_module().api;
    (void)require_compiled_kernel_target();
    return api;
}

// Purpose: Require executable device code for every product kernel before exposing GPU compression.
// Inputs: The calling thread's current device and this build's admitted immutable dispatch table.
// Outputs: Returns on native support; errors throw without dispatching kernels or retrying on CPU.
void require_hip_kernel_device_support() {
    require_hip_device_ready();
    const auto selected = require_compiled_kernel_target();
    const auto& api = *admitted_kernel_module().api;
    // Only immutable successful code compatibility is cached; readiness and operation errors remain live.
    thread_local int admitted_device = -1;
    if (admitted_device == selected) {
        return;
    }
    std::size_t index = 0U;
    // Purpose: Ask the HIP runtime to resolve one registered kernel for the current device without launching it.
    // Inputs: One typed module entrypoint. Outputs: Throws with its stable table index if no device code resolves.
    const auto check = [&index](auto entry) {
        hipFuncAttributes attributes{};
        const auto result = hipFuncGetAttributes(&attributes, reinterpret_cast<const void*>(entry));
        if (result != hipSuccess) {
            throw GpuError("AMD HIP kernel " + std::to_string(index) +
                           " is unavailable on the selected device: " + hipGetErrorString(result));
        }
        ++index;
    };
    std::apply([&check](auto... entry) { (check(entry), ...); }, device_kernel_entries(api));
    admitted_device = selected;
}

}  // namespace superzip
