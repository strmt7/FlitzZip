#include "gpu/hip_kernel_api.hpp"

namespace flitzzip {
// Purpose: Declare the component binders used only inside this kernel module.
// Inputs: The module-owned dispatch table. Outputs: Each binder fills its own registered entrypoints.
void bind_codec_kernels(HipKernelApi& api) noexcept;
namespace hip_detail {
// Purpose: Bind static prefix kernels. Inputs: Module-owned table. Outputs: Component entrypoints.
void bind_static_prefix_kernels(HipKernelApi& api) noexcept;
// Purpose: Bind adaptive prefix kernels. Inputs: Module-owned table. Outputs: Component entrypoints.
void bind_adaptive_prefix_kernels(HipKernelApi& api) noexcept;
}  // namespace hip_detail
namespace dictionary {
// Purpose: Bind dictionary kernels. Inputs: Module-owned table. Outputs: Component entrypoints.
void bind_dictionary_kernels(HipKernelApi& api) noexcept;
}  // namespace dictionary
namespace sparse_pattern {
// Purpose: Bind sparse pattern kernels. Inputs: Module-owned table. Outputs: Component entrypoints.
void bind_sparse_kernels(HipKernelApi& api) noexcept;
}  // namespace sparse_pattern
}  // namespace flitzzip

// Purpose: Expose one versioned, immutable POD ABI after the module's HIP registration has completed.
// Inputs: None; the caller has admitted the exact module and trusted HIP runtime.
// Outputs: Returns process-lifetime typed kernel metadata; transfers no heap owner or C++ exception.
extern "C" __declspec(dllexport) const flitzzip::HipKernelApi* flitzzip_hip_kernel_api_v1() noexcept {
    static const flitzzip::HipKernelApi api = []() noexcept {
        flitzzip::HipKernelApi value;
        flitzzip::bind_codec_kernels(value);
        flitzzip::hip_detail::bind_static_prefix_kernels(value);
        flitzzip::hip_detail::bind_adaptive_prefix_kernels(value);
        flitzzip::dictionary::bind_dictionary_kernels(value);
        flitzzip::sparse_pattern::bind_sparse_kernels(value);
        return value;
    }();
    return &api;
}
