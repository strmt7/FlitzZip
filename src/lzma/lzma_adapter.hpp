#pragma once

#include "core/archive.hpp"

#include <filesystem>

namespace superzip {

// Purpose: Extract a legacy LZMA-Alone `.lzma` stream with path-safe single-file publication.
// Inputs: `archive_path` is the `.lzma` stream, `destination` is the extraction root, `overwrite` controls replacement, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed LZMA data, unsafe output name, refused overwrite, resource limits, or verified-file publication failures.
OperationStats extract_lzma_file(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
