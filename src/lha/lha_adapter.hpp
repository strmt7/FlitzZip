#pragma once

#include "core/archive.hpp"

#include <filesystem>

namespace superzip {

// Purpose: Extract files from an LHA/LZH archive with SuperZip path-safety checks.
// Inputs: `archive_path` is an LHA/LZH archive, `destination` is the extraction root, `overwrite` allows existing targets only when true, and `progress_callback` receives extraction progress snapshots.
// Outputs: Returns operation statistics; throws on malformed LHA metadata, unsafe paths, symlink entries, resource-limit violations, refused overwrite, decode failure, CRC mismatch, or verified-file publication failure.
OperationStats extract_lha(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
