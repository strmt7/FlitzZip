#pragma once

#include "core/archive.hpp"

#include <filesystem>

namespace superzip {

// Purpose: Extract a concatenated `.xz` stream with path-safe single-file publication.
// Inputs: `archive_path` is the `.xz` stream, `destination` is the extraction root, `overwrite` controls replacement, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed XZ data, unsafe output name, refused overwrite, or verified-file publication failures.
OperationStats extract_xz_file(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
