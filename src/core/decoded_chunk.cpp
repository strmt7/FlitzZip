#include "core/decoded_chunk.hpp"

#include "core/checksum.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"
#include "gpu/hip_device.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace superzip {
namespace {

// Purpose: Select bounded pinned output only for HIP-capable blocks, without changing codec fallback policy.
// Inputs: size/blocks already passed output-extent admission; options select the normal CPU/HIP backend.
// Outputs: Returns uninitialized operation-owned output with matching cleanup; heap allocation may throw bad_alloc.
DecodedStorage allocate_decoded_storage(std::size_t size, std::span<const BlockDescriptor> blocks,
                                        const GpuCodecOptions& options) {
    if (!options.force_cpu) {
        const auto cpu_only = std::ranges::any_of(blocks, [](const BlockDescriptor& block) {
            return block.kind == BlockKind::Deflate || block.kind == BlockKind::CpuZstd;
        });
        if (!cpu_only) {
            if (options.host_output_pool) {
                const auto buffer = options.host_output_pool->acquire(size);
                if (buffer.pointer) {
                    return DecodedStorage(buffer.pointer,
                                          DecodedStorageDeleter{.size = buffer.allocation_bytes,
                                                                .pool = options.host_output_pool,
                                                                .new_pinned_allocation = !buffer.reused});
                }
                return DecodedStorage(new std::byte[size]);
            }
            if (auto* pinned = try_allocate_hip_host_output(size)) {
                return DecodedStorage(pinned, DecodedStorageDeleter{size, release_hip_host_output});
            }
        }
    }
    return DecodedStorage(new std::byte[size]);
}

}  // namespace

// Purpose: Release an output allocation through its matching heap, HIP, or operation-local pool owner.
// Inputs: pointer is uniquely owned; pool/release/size were established at allocation time and survive moves.
// Outputs: Returns completed pool storage or frees the allocation; never throws during destruction.
void DecodedStorageDeleter::operator()(std::byte* pointer) const noexcept {
    if (pool) {
        pool->release(pointer, size);
    } else if (release) {
        release(pointer, size);
    } else {
        delete[] pointer;
    }
}

// Purpose: Establish shared cache lifetime before required-HIP owned decoding without pinning any pages yet.
// Inputs: options select the production backend; CPU and optional fallback policies remain uncached.
// Outputs: Returns an empty or operation-owned pool; standard allocation failure propagates before any output.
std::shared_ptr<HipHostOutputPool> make_owned_decode_pool(const GpuCodecOptions& options) {
    return options.require_gpu && !options.force_cpu ? std::make_shared<HipHostOutputPool>() : nullptr;
}

// Purpose: Prevent required-HIP output workers from overwhelming the bounded pinned transfer allowance.
// Inputs: requested is positive admitted concurrency, output_bytes is its maximum window, and options select backend.
// Outputs: Keeps CPU/automatic policy unchanged; required HIP admits at least one task under its host-relative cap.
std::uint32_t resolve_owned_decode_inflight(std::uint32_t requested, std::size_t output_bytes,
                                            const GpuCodecOptions& options) noexcept {
    requested = std::max(1U, requested);
    if (!options.require_gpu || options.force_cpu || output_bytes == 0U) {
        return requested;
    }
    const auto host_slots = hip_host_output_capacity_bytes() / output_bytes;
    return static_cast<std::uint32_t>(std::max<std::uint64_t>(1U, std::min<std::uint64_t>(requested, host_slots)));
}

// Purpose: Admit exact output extents before allocating, then use the shared production decoder and checksum.
// Inputs: Borrowed encoded payload, contiguous decoded block lengths, bounded output size, and codec options.
// Outputs: Returns only fully decoded ownership; failed layout/decode releases private storage without publication.
DecodedChunk decode_owned_chunk(std::span<const std::byte> payload, std::span<const BlockDescriptor> blocks,
                                std::size_t decoded_size, const GpuCodecOptions& options) {
    if (decoded_size > kMaxArchiveChunkBytes || blocks.size() > kMaxBlocksPerEntry) {
        throw ArchiveError("decoded chunk exceeds SuperZip resource limits");
    }
    auto remaining = decoded_size;
    for (const auto& block : blocks) {
        if (block.uncompressed_len == 0U || block.uncompressed_len > kMaxArchiveBlockBytes ||
            block.uncompressed_len > remaining) {
            throw ArchiveError("decoded chunk block lengths are invalid");
        }
        remaining -= block.uncompressed_len;
    }
    if (remaining != 0U) {
        throw ArchiveError("decoded chunk block lengths do not match output size");
    }
    if (decoded_size == 0U) {
        return {};
    }
    auto* telemetry = options.telemetry.get();
    auto phase_started = telemetry ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto storage = allocate_decoded_storage(decoded_size, blocks, options);
    const std::span<std::byte> output(storage.get(), decoded_size);
    if (telemetry) {
        const auto finished = std::chrono::steady_clock::now();
        record_owned_decode_stage_time(telemetry, OwnedDecodeStage::OutputAllocation, finished - phase_started);
        phase_started = finished;
    }
    const auto gpu_used = decode_chunk(payload, blocks, output, options);
    if (telemetry) {
        const auto finished = std::chrono::steady_clock::now();
        record_owned_decode_stage_time(telemetry, OwnedDecodeStage::Materialization, finished - phase_started);
        phase_started = finished;
    }
    const auto checksum = gpu_used ? crc32_parallel(output, options.worker_count) : crc32(output);
    if (telemetry) {
        record_owned_decode_stage_time(telemetry, OwnedDecodeStage::HostChecksum,
                                       std::chrono::steady_clock::now() - phase_started);
    }
    const auto& owner = storage.get_deleter();
    if (gpu_used && (owner.pool || owner.release == release_hip_host_output)) {
        record_gpu_host_pinned_output_bytes(options.telemetry.get(), decoded_size);
        if (owner.new_pinned_allocation) {
            record_gpu_host_pinned_allocation_bytes(options.telemetry.get(), owner.size);
        }
    }
    return DecodedChunk{
        .storage = std::move(storage),
        .decoded_size = decoded_size,
        .crc32 = checksum,
        .gpu_used = gpu_used,
    };
}

}  // namespace superzip
