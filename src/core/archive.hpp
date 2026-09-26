#pragma once

#include "core/progress.hpp"
#include "core/resource_limits.hpp"
#include "gpu/gpu_codec.hpp"

#include <filesystem>
#include <vector>

namespace superzip {

struct CompressOptions {
    bool gpu_required = true;
    bool force_cpu = false;
    std::uint64_t chunk_size = kDefaultArchiveChunkBytes;
    std::uint32_t block_size = kDefaultArchiveBlockBytes;
    std::uint32_t worker_count = 0;
    std::uint32_t max_inflight_chunks = 0;
    int compression_level = kDefaultCompressionLevel;
    bool verify_after_write = false;
};

struct ExtractOptions {
    bool gpu_required = true;
    bool force_cpu = false;
    bool overwrite = false;
    std::uint64_t chunk_size = kDefaultArchiveChunkBytes;
    std::uint32_t block_size = kDefaultArchiveBlockBytes;
    std::uint32_t worker_count = 0;
    std::uint32_t max_inflight_chunks = 0;
};

struct OperationStats {
    std::uint64_t input_bytes = 0;
    std::uint64_t output_bytes = 0;
    std::uint64_t entries = 0;
    std::uint32_t workers = 0;
    std::uint32_t inflight_chunks = 0;
    bool gpu_used = false;
    GpuRuntimeStats gpu_runtime;
    double seconds = 0.0;
};

// Purpose: Create a native SuperZip `.suzip` archive from one or more files/directories.
// Inputs: `sources` are existing roots, `output_archive` is the final destination, and `options` selects codec policy
// and optional read-back verification. `progress_callback` observes both phases and may throw to cancel.
// Outputs: Returns archive statistics after publication; failures before publication preserve an existing destination.
OperationStats compress_suzip(const std::vector<std::filesystem::path>& sources,
                              const std::filesystem::path& output_archive, const CompressOptions& options,
                              const ProgressCallback& progress_callback = {});

// Purpose: Extract a native SuperZip `.suzip` archive into a destination directory.
// Inputs: `archive_path` is the trusted archive file handle target, `destination` is the extraction root, `options`
// controls overwrite/GPU/CPU behavior, and `progress_callback` receives progress snapshots. Outputs: Returns extraction
// statistics; throws when archive metadata is invalid, CRC fails, path validation fails, overwrite is refused, or a
// required GPU is unavailable.
OperationStats extract_suzip(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                             const ExtractOptions& options, const ProgressCallback& progress_callback = {});

// Purpose: Validate a native SuperZip `.suzip` archive without writing extracted files to disk.
// Inputs: `archive_path` is the archive to read, `options` controls decode policy, and `progress_callback` receives
// initialization, entry, and bounded decode-window snapshots. Callback exceptions cancel verification.
// Outputs: Returns verification statistics; throws on malformed metadata, invalid blocks, decode failure, or CRC
// mismatch.
OperationStats verify_suzip(const std::filesystem::path& archive_path, const ExtractOptions& options,
                            const ProgressCallback& progress_callback = {});

}  // namespace superzip
