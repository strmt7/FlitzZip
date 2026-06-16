#pragma once

#include "core/archive.hpp"

#include <filesystem>

namespace superzip {

// Purpose: Extract the CPIO payload from an RPM package with SuperZip path-safety checks.
// Inputs: `archive_path` is an RPM package, `destination` is the extraction root, `overwrite` allows existing targets only when true, and `progress_callback` receives CPIO extraction progress snapshots.
// Outputs: Returns operation statistics; throws on malformed RPM metadata, unsupported payload compression, unsafe CPIO paths, refused overwrite, or verified-file publication failures.
OperationStats extract_rpm(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
