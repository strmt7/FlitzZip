#pragma once

#include "core/archive.hpp"

#include <filesystem>

namespace flitzzip {

// Purpose: Extract files from a standalone Windows Imaging WIM archive through the bundled wimlib runtime with FlitzZip
// path-safety checks. Inputs: `archive_path` is a `.wim` archive, `destination` is the extraction root, `overwrite`
// allows existing targets only when true, and `progress_callback` receives extraction progress snapshots. Outputs:
// Returns operation statistics; throws on unavailable wimlib runtime, split WIMs, unsafe paths, unsupported WIM entry
// kinds, resource-limit violations, refused overwrite, corrupt payloads, or verified-file publication failure.
OperationStats extract_wim(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                           bool overwrite, const ProgressCallback& progress_callback = {});

}  // namespace flitzzip
