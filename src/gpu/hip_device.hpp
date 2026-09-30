#pragma once

#include "gpu/gpu_codec.hpp"

namespace superzip {

// Purpose: Validate the trusted HIP runtime and the calling thread's current device without gathering diagnostics.
// Inputs: None; preserves the thread-local device selection and does not cache device availability or free memory.
// Outputs: Returns when a current device is admitted; throws GpuError on missing HIP or runtime/device failure.
void require_hip_device_ready();

// Purpose: Query HIP availability, current device metadata, and a diagnostic memory snapshot.
// Inputs: None; preserves the calling thread's current device selection.
// Outputs: Returns diagnostic status in-band; memory values are not an allocation reservation.
GpuInfo query_hip_gpu_info();

}  // namespace superzip
