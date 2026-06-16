#pragma once

#include "core/archive.hpp"

#include <filesystem>

namespace superzip {

// Purpose: Extract a basic ISO 9660 image with SuperZip path-safety checks.
// Inputs: `archive_path` is an ISO 9660 image, `destination` is the extraction root, `overwrite` allows existing targets only when true, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed ISO metadata, unsafe paths, unsupported interleaving/multi-extent records, refused overwrite, or verified-file publication failures.
OperationStats extract_iso(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
