#include "gpu/hip_kernel_api.hpp"

#include "core/trusted_runtime.hpp"
#include "superzip_hip_kernel_identity.hpp"

#include <tuple>
#include <utility>
#include <windows.h>

namespace superzip {
namespace {

struct KernelModule {
    TrustedRuntimeModule owner;
    const HipKernelApi* api;
};

// Purpose: Reject an incomplete or incompatible private kernel ABI before retaining any dispatch entrypoint.
// Inputs: The table returned by the exact checksum-verified module. Outputs: Returns on completeness or throws.
void validate_kernel_api(const HipKernelApi* api) {
    if (api == nullptr || api->abi_version != 1U || api->abi_bytes != sizeof(HipKernelApi)) {
        throw GpuError("AMD HIP kernel module ABI does not match this application");
    }
    const auto entries = std::tuple{api->materialize_prefix_segments_kernel,
                                    api->verify_analysis_candidates_kernel,
                                    api->materialize_blocks_kernel,
                                    api->materialize_segments_kernel,
                                    api->apply_sparse_patches_kernel,
                                    api->crc32_segments_kernel,
                                    api->crc32_independent_ranges_kernel,
                                    api->decoded_crc32_segments_kernel,
                                    api->diagnostic_compute_kernel,
                                    api->diagnostic_checksum_kernel,
                                    api->prefix_segment_lengths_batch_kernel,
                                    api->prefix_pack_segments_batch_kernel,
                                    api->entropy_lengths_2,
                                    api->entropy_lengths_4,
                                    api->entropy_lengths_8,
                                    api->entropy_lengths_16,
                                    api->adaptive_prefix_pack_segments_batch_kernel,
                                    api->decode_dictionary_segments,
                                    api->build_dictionary_keys,
                                    api->link_dictionary_predecessors,
                                    api->search_dictionary_matches,
                                    api->search_neutron_matches,
                                    api->encode_dictionary_deferred,
                                    api->encode_dictionary_cached,
                                    api->encode_dictionary_periodic,
                                    api->compact_dictionary_segments,
                                    api->count_sparse_positions_kernel,
                                    api->gather_sparse_positions_kernel,
                                    api->neutron_bound_match_graphs,
                                    api->neutron_initialize,
                                    api->neutron_classify_match_graphs,
                                    api->neutron_initialize_noncompetitive_graphs,
                                    api->neutron_parse_tile,
                                    api->neutron_initialize_writers,
                                    api->neutron_emit_tile,
                                    api->byte_plane_transform,
                                    api->sort_dictionary_keys};
    if (!std::apply([](auto... entry) { return ((entry != nullptr) && ...); }, entries)) {
        throw GpuError("AMD HIP kernel module ABI is incomplete");
    }
}

// Purpose: Keep the exact kernel DLL and its source locks alive only after trusted device admission.
// Inputs: This build's embedded DLL digest and fixed export name. Outputs: A process-lifetime verified owner/table.
const KernelModule& admitted_kernel_module() {
    static const KernelModule loaded = [] {
        require_hip_device_ready();
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
    return *admitted_kernel_module().api;
}

}  // namespace superzip
