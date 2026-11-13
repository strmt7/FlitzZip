#pragma once

#include "core/rans_block.hpp"
#include "gpu/gpu_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace flitzzip::hip_detail {

// A scheduling bound, not an input limit. All admitted blocks retain support.
inline constexpr std::uint32_t kRansDecodeBatchSegments = 128U;

struct RansDecodeWorkspaceRequirements {
    std::size_t model_words;
    std::uint32_t batch_segments;
    std::size_t host_bytes;    // Caller model storage plus fixed host batch arrays.
    std::size_t device_bytes;  // Owned model/plan/status scratch; excludes borrowed input/output.
};

// Exact bounded workspace for this admitted block, with no allocation or HIP
// query. Callers account for host model storage before allocating their workspace
// and retain separate charges for resident encoded/decoded buffers and workers.
[[nodiscard]] RansDecodeWorkspaceRequirements
rans_decode_workspace_requirements(const GpuRansBlockView& block) noexcept;

// Decode an admitted block into a caller-owned resident window, without CPU
// decoding, fallback or host output staging. Device spans must be disjoint,
// exact logical extents on this thread's current device, mirroring block.payload()
// and block.decoded_size(). Owners and immutable input survive this whole call.
// Prior allocation and writes must be available on hipStreamPerThread at entry.
// Host model scratch follows prepare_models()'s extent/alias contract.
//
// Checkpoints run only before work or after actual completion. They may throw,
// but must not change device selection or mutate/retain borrowed storage. Errors
// propagate after an attempted stream drain; partial output is never success.
// Successful return includes checked scratch allocation release. The caller must still
// verify decoded CRC before publication and finish any consumer before releasing
// its output owner. This private component does not admit a new archive kind.
void decode_rans_block_device(const GpuRansBlockView& block, std::span<const std::byte> device_payload,
                              std::span<std::byte> device_output, std::span<std::uint32_t> host_models,
                              GpuTelemetry* telemetry, const std::function<void()>& checkpoint = {});

}  // namespace flitzzip::hip_detail
