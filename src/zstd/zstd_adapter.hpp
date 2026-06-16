#pragma once

#include "core/archive.hpp"

#include <filesystem>
#include <vector>

namespace superzip {

// Purpose: Create a single-file Zstandard stream from exactly one regular file.
// Inputs: `sources` must contain one existing regular file, `output_archive` is the destination `.zst`, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws when the source set is empty, has multiple paths, or is not a regular file.
OperationStats compress_zstd(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback = {});

// Purpose: Extract a single-frame or concatenated-frame Zstandard stream to one safe output file.
// Inputs: `archive_path` is the `.zst` stream, `destination` is the extraction root, `overwrite` controls replacement, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed Zstandard data, unsafe output name, or refused overwrite.
OperationStats extract_zstd_file(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
