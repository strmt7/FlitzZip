#pragma once

#include "core/progress.hpp"
#include "gpu/gpu_codec.hpp"

#include <filesystem>
#include <vector>

namespace superzip {

struct CompressOptions {
    bool gpu_required = true;
    std::uint64_t chunk_size = 16 * 1024 * 1024;
    std::uint32_t block_size = 64 * 1024;
    bool verify_after_write = true;
};

struct ExtractOptions {
    bool gpu_required = true;
    bool overwrite = false;
    std::uint32_t block_size = 64 * 1024;
};

struct OperationStats {
    std::uint64_t input_bytes = 0;
    std::uint64_t output_bytes = 0;
    std::uint64_t entries = 0;
    bool gpu_used = false;
    double seconds = 0.0;
};

// Purpose: Create a native SuperZip `.szip` archive from one or more files/directories.
// Inputs: `sources` are existing filesystem roots, `output_archive` is overwritten, `options` controls GPU/format behavior, and `progress_callback` receives snapshots from the worker path.
// Outputs: Returns archive statistics; throws `ArchiveError`, `SecurityError`, or `GpuError` on I/O, validation, or required-GPU failures.
OperationStats compress_szip(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const CompressOptions& options,
    const ProgressCallback& progress_callback = {});

// Purpose: Extract a native SuperZip `.szip` archive into a destination directory.
// Inputs: `archive_path` is the trusted archive file handle target, `destination` is the extraction root, `options` controls overwrite/GPU behavior, and `progress_callback` receives progress snapshots.
// Outputs: Returns extraction statistics; throws when archive metadata is invalid, CRC fails, path validation fails, overwrite is refused, or a required GPU is unavailable.
OperationStats extract_szip(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    const ExtractOptions& options,
    const ProgressCallback& progress_callback = {});

// Purpose: Validate a native SuperZip `.szip` archive without writing extracted files to disk.
// Inputs: `archive_path` is the archive to read, `options` controls GPU use during decode validation, and `progress_callback` receives verification progress.
// Outputs: Returns verification statistics; throws on malformed metadata, invalid block layout, decode failure, or CRC mismatch.
OperationStats verify_szip(
    const std::filesystem::path& archive_path,
    const ExtractOptions& options,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
