#pragma once

#include "core/archive.hpp"

#include <filesystem>

namespace superzip {

// Purpose: Extract files from a 7z archive with SuperZip path-safety checks.
// Inputs: `archive_path` is a 7z archive, `destination` is the extraction root, `overwrite` allows existing targets only when true, and `progress_callback` receives extraction progress snapshots.
// Outputs: Returns operation statistics; throws on malformed 7z metadata, unsafe paths, unsupported 7z features, refused overwrite, decode failure, or verified-file publication failure.
OperationStats extract_7z(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
