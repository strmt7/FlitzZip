#pragma once

#include "core/archive.hpp"

#include <filesystem>

namespace superzip {

// Purpose: Extract a bounded SEA ARC/ARK archive subset with SuperZip path-safety checks.
// Inputs: `archive_path` is a SEA ARC/ARK archive, `destination` is the extraction root, `overwrite` allows existing targets only when true, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed ARC metadata, unsupported compression methods, unsafe paths, CRC mismatch, refused overwrite, or verified-file publication failures.
OperationStats extract_arc(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
