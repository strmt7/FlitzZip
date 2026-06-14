#include "gpu/gpu_codec.hpp"

#include <sstream>

#if SUPERZIP_ENABLE_HIP
#include <hip/hip_runtime.h>
#endif

namespace superzip {

// Purpose: Query AMD HIP availability and selected device metadata.
// Inputs: None.
// Outputs: Returns device status; absent or unreadable devices are reported in-band rather than thrown.
GpuInfo query_gpu_info_cpu_only() {
    GpuInfo info;
#if SUPERZIP_ENABLE_HIP
    info.hip_compiled = true;
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
