#pragma once

#include "core/archive.hpp"

#include <filesystem>

namespace superzip {

// Purpose: Extract files from a CAB archive with SuperZip path-safety checks.
// Inputs: `archive_path` is a CAB archive, `destination` is the extraction root, `overwrite` allows existing targets only when true, and `progress_callback` receives extraction progress snapshots.
// Outputs: Returns operation statistics; throws on malformed CAB metadata, unsafe paths, refused overwrite, decompression failure, or verified-file publication failure.
OperationStats extract_cab(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
