#pragma once

#include "core/archive.hpp"
#include "gpu/gpu_codec.hpp"

#include <cstdint>
#include <ostream>

namespace flitzzip::cli {

// Purpose: Share throughput conversion across ordinary CLI and RAM benchmark records.
// Inputs: Processed byte count and elapsed seconds. Outputs: MiB/s for positive seconds, otherwise zero.
inline double mib_per_second(std::uint64_t bytes, double seconds) {
    return seconds > 0.0 ? (static_cast<double>(bytes) / (1024.0 * 1024.0)) / seconds : 0.0;
}

// Purpose: Share complete-size and payload-size ratio calculations without changing zero-input behavior.
// Inputs: Uncompressed and compressed byte counts. Outputs: Output/input, or zero for an empty input.
inline double compression_ratio(std::uint64_t input_bytes, std::uint64_t output_bytes) {
    return input_bytes == 0 ? 0.0 : static_cast<double>(output_bytes) / static_cast<double>(input_bytes);
}

// Purpose: Keep the common GPU counter names, order and source fields identical in every CLI record.
// Inputs: Borrowed output stream, completed counters and actual GPU-use flag. Outputs: Leading-space fields only;
// no newline or timing claim is added, and the caller's stream error/exception policy remains in force.
inline void write_gpu_runtime_stats(std::ostream& output, const GpuRuntimeStats& stats, bool used) {
    output << " gpu_used=" << (used ? "true" : "false") << " gpu_encode_chunks=" << stats.encode_chunks
           << " gpu_decode_chunks=" << stats.decode_chunks << " gpu_kernel_launches=" << stats.kernel_launches
           << " gpu_kernel_ms=" << stats.kernel_ms << " gpu_h2d_bytes=" << stats.h2d_bytes
           << " gpu_d2h_bytes=" << stats.d2h_bytes << " gpu_device_allocation_bytes=" << stats.device_allocation_bytes
           << " gpu_host_pinned_allocation_bytes=" << stats.host_pinned_allocation_bytes
           << " gpu_host_pinned_output_bytes=" << stats.host_pinned_output_bytes
           << " gpu_pattern_blocks=" << stats.pattern_blocks << " gpu_prefix_blocks=" << stats.prefix_blocks
           << " gpu_dictionary_blocks=" << stats.dictionary_blocks
           << " gpu_sparse_pattern_blocks=" << stats.sparse_pattern_blocks;
}

// Purpose: Write the stable ordinary archive-operation record through the same counters as RAM benchmarks.
// Inputs: Borrowed output stream and completed operation statistics. Outputs: One newline-terminated record;
// optional adapter phases remain presence-based, and stream failures propagate under the caller's policy.
inline void print_operation_stats(std::ostream& output, const OperationStats& stats) {
    output << "entries=" << stats.entries << " input_bytes=" << stats.input_bytes
           << " output_bytes=" << stats.output_bytes << " workers=" << stats.workers
           << " inflight_chunks=" << stats.inflight_chunks;
    write_gpu_runtime_stats(output, stats.gpu_runtime, stats.gpu_used);
    output << " seconds=" << stats.seconds << " throughput_mib_s=" << mib_per_second(stats.input_bytes, stats.seconds)
           << " compression_ratio=" << compression_ratio(stats.input_bytes, stats.output_bytes);
    if (stats.phases) {
        output << " setup_seconds=" << stats.phases->setup_seconds << " stream_seconds=" << stats.phases->stream_seconds
               << " publication_seconds=" << stats.phases->publication_seconds;
    }
    output << '\n';
}

}  // namespace flitzzip::cli
